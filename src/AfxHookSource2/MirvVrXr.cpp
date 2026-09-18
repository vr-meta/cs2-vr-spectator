#include "stdafx.h"

#include "MirvVrXr.h"
#include "MirvVr.h"
#include "MirvVrMath.h"
#include "MirvTime.h"

#include "WrpConsole.h"

#include "../shared/AfxConsole.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <string>
#include <vector>
#include <mutex>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// The game's D3D11 device, owned by RenderSystemDX11Hooks.cpp. The session is created on
// it, so an eye can be submitted with a plain CopyResource.
extern ID3D11Device * g_pDevice;

// For dispatching a console command from a controller button, the way the rest of the
// hook does it.
extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

// 1 CS2 unit is 1 inch; OpenXR is in metres.
static const float kUnitsPerMetre = 39.3700787f;

namespace {

HMODULE g_hLoader = nullptr;
XrInstance g_Instance = XR_NULL_HANDLE;
XrSystemId g_SystemId = XR_NULL_SYSTEM_ID;
XrSession g_Session = XR_NULL_HANDLE;
XrSpace g_Space = XR_NULL_HANDLE;
XrSessionState g_State = XR_SESSION_STATE_UNKNOWN;
bool g_SessionRunning = false;

XrSwapchain g_Swapchain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
std::vector<ID3D11Texture2D*> g_SwapchainImages[2];
uint32_t g_SwapchainWidth = 0, g_SwapchainHeight = 0;
DXGI_FORMAT g_SwapchainFormat = DXGI_FORMAT_UNKNOWN;

// Filled by xrLocateViews on the render thread, consumed by the engine thread at the top
// of the next frame. Guarded because the two threads genuinely race here.
std::mutex g_ViewMutex;
XrView g_Views[2] = {};
bool g_ViewsValid = false;

// The poses the frame was actually rendered with - last frame's, by construction. These
// are what the projection layer must report: the runtime reprojects the image from the
// pose it was drawn from to the pose the eye is in at display time, and telling it the
// wrong origin makes the world swim whenever the head turns.
XrView g_RenderedViews[2] = {};
bool g_RenderedViewsValid = false;

XrFrameState g_FrameState = { XR_TYPE_FRAME_STATE };
bool g_FrameBegun = false;
int g_EyesCopied = 0;
bool g_ReportedFirstSubmit = false;

// Submitted frames per second, measured where it matters - at xrEndFrame, not at the
// game's own frame counter, which also counts frames the headset never sees.
ULONGLONG g_FpsWindowStart = 0;
int g_FpsFrames = 0;
float g_SubmitFps = 0.0f;
bool g_LogFps = false;

// --- controller input -------------------------------------------------------------
//
// One hand each, and one idea per control. The left hand chooses who is being watched and
// where the viewer stands; the right hand controls how time runs and where the camera
// looks. The triggers move through the demo, because that is what a spectator reaches for
// most and it had no button at all until now.
//
//   left stick          walk, in the direction you are looking
//   left stick click    back onto the player, undoing the flight
//   left trigger        seek backwards
//   left grip           free look on / off
//   X                   previous player
//   Y                   next player
//
//   right stick         turn (a snap by default), and rise or descend
//   right stick click   recentre: straight ahead in the room is straight ahead in the map
//   right trigger       seek forwards
//   right grip          next camera mode
//   A                   pause / resume
//   B                   slow motion / normal speed
//
// The mapping is printed by mirv_vr_controls, because a table in a config file is no use
// with a headset on. Until #2 puts it on a quad layer that is the best available.
XrActionSet g_ActionSet = XR_NULL_HANDLE;
XrAction g_MoveAction = XR_NULL_HANDLE;
XrAction g_TurnAction = XR_NULL_HANDLE;
XrAction g_RecenterAction = XR_NULL_HANDLE;
XrAction g_FreeLookAction = XR_NULL_HANDLE;
XrAction g_ResetAction = XR_NULL_HANDLE;
XrAction g_PauseAction = XR_NULL_HANDLE;
XrAction g_SlowMoAction = XR_NULL_HANDLE;
XrAction g_NextAction = XR_NULL_HANDLE;
XrAction g_PrevAction = XR_NULL_HANDLE;
XrAction g_ModeAction = XR_NULL_HANDLE;
XrAction g_SeekForwardAction = XR_NULL_HANDLE;
XrAction g_SeekBackAction = XR_NULL_HANDLE;
bool g_ActionsAttached = false;

bool g_PrevRecenter = false, g_PrevFreeLook = false, g_PrevReset = false, g_PrevPause = false;
bool g_PrevNext = false, g_PrevPrev = false, g_PrevMode = false;
bool g_PrevSlowMo = false, g_PrevSeekForward = false, g_PrevSeekBack = false;
bool g_SlowMotion = false;
ULONGLONG g_LastInputTick = 0;

// Console commands raised by a controller button. They cannot be dispatched from the
// render thread where the input is read, so the engine thread drains this.
//
// `delay` is in engine frames. A button that maps to a game action has to be pressed and
// released on separate frames - dispatching "+attack" and "-attack" back to back in one
// frame is not a press the game notices.
struct PendingCommand { std::string cmd; int delay; };

std::mutex g_CmdMutex;
std::vector<PendingCommand> g_PendingCommands;

void QueueCommand(const char * cmd, int delayFrames = 0) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    if (g_PendingCommands.size() < 16) {
        PendingCommand p;
        p.cmd = cmd;
        p.delay = delayFrames;
        g_PendingCommands.push_back(p);
    }
}

// Seeking cannot be expressed as a fixed command string: demo_gototick takes an absolute
// tick and the current one is only knowable on the engine thread. So a trigger press
// queues the offset in seconds and the drain turns it into a tick when it runs.
//
// Coalesced rather than queued, deliberately. Holding the trigger down or tapping it
// impatiently should move the viewer further through the demo, not schedule six separate
// seeks -- and seeking repeatedly in quick succession is the shape that crashed this game
// before (docs/experiments/11-seeking.md).
float g_PendingSeekSeconds = 0.0f;

void QueueSeek(float seconds) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    g_PendingSeekSeconds += seconds;
}

// Presses and releases a game action, the way the demo's own spectator controls are
// driven. spec_next and spec_prev exist as commands but do nothing during demo playback -
// the hints on screen say MOUSE1 and SPACE for a reason.
void QueueTap(const char * action) {
    std::string down("+"); down += action;
    std::string up("-");   up   += action;
    QueueCommand(down.c_str(), 0);
    QueueCommand(up.c_str(), 2);
}

// Every number a hand touches, in one place and adjustable from the console. They were
// all guesses to begin with and none of them was ever tried against an alternative; a
// console variable is what makes trying one cost nothing.

// Units per second. A comfortable walking pace in a headset is far slower than a
// mouse-driven spectator would ever use. 1 unit is 1 inch, so 120 is about 3 m/s.
float g_MoveSpeed = 120.0f;

// Degrees per second, for smooth turning. Only used when snap turning is off.
float g_TurnSpeed = 90.0f;

// Degrees per snap. Smooth rotation the body did not ask for is the main cause of
// sickness in VR, so the default is a snap; 0 turns it off and goes back to smooth.
float g_SnapTurnDegrees = 30.0f;

float g_StickDeadzone = 0.18f;

// An exponent on the shaped stick value. Above 1 the first half of the throw moves
// slowly, which is what makes it possible to line a shot up rather than sail past it.
float g_StickCurve = 2.0f;

// Seconds per press of a trigger.
float g_SeekSeconds = 10.0f;

// Demo speed while slow motion is on.
float g_SlowMoScale = 0.25f;

AfxVrMath::SnapTurnState g_SnapTurn;

// Shape a raw stick axis: deadzone first so a resting stick is still, then the curve.
float ShapeStick(float v) {
    return AfxVrMath::ApplyResponseCurve(AfxVrMath::ApplyDeadzone(v, g_StickDeadzone), g_StickCurve);
}
XrCompositionLayerProjectionView g_ProjViews[2] = {};

PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_ = nullptr;

#define AFXVR_XR_FUNCS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetInstanceProperties) \
    X(xrGetSystem) X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) \
    X(xrResultToString) X(xrPollEvent) \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) \
    X(xrSyncActions) X(xrGetActionStateVector2f) X(xrGetActionStateBoolean)

#define AFXVR_DECL(name) PFN_##name name##_ = nullptr;
AFXVR_XR_FUNCS(AFXVR_DECL)
#undef AFXVR_DECL

PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR_ = nullptr;

const wchar_t * const kLoaderPaths[] = {
    L"D:\\Dev\\cs2-vr-tools\\openxr\\pkg\\native\\x64\\release\\bin\\openxr_loader.dll",
    L"openxr_loader.dll",
};

const char * ResultName(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString_ && g_Instance != XR_NULL_HANDLE
        && XR_SUCCEEDED(xrResultToString_(g_Instance, r, buf))) {
        return buf;
    }
    static char fallback[32];
    sprintf_s(fallback, "XrResult %i", (int)r);
    return fallback;
}

bool Check(XrResult r, const char * what) {
    if (XR_SUCCEEDED(r)) return true;
    advancedfx::Warning("AFXVR: %s failed: %s\n", what, ResultName(r));
    return false;
}

bool LoadLoader() {
    if (g_hLoader) return true;

    for (int i = 0; i < _countof(kLoaderPaths); i++) {
        g_hLoader = LoadLibraryW(kLoaderPaths[i]);
        if (g_hLoader) break;
    }
    if (!g_hLoader) {
        advancedfx::Warning("AFXVR: could not load openxr_loader.dll.\n");
        return false;
    }

    xrGetInstanceProcAddr_ = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_hLoader, "xrGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr_) {
        advancedfx::Warning("AFXVR: openxr_loader.dll has no xrGetInstanceProcAddr.\n");
        return false;
    }

    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance_);
    return nullptr != xrCreateInstance_;
}

bool LoadInstanceFunctions() {
#define AFXVR_RESOLVE(name) \
    if (XR_FAILED(xrGetInstanceProcAddr_(g_Instance, #name, (PFN_xrVoidFunction*)&name##_)) || nullptr == name##_) { \
        advancedfx::Warning("AFXVR: missing entry point %s.\n", #name); return false; }
    AFXVR_XR_FUNCS(AFXVR_RESOLVE)
#undef AFXVR_RESOLVE

    if (XR_FAILED(xrGetInstanceProcAddr_(g_Instance, "xrGetD3D11GraphicsRequirementsKHR",
            (PFN_xrVoidFunction*)&xrGetD3D11GraphicsRequirementsKHR_))) {
        advancedfx::Warning("AFXVR: runtime has no D3D11 support.\n");
        return false;
    }
    return true;
}

// OpenXR is right handed, Y up, -Z forward, metres. CS2 is Z up, X forward, inches - and
// MirvVr takes offsets in the camera's own frame, which is exactly what an eye pose is,
// so only the axis names and the scale have to change.
void XrPoseToEye(const XrPosef & pose, const XrPosef & base,
                 float & right, float & forward, float & up,
                 float & dPitch, float & dYaw, float & dRoll) {
    right   =  (pose.position.x - base.position.x) * kUnitsPerMetre;
    up      =  (pose.position.y - base.position.y) * kUnitsPerMetre;
    forward = -(pose.position.z - base.position.z) * kUnitsPerMetre;

    // Quaternion to yaw/pitch/roll, in the OpenXR frame.
    const XrQuaternionf & q = pose.orientation;
    double sinPitch = 2.0 * (q.w * q.x - q.y * q.z);
    if (sinPitch > 1.0) sinPitch = 1.0;
    if (sinPitch < -1.0) sinPitch = -1.0;

    double pitch = asin(sinPitch);
    double yaw   = atan2(2.0 * (q.w * q.y + q.z * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    double roll  = atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.x * q.x + q.z * q.z));

    const double r2d = 180.0 / M_PI;
    // Source: +pitch looks down, +yaw turns left. OpenXR: +pitch looks up, +yaw turns left.
    dPitch = (float)(-pitch * r2d);
    dYaw   = (float)( yaw   * r2d);
    dRoll  = (float)(-roll  * r2d);
}

// The widest symmetric frustum that covers the runtime's asymmetric one. CS2 can only
// express a single fov, so the eye is rendered wider than needed and the angles actually
// rendered are reported back to the compositor - correct, at the cost of edge pixels.
float SymmetricFovDegrees(const XrFovf & fov) {
    float a = fabsf(fov.angleLeft), b = fabsf(fov.angleRight);
    float half = a > b ? a : b;
    return (float)(2.0 * half * 180.0 / M_PI);
}

bool EnsureSwapchains(ID3D11Texture2D * pTexture) {
    if (g_Swapchain[0] != XR_NULL_HANDLE) return true;
    if (!pTexture) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    pTexture->GetDesc(&desc);

    // The swapchain must match the back buffer exactly, because submission is a
    // CopyResource. A mismatch would need a shader blit instead.
    uint32_t formatCount = 0;
    if (!Check(xrEnumerateSwapchainFormats_(g_Session, 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats")) return false;
    std::vector<int64_t> formats(formatCount);
    if (!Check(xrEnumerateSwapchainFormats_(g_Session, formatCount, &formatCount, formats.data()), "xrEnumerateSwapchainFormats")) return false;

    // CS2's back buffer is typeless (R8G8B8A8_TYPELESS), and no runtime offers a typeless
    // swapchain. CopyResource does allow a typeless source and a fully typed destination
    // of the same family, so pick a concrete member of that family instead - sRGB first,
    // since the game's pixels are gamma encoded and the compositor needs to know.
    DXGI_FORMAT candidates[3] = { desc.Format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        candidates[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        candidates[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        candidates[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        candidates[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    default:
        break;
    }

    DXGI_FORMAT chosen = DXGI_FORMAT_UNKNOWN;
    for (int c = 0; c < 3 && DXGI_FORMAT_UNKNOWN == chosen; c++) {
        if (DXGI_FORMAT_UNKNOWN == candidates[c]) continue;
        for (uint32_t i = 0; i < formatCount; i++) {
            if ((DXGI_FORMAT)formats[i] == candidates[c]) { chosen = candidates[c]; break; }
        }
    }

    if (DXGI_FORMAT_UNKNOWN == chosen) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            advancedfx::Warning(
                "AFXVR: the runtime accepts none of the formats compatible with the game's "
                "back buffer (%i); submission would need a shader blit. Offered:",
                (int)desc.Format);
            for (uint32_t i = 0; i < formatCount; i++) advancedfx::Warning(" %i", (int)formats[i]);
            advancedfx::Warning("\n");
        }
        return false;
    }

    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = (int64_t)chosen;
        info.sampleCount = 1;
        info.width = desc.Width;
        info.height = desc.Height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;

        if (!Check(xrCreateSwapchain_(g_Session, &info, &g_Swapchain[eye]), "xrCreateSwapchain")) return false;

        uint32_t imageCount = 0;
        if (!Check(xrEnumerateSwapchainImages_(g_Swapchain[eye], 0, &imageCount, nullptr), "xrEnumerateSwapchainImages")) return false;

        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        if (!Check(xrEnumerateSwapchainImages_(g_Swapchain[eye], imageCount, &imageCount,
                (XrSwapchainImageBaseHeader*)images.data()), "xrEnumerateSwapchainImages")) return false;

        g_SwapchainImages[eye].clear();
        for (uint32_t i = 0; i < imageCount; i++) g_SwapchainImages[eye].push_back(images[i].texture);
    }

    g_SwapchainWidth = desc.Width;
    g_SwapchainHeight = desc.Height;
    g_SwapchainFormat = chosen;

    advancedfx::Message("AFXVR: swapchains %ux%u, back buffer format %i -> swapchain %i, %u images per eye.\n",
        desc.Width, desc.Height, (int)desc.Format, (int)chosen, (unsigned)g_SwapchainImages[0].size());
    return true;
}

void DestroySwapchains() {
    for (int eye = 0; eye < 2; eye++) {
        if (g_Swapchain[eye] != XR_NULL_HANDLE && xrDestroySwapchain_) xrDestroySwapchain_(g_Swapchain[eye]);
        g_Swapchain[eye] = XR_NULL_HANDLE;
        g_SwapchainImages[eye].clear();
    }
    g_SwapchainWidth = g_SwapchainHeight = 0;
}

XrPath Path(const char * s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath_(g_Instance, s, &p);
    return p;
}

XrAction MakeAction(XrActionType type, const char * name, const char * localized) {
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    strcpy_s(info.actionName, name);
    strcpy_s(info.localizedActionName, localized);
    XrAction action = XR_NULL_HANDLE;
    if (!Check(xrCreateAction_(g_ActionSet, &info, &action), "xrCreateAction")) return XR_NULL_HANDLE;
    return action;
}

bool CreateActions() {
    if (XR_NULL_HANDLE != g_ActionSet) return true;

    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(setInfo.actionSetName, "spectator");
    strcpy_s(setInfo.localizedActionSetName, "Spectator");
    setInfo.priority = 0;
    if (!Check(xrCreateActionSet_(g_Instance, &setInfo, &g_ActionSet), "xrCreateActionSet")) return false;

    g_MoveAction        = MakeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move");
    g_TurnAction        = MakeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn and rise");
    g_RecenterAction    = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "recenter", "Recenter");
    g_FreeLookAction    = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "freelook", "Toggle free look");
    g_ResetAction       = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "reset", "Return to the demo camera");
    g_PauseAction       = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "pause", "Pause the demo");
    g_SlowMoAction      = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "slowmo", "Slow motion");
    g_NextAction        = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "nextplayer", "Next player");
    g_PrevAction        = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "prevplayer", "Previous player");
    g_ModeAction        = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "specmode", "Next camera mode");
    g_SeekForwardAction = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "seekforward", "Seek forwards");
    g_SeekBackAction    = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "seekback", "Seek backwards");

    if (!g_MoveAction || !g_TurnAction || !g_RecenterAction || !g_FreeLookAction
        || !g_ResetAction || !g_PauseAction || !g_SlowMoAction || !g_NextAction
        || !g_PrevAction || !g_ModeAction || !g_SeekForwardAction || !g_SeekBackAction) {
        return false;
    }

    // Quest 3 controllers. One idea per control, and the two things a spectator reaches
    // for most -- moving through the demo, and choosing who to watch -- on the triggers
    // and the face buttons rather than wherever there happened to be room.
    XrActionSuggestedBinding bindings[] = {
        { g_MoveAction,        Path("/user/hand/left/input/thumbstick") },
        { g_ResetAction,       Path("/user/hand/left/input/thumbstick/click") },
        { g_PrevAction,        Path("/user/hand/left/input/x/click") },
        { g_NextAction,        Path("/user/hand/left/input/y/click") },

        { g_TurnAction,        Path("/user/hand/right/input/thumbstick") },
        { g_RecenterAction,    Path("/user/hand/right/input/thumbstick/click") },
        { g_PauseAction,       Path("/user/hand/right/input/a/click") },
        { g_SlowMoAction,      Path("/user/hand/right/input/b/click") },

        // Boolean actions bound to analogue inputs; the runtime picks the threshold.
        { g_SeekBackAction,    Path("/user/hand/left/input/trigger/value") },
        { g_SeekForwardAction, Path("/user/hand/right/input/trigger/value") },
        { g_FreeLookAction,    Path("/user/hand/left/input/squeeze/value") },
        { g_ModeAction,        Path("/user/hand/right/input/squeeze/value") },
    };

    XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = _countof(bindings);
    suggested.suggestedBindings = bindings;
    if (!Check(xrSuggestInteractionProfileBindings_(g_Instance, &suggested),
               "xrSuggestInteractionProfileBindings")) return false;

    return true;
}

void PrintControls() {
    advancedfx::Message(
        "\n"
        "Controllers\n"
        "  left hand -- who you are watching, and where you are standing\n"
        "    stick            walk, in the direction you are looking\n"
        "    stick click      back onto the player\n"
        "    trigger          seek back %.0f s\n"
        "    grip             free look on / off  (currently %s)\n"
        "    X                previous player\n"
        "    Y                next player\n"
        "  right hand -- how time runs, and where the camera points\n"
        "    stick            turn%s, and rise or descend\n"
        "    stick click      recentre\n"
        "    trigger          seek forward %.0f s\n"
        "    grip             next camera mode\n"
        "    A                pause / resume\n"
        "    B                slow motion / normal speed  (currently %s)\n"
        "\n"
        "Speeds and feel: mirv_vr_speed, mirv_vr_turn, mirv_vr_stick, mirv_vr_seek.\n",
        g_SeekSeconds,
        AfxVr_GetFreeLook() ? "on" : "off",
        (0.0f < g_SnapTurnDegrees) ? " (snaps)" : " (smoothly)",
        g_SeekSeconds,
        g_SlowMotion ? "slow" : "normal");
}

bool AttachActions() {
    if (g_ActionsAttached) return true;
    if (XR_NULL_HANDLE == g_ActionSet || XR_NULL_HANDLE == g_Session) return false;

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &g_ActionSet;
    if (!Check(xrAttachSessionActionSets_(g_Session, &attach), "xrAttachSessionActionSets")) return false;

    g_ActionsAttached = true;
    advancedfx::Message("AFXVR: controllers bound. mirv_vr_controls prints the mapping.\n");
    PrintControls();
    return true;
}

bool GetVec2(XrAction action, float & x, float & y) {
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f_(g_Session, &info, &state)) || !state.isActive) return false;
    x = state.currentState.x;
    y = state.currentState.y;
    return true;
}

bool GetPressed(XrAction action) {
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    info.action = action;
    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean_(g_Session, &info, &state)) || !state.isActive) return false;
    return XR_TRUE == state.currentState;
}

void ProcessInput() {
    if (!g_ActionsAttached || XR_SESSION_STATE_FOCUSED != g_State) return;

    XrActiveActionSet active = { g_ActionSet, XR_NULL_PATH };
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions_(g_Session, &sync))) return;

    ULONGLONG now = GetTickCount64();
    float dt = g_LastInputTick ? (float)(now - g_LastInputTick) / 1000.0f : 0.0f;
    g_LastInputTick = now;
    if (dt > 0.1f) dt = 0.1f; // a hitch must not teleport the viewer

    float x = 0.0f, y = 0.0f;
    if (GetVec2(g_MoveAction, x, y)) {
        float r = ShapeStick(x), f = ShapeStick(y);
        if (r || f) AfxVr_AddMove(r * g_MoveSpeed * dt, f * g_MoveSpeed * dt, 0.0f);
    }
    if (GetVec2(g_TurnAction, x, y)) {
        if (0.0f < g_SnapTurnDegrees) {
            // Snapping reads the raw axis, not the shaped one: it is a decision, not a
            // rate, and a deadzone that has already eaten most of the throw would make
            // the threshold mean something different from what it says.
            float step = AfxVrMath::SnapTurn(g_SnapTurn, x, g_SnapTurnDegrees);
            if (step) AfxVr_AddYaw(step);
        } else {
            float t = ShapeStick(x);
            if (t) AfxVr_AddYaw(-t * g_TurnSpeed * dt); // push right, turn right
        }
        float u = ShapeStick(y);
        if (u) AfxVr_AddMove(0.0f, 0.0f, u * g_MoveSpeed * dt);
    }

    // Edge triggered, or a single press would fire for as long as it is held.
    bool b;

    b = GetPressed(g_FreeLookAction);
    if (b && !g_PrevFreeLook) { AfxVr_SetFreeLook(!AfxVr_GetFreeLook()); if (AfxVr_GetFreeLook()) AfxVr_Recenter(); }
    g_PrevFreeLook = b;

    b = GetPressed(g_RecenterAction);
    if (b && !g_PrevRecenter) AfxVr_Recenter();
    g_PrevRecenter = b;

    b = GetPressed(g_ResetAction);
    if (b && !g_PrevReset) AfxVr_ResetMove();
    g_PrevReset = b;

    b = GetPressed(g_PauseAction);
    if (b && !g_PrevPause) QueueCommand("demo_togglepause");
    g_PrevPause = b;

    b = GetPressed(g_SlowMoAction);
    if (b && !g_PrevSlowMo) {
        g_SlowMotion = !g_SlowMotion;
        char cmd[64];
        sprintf_s(cmd, "demo_timescale %f", g_SlowMotion ? g_SlowMoScale : 1.0f);
        QueueCommand(cmd);
    }
    g_PrevSlowMo = b;

    // Switching players puts the viewer back on that player rather than wherever the
    // sticks had wandered to - otherwise you follow someone from across the map.
    b = GetPressed(g_NextAction);
    if (b && !g_PrevNext) { QueueTap("attack"); AfxVr_ResetMove(); }
    g_PrevNext = b;

    b = GetPressed(g_PrevAction);
    if (b && !g_PrevPrev) { QueueTap("attack2"); AfxVr_ResetMove(); }
    g_PrevPrev = b;

    b = GetPressed(g_ModeAction);
    if (b && !g_PrevMode) QueueTap("jump"); // the demo's "next camera"
    g_PrevMode = b;

    b = GetPressed(g_SeekForwardAction);
    if (b && !g_PrevSeekForward) QueueSeek(+g_SeekSeconds);
    g_PrevSeekForward = b;

    b = GetPressed(g_SeekBackAction);
    if (b && !g_PrevSeekBack) QueueSeek(-g_SeekSeconds);
    g_PrevSeekBack = b;
}

void PollEvents() {
    if (XR_NULL_HANDLE == g_Instance) return;

    for (;;) {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
        XrResult r = xrPollEvent_(g_Instance, &ev);
        if (XR_EVENT_UNAVAILABLE == r) break;
        if (XR_FAILED(r)) break;

        if (XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED == ev.type) {
            const XrEventDataSessionStateChanged & e = *(XrEventDataSessionStateChanged*)&ev;
            g_State = e.state;
            advancedfx::Message("AFXVR: session state %i\n", (int)g_State);

            if (XR_SESSION_STATE_READY == g_State && !g_SessionRunning) {
                XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession_(g_Session, &begin), "xrBeginSession")) {
                    g_SessionRunning = true;
                    advancedfx::Message("AFXVR: session begun.\n");
                }
            } else if ((XR_SESSION_STATE_STOPPING == g_State) && g_SessionRunning) {
                g_SessionRunning = false;
                xrEndSession_(g_Session);
                advancedfx::Message("AFXVR: session ended by the runtime.\n");
            }
        } else if (XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING == ev.type) {
            advancedfx::Warning("AFXVR: the runtime is going away.\n");
            g_SessionRunning = false;
        }
    }
}

} // namespace

bool MirvVrXr_IsRunning() {
    return XR_NULL_HANDLE != g_Instance;
}

bool MirvVrXr_WantsPasses() {
    // As soon as the session is running, not once it is visible. The runtime only
    // advances a session past READY when the application starts its frame loop, so
    // waiting for SYNCHRONIZED before running it means it never starts.
    return g_SessionRunning;
}

bool MirvVrXr_Start() {
    if (MirvVrXr_IsRunning()) {
        advancedfx::Message("AFXVR: already running.\n");
        return true;
    }

    if (!LoadLoader()) return false;

    const char * extensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.enabledExtensionCount = _countof(extensions);
    createInfo.enabledExtensionNames = extensions;
    strcpy_s(createInfo.applicationInfo.applicationName, "cs2-vr-spectator");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "AfxHookSource2");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    XrResult r = xrCreateInstance_(&createInfo, &g_Instance);
    if (XR_FAILED(r)) {
        g_Instance = XR_NULL_HANDLE;
        advancedfx::Warning("AFXVR: xrCreateInstance failed (%i). Is a runtime installed and running?\n", (int)r);
        return false;
    }

    if (!LoadInstanceFunctions()) { MirvVrXr_Stop(); return false; }

    XrInstanceProperties instanceProps = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties_(g_Instance, &instanceProps))) {
        // Narrow before printing: advancedfx::Message does not take 64-bit arguments,
        // and passing them shifts the whole list.
        advancedfx::Message("AFXVR: runtime \"%s\" version %u.%u.%u\n",
            instanceProps.runtimeName,
            (unsigned int)XR_VERSION_MAJOR(instanceProps.runtimeVersion),
            (unsigned int)XR_VERSION_MINOR(instanceProps.runtimeVersion),
            (unsigned int)XR_VERSION_PATCH(instanceProps.runtimeVersion));
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem_(g_Instance, &systemInfo, &g_SystemId);
    if (XR_FAILED(r)) {
        advancedfx::Warning("AFXVR: no head mounted display available (%s). Is the headset connected?\n", ResultName(r));
        MirvVrXr_Stop();
        return false;
    }

    XrSystemProperties systemProps = { XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties_(g_Instance, g_SystemId, &systemProps))) {
        advancedfx::Message("AFXVR: system \"%s\", max swapchain %ux%u, %u layers\n",
            systemProps.systemName,
            systemProps.graphicsProperties.maxSwapchainImageWidth,
            systemProps.graphicsProperties.maxSwapchainImageHeight,
            systemProps.graphicsProperties.maxLayerCount);
    }

    uint32_t viewCount = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews_(g_Instance, g_SystemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr)) && viewCount) {
        std::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews_(g_Instance, g_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()))) {
            for (uint32_t i = 0; i < viewCount; i++) {
                advancedfx::Message("AFXVR: view %u recommended %ux%u\n",
                    i, views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight);
            }
        }
    }

    advancedfx::Message("AFXVR: instance up. Game D3D11 device: %p\n", g_pDevice);
    return true;
}

bool MirvVrXr_SessionStart() {
    if (!MirvVrXr_IsRunning() && !MirvVrXr_Start()) return false;
    if (XR_NULL_HANDLE != g_Session) {
        advancedfx::Message("AFXVR: session already created.\n");
        return true;
    }
    if (nullptr == g_pDevice) {
        advancedfx::Warning("AFXVR: the game has no D3D11 device yet. Let it render a frame first.\n");
        return false;
    }

    // Required by the spec before creating a D3D11 session, even though we are handing
    // over a device the game already made.
    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    xrGetD3D11GraphicsRequirementsKHR_(g_Instance, g_SystemId, &req);

    XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = g_pDevice;

    XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
    info.next = &binding;
    info.systemId = g_SystemId;

    if (!Check(xrCreateSession_(g_Instance, &info, &g_Session), "xrCreateSession")) {
        g_Session = XR_NULL_HANDLE;
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace_(g_Session, &spaceInfo, &g_Space), "xrCreateReferenceSpace")) {
        MirvVrXr_SessionStop();
        return false;
    }

    if (CreateActions()) AttachActions();

    advancedfx::Message("AFXVR: session created; waiting for the runtime to make it ready.\n");
    return true;
}

void MirvVrXr_SessionStop() {
    if (g_SessionRunning && xrEndSession_) { xrEndSession_(g_Session); g_SessionRunning = false; }
    DestroySwapchains();
    if (XR_NULL_HANDLE != g_Space && xrDestroySpace_) { xrDestroySpace_(g_Space); g_Space = XR_NULL_HANDLE; }
    if (XR_NULL_HANDLE != g_Session && xrDestroySession_) { xrDestroySession_(g_Session); g_Session = XR_NULL_HANDLE; }
    g_State = XR_SESSION_STATE_UNKNOWN;
    g_FrameBegun = false;
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        g_ViewsValid = false;
    }
}

void MirvVrXr_Stop() {
    MirvVrXr_SessionStop();
    if (XR_NULL_HANDLE != g_Instance && xrDestroyInstance_) xrDestroyInstance_(g_Instance);
    g_Instance = XR_NULL_HANDLE;
    g_SystemId = XR_NULL_SYSTEM_ID;
}

void MirvVrXr_EngineThread_Frame() {
    PollEvents();

    // A console command has to be dispatched from the engine thread, so the controller
    // handler only raises a flag.
    {
        std::vector<std::string> due;
        float seekSeconds = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_CmdMutex);
            for (size_t i = 0; i < g_PendingCommands.size(); ) {
                if (0 >= g_PendingCommands[i].delay) {
                    due.push_back(g_PendingCommands[i].cmd);
                    g_PendingCommands.erase(g_PendingCommands.begin() + i);
                } else {
                    g_PendingCommands[i].delay--;
                    i++;
                }
            }
            seekSeconds = g_PendingSeekSeconds;
            g_PendingSeekSeconds = 0.0f;
        }

        if (0.0f != seekSeconds) {
            int tick = 0;
            if (g_MirvTime.GetCurrentDemoTick(tick)) {
                float interval = g_MirvTime.interval_per_tick_get();
                if (interval > 0.0f) {
                    int target = tick + (int)(seekSeconds / interval);
                    // The demo starts somewhere above zero and seeking before the start
                    // is the documented way to make this crash.
                    if (target < 1) target = 1;
                    due.push_back(std::string("demo_gototick ") + std::to_string(target));
                    advancedfx::Message("AFXVR: seek %+.1fs, tick %i -> %i\n", seekSeconds, tick, target);
                } else {
                    advancedfx::Warning("AFXVR: cannot seek: the tick interval is not known yet.\n");
                }
            } else {
                advancedfx::Warning("AFXVR: cannot seek: no demo is playing.\n");
            }
        }

        for (size_t i = 0; i < due.size(); i++) {
            if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, due[i].c_str(), true);
        }
    }

    XrView views[2];
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        if (!g_ViewsValid) return;
        views[0] = g_Views[0];
        views[1] = g_Views[1];

        // Remember exactly what this frame is being rendered with, for the layer.
        g_RenderedViews[0] = views[0];
        g_RenderedViews[1] = views[1];
        g_RenderedViewsValid = true;
    }

    // The head is the midpoint between the eyes: the spectator camera stays where the
    // demo put it, and each eye is offset from it.
    XrPosef base = views[0].pose;
    base.position.x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
    base.position.y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
    base.position.z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);

    for (int eye = 0; eye < 2; eye++) {
        float right, forward, up, dPitch, dYaw, dRoll;
        XrPoseToEye(views[eye].pose, base, right, forward, up, dPitch, dYaw, dRoll);
        AfxVr_SetEye(eye + 1, true, right, forward, up, dPitch, dYaw, dRoll,
            SymmetricFovDegrees(views[eye].fov));
    }
}

void MirvVrXr_RenderThread_SubmitEye(int eyeIndex, ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture) {
    if (eyeIndex < 0 || eyeIndex > 1) return;
    if (!pContext || !pTexture) return;

    if (0 == eyeIndex) {
        if (!MirvVrXr_WantsPasses()) return;

        g_FrameState = { XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
        if (!Check(xrWaitFrame_(g_Session, &waitInfo, &g_FrameState), "xrWaitFrame")) return;

        XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
        if (!Check(xrBeginFrame_(g_Session, &beginInfo), "xrBeginFrame")) return;
        g_FrameBegun = true;

        ProcessInput();

        // Locate for this frame; the engine thread picks these up for the next one.
        XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = g_FrameState.predictedDisplayTime;
        locate.space = g_Space;

        XrViewState viewState = { XR_TYPE_VIEW_STATE };
        uint32_t got = 0;
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        if (XR_SUCCEEDED(xrLocateViews_(g_Session, &locate, &viewState, 2, &got, views)) && 2 == got
            && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
            && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            std::lock_guard<std::mutex> lock(g_ViewMutex);
            g_Views[0] = views[0];
            g_Views[1] = views[1];
            g_ViewsValid = true;
        }

        g_EyesCopied = 0;
        EnsureSwapchains(pTexture);

        // Report the poses the image was rendered from, not the ones just located. The
        // runtime then warps correctly from there to wherever the head is at display
        // time; reporting the fresh poses tells it no correction is needed, and the
        // world appears to swim as the head turns.
        XrView rendered[2];
        {
            std::lock_guard<std::mutex> lock(g_ViewMutex);
            if (g_RenderedViewsValid) { rendered[0] = g_RenderedViews[0]; rendered[1] = g_RenderedViews[1]; }
            else { rendered[0] = views[0]; rendered[1] = views[1]; }
        }

        for (int eye = 0; eye < 2; eye++) {
            g_ProjViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            g_ProjViews[eye].pose = rendered[eye].pose;

            // Report the symmetric frustum actually rendered, not the runtime's
            // asymmetric recommendation. The vertical half-angle follows from the image
            // aspect - equal to the horizontal one only when the image is square, which
            // is what the first 1080x1080 test happened to be.
            float half = 0.5f * SymmetricFovDegrees(rendered[eye].fov) * (float)(M_PI / 180.0);
            float vHalf = half;
            if (g_SwapchainWidth && g_SwapchainHeight) {
                vHalf = atanf(tanf(half) * (float)g_SwapchainHeight / (float)g_SwapchainWidth);
            }

            g_ProjViews[eye].fov.angleLeft = -half;
            g_ProjViews[eye].fov.angleRight = half;
            g_ProjViews[eye].fov.angleUp = vHalf;
            g_ProjViews[eye].fov.angleDown = -vHalf;
        }
    }

    if (!g_FrameBegun) return;

    bool canCopy = g_FrameState.shouldRender && XR_NULL_HANDLE != g_Swapchain[eyeIndex];

    if (canCopy) {
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (Check(xrAcquireSwapchainImage_(g_Swapchain[eyeIndex], &acquire, &imageIndex), "xrAcquireSwapchainImage")) {
            XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wait.timeout = XR_INFINITE_DURATION;
            if (Check(xrWaitSwapchainImage_(g_Swapchain[eyeIndex], &wait), "xrWaitSwapchainImage")) {
                pContext->CopyResource(g_SwapchainImages[eyeIndex][imageIndex], pTexture);
                g_EyesCopied++;
            }
            XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage_(g_Swapchain[eyeIndex], &release);
        }
    }

    // A begun frame must always be ended, with or without a layer. Leaving one open is
    // what stalls the runtime.
    if (1 == eyeIndex) {
        for (int eye = 0; eye < 2; eye++) {
            g_ProjViews[eye].subImage.swapchain = g_Swapchain[eye];
            g_ProjViews[eye].subImage.imageRect.offset = { 0, 0 };
            g_ProjViews[eye].subImage.imageRect.extent = { (int32_t)g_SwapchainWidth, (int32_t)g_SwapchainHeight };
            g_ProjViews[eye].subImage.imageArrayIndex = 0;
        }

        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = g_Space;
        layer.viewCount = 2;
        layer.views = g_ProjViews;

        const XrCompositionLayerBaseHeader * layers[] = { (XrCompositionLayerBaseHeader*)&layer };

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = g_FrameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = (2 == g_EyesCopied) ? 1 : 0;
        endInfo.layers = (2 == g_EyesCopied) ? layers : nullptr;

        Check(xrEndFrame_(g_Session, &endInfo), "xrEndFrame");
        g_FrameBegun = false;

        ULONGLONG now = GetTickCount64();
        if (0 == g_FpsWindowStart) g_FpsWindowStart = now;
        g_FpsFrames++;
        if (now - g_FpsWindowStart >= 2000) {
            g_SubmitFps = 1000.0f * g_FpsFrames / (float)(now - g_FpsWindowStart);
            g_FpsWindowStart = now;
            g_FpsFrames = 0;
            if (g_LogFps) {
                advancedfx::Message("AFXVR: %.1f frames/s submitted at %ux%u per eye (%.2f ms)\n",
                    g_SubmitFps, g_SwapchainWidth, g_SwapchainHeight,
                    g_SubmitFps > 0.0f ? 1000.0f / g_SubmitFps : 0.0f);
            }
        }

        if (!g_ReportedFirstSubmit && 2 == g_EyesCopied) {
            g_ReportedFirstSubmit = true;
            advancedfx::Message("AFXVR: submitting frames to the headset.\n");
        }
    }
}

CON_COMMAND(mirv_vr_xr, "cs2-vr-spectator: connect to the OpenXR runtime and submit frames.")
{
    if (2 <= args->ArgC()) {
        const char * arg1 = args->ArgV(1);
        if (!_stricmp(arg1, "info"))    { MirvVrXr_Start(); return; }
        if (!_stricmp(arg1, "start"))   { MirvVrXr_SessionStart(); return; }
        if (!_stricmp(arg1, "stop"))    { MirvVrXr_SessionStop(); AfxVr_SetEye(1,false,0,0,0,0,0,0,0); AfxVr_SetEye(2,false,0,0,0,0,0,0,0); advancedfx::Message("AFXVR: session stopped.\n"); return; }
        if (!_stricmp(arg1, "quit"))    { MirvVrXr_Stop(); advancedfx::Message("AFXVR: disconnected.\n"); return; }
        if (!_stricmp(arg1, "fps")) {
            g_LogFps = (3 <= args->ArgC()) ? (0 != atoi(args->ArgV(2))) : !g_LogFps;
            advancedfx::Message("AFXVR: frame logging %s. Last measured: %.1f frames/s at %ux%u per eye.\n",
                g_LogFps ? "on" : "off", g_SubmitFps, g_SwapchainWidth, g_SwapchainHeight);
            return;
        }
    }

    advancedfx::Message(
        "mirv_vr_xr info  - connect and report what the runtime is, nothing more.\n"
        "mirv_vr_xr start - create the session and start sending frames to the headset.\n"
        "mirv_vr_xr stop  - stop sending frames, keep the connection.\n"
        "mirv_vr_xr quit  - disconnect entirely.\n"
        "mirv_vr_xr fps [0|1] - log submitted frames per second.\n"
        "\n"
        "Instance: %s, session: %s, state %i, submitting: %s\n"
        "Last measured: %.1f frames/s at %ux%u per eye.\n",
        MirvVrXr_IsRunning() ? "up" : "down",
        (XR_NULL_HANDLE != g_Session) ? "created" : "none",
        (int)g_State,
        MirvVrXr_WantsPasses() ? "yes" : "no",
        g_SubmitFps, g_SwapchainWidth, g_SwapchainHeight);
}

// --- the numbers a hand can feel ---------------------------------------------------
//
// Every one of these started as a guess and none had ever been compared against an
// alternative. Making them console variables is not polish; it is the difference between
// "this feels wrong" and an answer, when the only instrument is a person wearing the
// headset and the only way to test is to change it and look again.

CON_COMMAND(mirv_vr_controls, "cs2-vr-spectator: print the controller mapping.")
{
    PrintControls();
}

CON_COMMAND(mirv_vr_speed, "cs2-vr-spectator: how fast the sticks move the viewer, in units per second.")
{
    if (2 <= args->ArgC()) {
        g_MoveSpeed = (float)atof(args->ArgV(1));
        advancedfx::Message("mirv_vr_speed: %.1f units/s\n", g_MoveSpeed);
        return;
    }
    advancedfx::Message(
        "mirv_vr_speed <units per second> - how fast the left stick flies the viewer.\n"
        "1 unit is 1 inch, so 120 is a brisk walk and 400 is uncomfortable in a headset.\n"
        "Current value: %.1f\n", g_MoveSpeed);
}

CON_COMMAND(mirv_vr_turn, "cs2-vr-spectator: snap or smooth turning, and how fast.")
{
    if (2 <= args->ArgC()) {
        const char * arg1 = args->ArgV(1);
        if (!_stricmp(arg1, "smooth")) {
            g_SnapTurnDegrees = 0.0f;
            if (3 <= args->ArgC()) g_TurnSpeed = (float)atof(args->ArgV(2));
            advancedfx::Message("mirv_vr_turn: smooth, %.0f deg/s\n", g_TurnSpeed);
            return;
        }
        if (!_stricmp(arg1, "snap")) {
            g_SnapTurnDegrees = (3 <= args->ArgC()) ? (float)atof(args->ArgV(2)) : 30.0f;
            if (g_SnapTurnDegrees <= 0.0f) g_SnapTurnDegrees = 30.0f;
            advancedfx::Message("mirv_vr_turn: snapping %.0f degrees\n", g_SnapTurnDegrees);
            return;
        }
    }
    advancedfx::Message(
        "mirv_vr_turn snap [degrees]  - turn in steps (the default, 30).\n"
        "mirv_vr_turn smooth [deg/s]  - turn continuously.\n"
        "\n"
        "Smooth rotation that the body did not ask for is the main cause of sickness in\n"
        "VR: the eyes report turning and the inner ear does not agree. A snap gives it\n"
        "nothing to disagree with. Smooth is here because some people prefer it and\n"
        "nobody had tried either.\n"
        "Current: %s\n",
        (0.0f < g_SnapTurnDegrees) ? "snap" : "smooth");
}

CON_COMMAND(mirv_vr_stick, "cs2-vr-spectator: stick deadzone and response curve.")
{
    if (2 <= args->ArgC()) {
        g_StickDeadzone = (float)atof(args->ArgV(1));
        if (g_StickDeadzone < 0.0f) g_StickDeadzone = 0.0f;
        if (g_StickDeadzone > 0.9f) g_StickDeadzone = 0.9f;
        if (3 <= args->ArgC()) g_StickCurve = (float)atof(args->ArgV(2));
        advancedfx::Message("mirv_vr_stick: deadzone %.2f, curve %.2f\n", g_StickDeadzone, g_StickCurve);
        return;
    }
    advancedfx::Message(
        "mirv_vr_stick <deadzone> [curve] - how the sticks are shaped.\n"
        "\n"
        "Deadzone is the fraction of the throw that reads as nothing; a stick that does\n"
        "not sit perfectly centred needs one, and the rest of the throw is rescaled so\n"
        "full deflection still means full speed.\n"
        "Curve is an exponent: 1 is linear, 2 makes half a push a quarter of the speed,\n"
        "which is what makes fine positioning possible without losing the top end.\n"
        "Current: deadzone %.2f, curve %.2f\n",
        g_StickDeadzone, g_StickCurve);
}

CON_COMMAND(mirv_vr_seek, "cs2-vr-spectator: how far the triggers move through the demo, and seek now.")
{
    int argc = args->ArgC();

    if (3 <= argc && !_stricmp(args->ArgV(1), "now")) {
        // The same path a trigger takes, reachable from a key bind. Without this the
        // seeking code could only be exercised with a headset on, which is a poor way to
        // find out whether demo_gototick survives being used.
        QueueSeek((float)atof(args->ArgV(2)));
        return;
    }

    if (2 <= argc && !_stricmp(args->ArgV(1), "where")) {
        int tick = 0;
        if (g_MirvTime.GetCurrentDemoTick(tick)) {
            float interval = g_MirvTime.interval_per_tick_get();
            advancedfx::Message("mirv_vr_seek: tick %i, %.2f s in, %.4f s per tick\n",
                tick, interval > 0.0f ? tick * interval : 0.0f, interval);
        } else {
            advancedfx::Message("mirv_vr_seek: no demo is playing.\n");
        }
        return;
    }

    if (2 <= argc) {
        g_SeekSeconds = (float)atof(args->ArgV(1));
        if (g_SeekSeconds < 0.0f) g_SeekSeconds = -g_SeekSeconds;
        advancedfx::Message("mirv_vr_seek: %.1f seconds per press\n", g_SeekSeconds);
        return;
    }

    advancedfx::Message(
        "mirv_vr_seek <seconds>      - how far one trigger press moves through the demo.\n"
        "mirv_vr_seek now <seconds>  - seek by that much right now (negative goes back).\n"
        "mirv_vr_seek where          - report the current tick and the tick interval.\n"
        "\n"
        "Left trigger goes back, right goes forward. Presses inside one frame are added\n"
        "together rather than queued, so an impatient hand does not schedule six seeks.\n"
        "Current value: %.1f\n", g_SeekSeconds);
}

CON_COMMAND(mirv_vr_slowmo, "cs2-vr-spectator: the demo speed the B button switches to.")
{
    if (2 <= args->ArgC()) {
        g_SlowMoScale = (float)atof(args->ArgV(1));
        if (g_SlowMoScale <= 0.0f) g_SlowMoScale = 0.25f;
        advancedfx::Message("mirv_vr_slowmo: %.2fx\n", g_SlowMoScale);
        return;
    }
    advancedfx::Message(
        "mirv_vr_slowmo <scale> - the demo_timescale the B button toggles to.\n"
        "Current value: %.2f (currently %s)\n",
        g_SlowMoScale, g_SlowMotion ? "slow" : "normal");
}
