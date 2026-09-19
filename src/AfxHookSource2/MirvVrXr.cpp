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

// 0 and 1 are the eyes. 2 is the panel: the main pass's image, which still has the HUD and
// the demo menu composited into it, carried as a flat quad in space rather than smeared
// across both eyes at screen depth. See MirvVrXr_RenderThread_SubmitPanel.
const int kSwapchainCount = 3;
const int kPanelSwapchain = 2;

XrSwapchain g_Swapchain[kSwapchainCount] = { XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE };
std::vector<ID3D11Texture2D*> g_SwapchainImages[kSwapchainCount];
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

// Where xrWaitFrame and xrLocateViews happen.
//
// Originally both were on the render thread, inside the first eye's submission, because
// keeping xrBeginFrame and xrEndFrame together is the simple thing to do. The cost is a
// frame of latency by construction: the engine thread has already set this frame's eye
// poses by the time the render thread locates the head, so every frame renders with the
// previous frame's poses. That is issue #8, and at 21-26 ms a frame it is not small.
//
// The canonical OpenXR arrangement is the split one: xrWaitFrame on the thread that paces
// the application, xrBeginFrame and xrEndFrame on the thread that renders. The spec
// supports it explicitly - xrWaitFrame may run in parallel with the other two. Doing it
// that way lets the poses be located at the top of the frame they belong to.
//
// Off by default until it has been worn. The two must stay matched one to one, which is
// what g_FrameWaited is for: a frame the render thread never picks up must not cause a
// second wait.
bool g_LowLatency = false;
bool g_FrameWaited = false;

// Whether g_ProjViews describes anything. False before the first successful locate, and
// the frame is then ended with no layer rather than with a made-up one.
bool g_ProjViewsValid = false;

// Exchange which located view drives which render pass, and which swapchain each is
// submitted to. There is no reason this should ever be needed - view 0 is the left eye by
// specification - but "the image will not fuse" has exactly two plausible causes and this
// tells them apart in one keypress instead of an argument.
bool g_SwapEyes = false;

// Both eyes from the head's midpoint, zero separation. The two images are then identical
// by construction, so they must fuse - and if they still do not, the fault is not in the
// stereo at all but in how the frames are submitted or paired. That is a fork worth one
// keypress, because the two halves need completely different work.
bool g_Monoscopic = false;

// Multiplies the eye offsets. The runtime reports a true interpupillary distance and we
// convert it honestly, so this should be 1 - but "the eyes diverge too strongly" is a
// complaint about exactly this number, and being able to wind it down until the world
// fuses turns an argument into a measurement.
float g_IpdScale = 1.0f;

// Calibration mode. The person who can see the problem is wearing a headset and cannot
// reach the keyboard, so the controllers have to do it. While this is on, the face buttons
// and grips adjust the numbers instead of their usual jobs; the sticks still fly.
bool g_Calibrating = false;
int g_EyesCopied = 0;
bool g_ReportedFirstSubmit = false;

// Submitted frames per second, measured where it matters - at xrEndFrame, not at the
// game's own frame counter, which also counts frames the headset never sees.
ULONGLONG g_FpsWindowStart = 0;
int g_FpsFrames = 0;
float g_SubmitFps = 0.0f;
bool g_LogFps = false;

// --- frame time sampling ------------------------------------------------------------
//
// An average tells you almost nothing about whether a headset is comfortable. What matters
// is the shape of the distribution: a run that averages 60 but drops one frame in twenty
// feels worse than a steady 50, and a mean hides that completely. So this keeps the
// samples and reports percentiles.
//
// Measured on the engine thread, around the whole frame, so it counts everything the game
// does and not only what the hook can see. It works with no session and no headset, which
// is the point: the graphics settings can then be compared on a desk instead of on a face.
const int kFrameSampleCapacity = 16384;
double g_FrameSamplesMs[kFrameSampleCapacity];
int g_FrameSampleCount = 0;
bool g_SamplingFrames = false;
LARGE_INTEGER g_PerfFrequency = {};
LARGE_INTEGER g_LastFrameStamp = {};
double g_SampleUntilSeconds = 0.0;
double g_SampledSeconds = 0.0;
char g_SampleLabel[64] = "";

int CompareDouble(const void * a, const void * b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

double Percentile(const double * sorted, int count, double fraction) {
    if (count <= 0) return 0.0;
    int index = (int)(fraction * (count - 1) + 0.5);
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return sorted[index];
}

void ReportFrameSamples() {
    if (g_FrameSampleCount < 2) {
        advancedfx::Message("AFXVR: not enough frames sampled.\n");
        return;
    }

    double total = 0.0;
    for (int i = 0; i < g_FrameSampleCount; i++) total += g_FrameSamplesMs[i];
    double mean = total / g_FrameSampleCount;

    qsort(g_FrameSamplesMs, g_FrameSampleCount, sizeof(double), CompareDouble);

    double p50 = Percentile(g_FrameSamplesMs, g_FrameSampleCount, 0.50);
    double p95 = Percentile(g_FrameSamplesMs, g_FrameSampleCount, 0.95);
    double p99 = Percentile(g_FrameSamplesMs, g_FrameSampleCount, 0.99);
    double worst = g_FrameSamplesMs[g_FrameSampleCount - 1];

    advancedfx::Message(
        "AFXVR frametime%s%s: %i frames over %.1f s\n"
        "  mean %.2f ms (%.1f fps)   median %.2f ms (%.1f fps)\n"
        "  p95  %.2f ms   p99 %.2f ms   worst %.2f ms\n",
        g_SampleLabel[0] ? " " : "", g_SampleLabel,
        g_FrameSampleCount, g_SampledSeconds,
        mean, mean > 0.0 ? 1000.0 / mean : 0.0,
        p50, p50 > 0.0 ? 1000.0 / p50 : 0.0,
        p95, p99, worst);
}

// --- where a submitted frame goes ----------------------------------------------------
//
// Experiment 13 established that three scene traversals are about 11 ms of a 21-26 ms VR
// frame, so more than half the budget is the submission path and the runtime -- and
// nothing measures that half. These do.
//
// xrWaitFrame is the one to watch. It is where the runtime paces the application, so time
// spent in it is not necessarily time wasted: a fast application waits there on purpose.
// Time in the copies and in xrEndFrame is, though.
struct StageTimer {
    double totalMs = 0.0;
    double worstMs = 0.0;
    int count = 0;

    void Add(double ms) {
        totalMs += ms;
        if (ms > worstMs) worstMs = ms;
        count++;
    }
    void Reset() { totalMs = 0.0; worstMs = 0.0; count = 0; }
    double Mean() const { return count ? totalMs / count : 0.0; }
};

StageTimer g_StageWaitFrame, g_StageLocate, g_StageCopy, g_StageEndFrame;

double SecondsSince(const LARGE_INTEGER & start) {
    if (0 == g_PerfFrequency.QuadPart) QueryPerformanceFrequency(&g_PerfFrequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)g_PerfFrequency.QuadPart;
}

LARGE_INTEGER StageStart() {
    if (0 == g_PerfFrequency.QuadPart) QueryPerformanceFrequency(&g_PerfFrequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now;
}

void SampleFrameTime() {
    if (!g_SamplingFrames) return;

    if (0 == g_PerfFrequency.QuadPart) QueryPerformanceFrequency(&g_PerfFrequency);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (0 != g_LastFrameStamp.QuadPart) {
        double ms = 1000.0 * (double)(now.QuadPart - g_LastFrameStamp.QuadPart)
                  / (double)g_PerfFrequency.QuadPart;
        g_SampledSeconds += ms / 1000.0;
        if (g_FrameSampleCount < kFrameSampleCapacity) {
            g_FrameSamplesMs[g_FrameSampleCount++] = ms;
        }
        if (g_SampledSeconds >= g_SampleUntilSeconds || g_FrameSampleCount >= kFrameSampleCapacity) {
            g_SamplingFrames = false;
            ReportFrameSamples();
        }
    }
    g_LastFrameStamp = now;
}

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

// Render the eye passes with no session and no headset. Nothing reaches a runtime; the
// point is that the pass loop, the per-pass camera and the UI compositing order can all
// be watched from a desk, which is the only way most of this project can be debugged
// without a Quest on someone's head.
int g_ForcedPasses = 0;

// See MirvVrXr.h. Off by default: it removes the demo menu from the headset entirely, and
// until there is a quad layer to put it back on, a wrongly-placed menu beats no menu.
bool g_CaptureBeforeUi = false;

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

// --- the panel ------------------------------------------------------------------------
//
// The demo's timeline, scoreboard and speed controls are a flat Panorama overlay drawn at
// screen depth. Copied into each eye that is doubled, placed at the wrong distance, and
// unreadable; issue #2. A quad layer is what a flat panel wants to be in a headset.
//
// It carries the main pass, which is the one image that still has the UI in it once
// `mirv_vr_xr ui out` has taken it out of the eyes. So the two switches are meant to be
// used together: clean world in stereo, menu on a panel.
//
// World-locked rather than head-locked. Head-locked is easier and worse: a panel that
// follows the eyes cannot be looked away from, and looking away from the menu is most of
// what a viewer does. It is placed in front of wherever the viewer was when it was turned
// on, and stays there until it is placed again.
bool g_PanelEnabled = false;
bool g_PanelCopied = false;
bool g_PanelPlaced = false;
XrPosef g_PanelPose = {};
float g_PanelWidthMetres = 1.6f;
float g_PanelDistanceMetres = 1.8f;

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

// Tried in order. AFXVR_OPENXR_LOADER comes first so an installation that is not this
// machine's has somewhere to say so; the absolute path is this machine's and the bare name
// is the last resort, which finds one only if it happens to sit next to the game.
//
// Note this is the *loader*, not the runtime. Which runtime the loader then picks is the
// registry's business, or XR_RUNTIME_JSON's -- see scripts/openxr-runtime.ps1.
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

    wchar_t fromEnvironment[MAX_PATH];
    if (0 < GetEnvironmentVariableW(L"AFXVR_OPENXR_LOADER", fromEnvironment, MAX_PATH)) {
        g_hLoader = LoadLibraryW(fromEnvironment);
        if (!g_hLoader) {
            advancedfx::Warning("AFXVR: AFXVR_OPENXR_LOADER is set to \"%ls\" but it would not load.\n",
                fromEnvironment);
        }
    }

    for (int i = 0; !g_hLoader && i < _countof(kLoaderPaths); i++) {
        g_hLoader = LoadLibraryW(kLoaderPaths[i]);
    }
    if (!g_hLoader) {
        advancedfx::Warning(
            "AFXVR: could not load openxr_loader.dll. Set AFXVR_OPENXR_LOADER to its full\n"
            "AFXVR: path before launching, or see docs/install.md.\n");
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

// Overrides for the frustum, because the automatic answer is only right if CS2 renders
// exactly the field of view it is handed - and on a 2528x2780 window, which is portrait
// and nothing like the 4:3 the engine's fov convention is written around, that is an
// assumption rather than a fact.
//
// 0 means automatic. Whatever comes out of here is used for BOTH what the game renders and
// what the projection layer claims - those two must never disagree, which is the whole
// reason this is one function and not two numbers.
float g_FovOverrideDegrees = 0.0f;
float g_FovScale = 1.0f;
float g_FovVerticalOverrideDegrees = 0.0f;

// What the projection layer CLAIMS, when it must differ from what we asked the game for.
//
// Normally these are the same number and that is the whole point - claim what you drew.
// But if the engine does not render the angle it is handed, they are not the same, and
// the only way to find the true relationship is to hold one fixed and move the other
// until the world looks right. Whatever value fuses tells us what the engine actually
// rendered. 0 means "the same as we asked for".
float g_ReportedFovOverrideDegrees = 0.0f;

// Source does not treat `fov` as "the horizontal angle I will render". It treats it as the
// horizontal angle *at 4:3*, derives the vertical from that, and then recomputes the
// horizontal for the window's real aspect. On a 16:9 monitor the difference is small
// enough to ignore. On a 2528x2780 window - portrait, aspect 0.909 - asking for 108
// degrees gets about 86 rendered.
//
// That gap is invisible while the reported interpupillary distance is small, because the
// disparity it corrupts is proportional to it. SteamVR reports 22 mm here and it looked
// almost right; Meta's runtime reports the true 61 mm and the two eyes stop fusing.
//
// So: given the frustum we actually want, work out what to ask Source for.
bool g_SourceAspectFix = false;

float AspectOfImage() {
    if (g_SwapchainWidth && g_SwapchainHeight) {
        return (float)g_SwapchainWidth / (float)g_SwapchainHeight;
    }
    return 1.0f;
}

// The engine's chain, forwards: what it actually renders when handed `askedDegrees`.
float RenderedFovForAsked(float askedDegrees) {
    const double d2r = M_PI / 180.0, r2d = 180.0 / M_PI;
    double aspect = AspectOfImage();
    if (aspect <= 0.0) return askedDegrees;
    double halfY = atan(tan(0.5 * askedDegrees * d2r) * 0.75);
    double halfX = atan(tan(halfY) * aspect);
    return (float)(2.0 * halfX * r2d);
}

// The angle to hand the engine so that it renders `wantedDegrees` horizontally.
float SourceFovForWanted(float wantedDegrees) {
    const double d2r = M_PI / 180.0, r2d = 180.0 / M_PI;
    double halfX = 0.5 * wantedDegrees * d2r;
    double aspect = AspectOfImage();
    if (aspect <= 0.0) return wantedDegrees;

    // Undo the engine's chain: vertical from the wanted horizontal at this aspect, then
    // the 4:3 horizontal that would have produced that vertical.
    double halfY = atan(tan(halfX) / aspect);
    double half43 = atan(tan(halfY) / 0.75);
    return (float)(2.0 * half43 * r2d);
}

// What the frustum should be: the smallest symmetric one containing the runtime's
// asymmetric recommendation, unless overridden.
float WantedFovDegrees(const XrFovf & fov) {
    float degrees = (g_FovOverrideDegrees > 0.0f) ? g_FovOverrideDegrees : SymmetricFovDegrees(fov);
    degrees *= g_FovScale;
    if (degrees < 10.0f) degrees = 10.0f;
    if (degrees > 170.0f) degrees = 170.0f;
    return degrees;
}

// What to hand the engine. Differs from the above only when the aspect fix is on.
float EffectiveFovDegrees(const XrFovf & fov) {
    float wanted = WantedFovDegrees(fov);
    if (!g_SourceAspectFix) return wanted;

    float asked = SourceFovForWanted(wanted);
    if (asked < 10.0f) asked = 10.0f;
    if (asked > 178.0f) asked = 178.0f;
    return asked;
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

    for (int eye = 0; eye < kSwapchainCount; eye++) {
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

    advancedfx::Message("AFXVR: swapchains %ux%u, back buffer format %i -> swapchain %i, %u images each (two eyes and a panel).\n",
        desc.Width, desc.Height, (int)desc.Format, (int)chosen, (unsigned)g_SwapchainImages[0].size());
    return true;
}

void DestroySwapchains() {
    for (int eye = 0; eye < kSwapchainCount; eye++) {
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

    if (g_Calibrating) {
        // The face buttons and grips are the dials while this is on. Everything they
        // normally do is unreachable, which is fine: nobody calibrates and spectates at
        // the same moment, and the sticks still fly.
        bool changed = false;

        // Geometric, not additive. A fixed step is either too coarse to settle on a value
        // or too fine to reach one - the first version stepped by 0.1 and ran into its own
        // limits before it reached anything interesting. Multiplying gets to a twentieth
        // or to five times in about seven presses each, while still being fine near 1.
        const float kStep = 1.25f;

        b = GetPressed(g_PrevAction);   // X
        if (b && !g_PrevPrev) { g_IpdScale /= kStep; changed = true; }
        g_PrevPrev = b;

        b = GetPressed(g_NextAction);   // Y
        if (b && !g_PrevNext) { g_IpdScale *= kStep; changed = true; }
        g_PrevNext = b;

        // Wide enough not to be in the way. The far ends are absurd for a real headset,
        // which is the point: if the image only fuses at a twentieth of the reported
        // separation, that is a finding, not a setting.
        if (g_IpdScale < 0.02f) g_IpdScale = 0.02f;
        if (g_IpdScale > 50.0f) g_IpdScale = 50.0f;

        b = GetPressed(g_PauseAction);  // A
        if (b && !g_PrevPause) {
            float base = (g_ReportedFovOverrideDegrees > 0.0f) ? g_ReportedFovOverrideDegrees : 108.0f;
            g_ReportedFovOverrideDegrees = base - 8.0f;
            changed = true;
        }
        g_PrevPause = b;

        b = GetPressed(g_SlowMoAction); // B
        if (b && !g_PrevSlowMo) {
            float base = (g_ReportedFovOverrideDegrees > 0.0f) ? g_ReportedFovOverrideDegrees : 108.0f;
            g_ReportedFovOverrideDegrees = base + 8.0f;
            changed = true;
        }
        g_PrevSlowMo = b;

        if (g_ReportedFovOverrideDegrees > 0.0f) {
            if (g_ReportedFovOverrideDegrees < 15.0f) g_ReportedFovOverrideDegrees = 15.0f;
            if (g_ReportedFovOverrideDegrees > 179.0f) g_ReportedFovOverrideDegrees = 179.0f;
        }

        b = GetPressed(g_FreeLookAction); // left grip
        if (b && !g_PrevFreeLook) { g_Monoscopic = !g_Monoscopic; changed = true; }
        g_PrevFreeLook = b;

        b = GetPressed(g_ModeAction);   // right grip
        if (b && !g_PrevMode) {
            g_IpdScale = 1.0f;
            g_ReportedFovOverrideDegrees = 0.0f;
            g_Monoscopic = false;
            changed = true;
        }
        g_PrevMode = b;

        if (changed) {
            advancedfx::Message(
                "AFXVR calibrate: %s | separation x%.2f | claiming %.0f deg\n",
                g_Monoscopic ? "MONO" : "stereo",
                g_IpdScale,
                g_ReportedFovOverrideDegrees > 0.0f ? g_ReportedFovOverrideDegrees : 0.0f);
        }
        return;
    }

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

// Put the panel in front of a head pose, upright, at the configured distance. Only the
// yaw is taken: a panel that inherits the pitch and roll of whatever angle someone happened
// to be looking at when they pressed the button is a panel nobody can read.
void PlacePanelFrom(const XrPosef & head) {
    const XrQuaternionf & q = head.orientation;
    double yaw = atan2(2.0 * (q.w * q.y + q.z * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));

    // OpenXR looks down -Z, so forward is (-sin yaw, 0, -cos yaw).
    float fx = -(float)sin(yaw);
    float fz = -(float)cos(yaw);

    g_PanelPose.position.x = head.position.x + fx * g_PanelDistanceMetres;
    g_PanelPose.position.y = head.position.y;
    g_PanelPose.position.z = head.position.z + fz * g_PanelDistanceMetres;

    g_PanelPose.orientation.x = 0.0f;
    g_PanelPose.orientation.y = (float)sin(yaw * 0.5);
    g_PanelPose.orientation.z = 0.0f;
    g_PanelPose.orientation.w = (float)cos(yaw * 0.5);

    g_PanelPlaced = true;
}

// Locate both eyes for a display time and publish them. Shared by the two threading
// arrangements, so they cannot drift apart.
void LocateViews(XrTime displayTime) {
    XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = displayTime;
    locate.space = g_Space;

    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    uint32_t got = 0;
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

    LARGE_INTEGER locateStart = StageStart();
    XrResult r = xrLocateViews_(g_Session, &locate, &viewState, 2, &got, views);
    g_StageLocate.Add(1000.0 * SecondsSince(locateStart));

    if (XR_SUCCEEDED(r) && 2 == got
        && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
        && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        g_Views[0] = views[0];
        g_Views[1] = views[1];
        g_ViewsValid = true;
    }
}

// The low-latency half of the split: wait for the runtime's pacing and locate the head at
// the top of the frame that is about to be drawn, rather than a frame behind.
void EngineThread_WaitAndLocate() {
    if (!g_LowLatency) return;
    if (!g_SessionRunning || XR_NULL_HANDLE == g_Session || !xrWaitFrame_) return;

    // One wait per begin, always. A frame the render thread never picked up has a wait
    // outstanding; waiting again would hand the runtime two for one frame.
    if (g_FrameWaited) return;

    g_FrameState = { XR_TYPE_FRAME_STATE };
    XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    LARGE_INTEGER waitStart = StageStart();
    if (!Check(xrWaitFrame_(g_Session, &waitInfo, &g_FrameState), "xrWaitFrame")) return;
    g_StageWaitFrame.Add(1000.0 * SecondsSince(waitStart));
    g_FrameWaited = true;

    // Input here too, which it was never going to hurt: one sync per frame either way, and
    // a stick read at the top of the frame acts on the frame it was read for.
    ProcessInput();

    LocateViews(g_FrameState.predictedDisplayTime);
}

// "session state 1" is not a diagnosis. IDLE in particular means the runtime has taken
// the session and is declining to run it - usually because the headset is not being worn,
// or because something else owns the compositor - and reading that as a bug in the hook
// wastes an evening.
const char * SessionStateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE:         return "IDLE - created, but the runtime will not run it yet (headset not worn? another app in front?)";
        case XR_SESSION_STATE_READY:        return "READY - the runtime wants us to begin";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED - running, not yet visible";
        case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED:      return "FOCUSED - visible and receiving input";
        case XR_SESSION_STATE_STOPPING:     return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING - the runtime is going away";
        case XR_SESSION_STATE_EXITING:      return "EXITING";
        default:                            return "UNKNOWN";
    }
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
            advancedfx::Message("AFXVR: session %s\n", SessionStateName(g_State));

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

bool MirvVrXr_CaptureBeforeUi() {
    return g_CaptureBeforeUi;
}

bool MirvVrXr_WantsPanel() {
    return g_PanelEnabled && g_SessionRunning;
}

bool MirvVrXr_WantsPasses() {
    // As soon as the session is running, not once it is visible. The runtime only
    // advances a session past READY when the application starts its frame loop, so
    // waiting for SYNCHRONIZED before running it means it never starts.
    //
    // Or when forced, with no session and no headset, so the pass machinery can be
    // watched at a desk. Nothing is submitted in that case - see SubmitEye.
    return g_SessionRunning || 0 < g_ForcedPasses;
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
    g_FrameWaited = false;
    g_ProjViewsValid = false;
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
    // First, before anything this function does can be counted as part of the frame.
    SampleFrameTime();

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

    // In low-latency mode this is where the frame's poses come from, located a few
    // microseconds ago rather than a whole frame ago.
    EngineThread_WaitAndLocate();

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

    for (int pass = 0; pass < 2; pass++) {
        int eye = g_SwapEyes ? (1 - pass) : pass;
        float right, forward, up, dPitch, dYaw, dRoll;
        XrPoseToEye(views[eye].pose, base, right, forward, up, dPitch, dYaw, dRoll);
        if (g_Monoscopic) {
            right = forward = up = 0.0f;
        } else {
            right *= g_IpdScale; forward *= g_IpdScale; up *= g_IpdScale;
        }
        AfxVr_SetEye(pass + 1, true, right, forward, up, dPitch, dYaw, dRoll,
            EffectiveFovDegrees(views[eye].fov));
    }
}

void MirvVrXr_RenderThread_SubmitPanel(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture) {
    if (!g_PanelEnabled || !pContext || !pTexture) return;
    if (!g_SessionRunning) return;
    if (XR_NULL_HANDLE == g_Swapchain[kPanelSwapchain]) return; // first frame, not made yet

    // This runs during the main pass, which comes before the eyes and therefore before
    // xrBeginFrame. That is allowed: acquiring, waiting on and releasing a swapchain image
    // is not scoped to a frame, only referencing it at xrEndFrame is - and by then it has
    // been released.
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage_(g_Swapchain[kPanelSwapchain], &acquire, &imageIndex),
               "xrAcquireSwapchainImage (panel)")) return;

    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    if (Check(xrWaitSwapchainImage_(g_Swapchain[kPanelSwapchain], &wait), "xrWaitSwapchainImage (panel)")) {
        pContext->CopyResource(g_SwapchainImages[kPanelSwapchain][imageIndex], pTexture);
        g_PanelCopied = true;
    }

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g_Swapchain[kPanelSwapchain], &release);
}

void MirvVrXr_RenderThread_SubmitEye(int eyeIndex, ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture) {
    if (eyeIndex < 0 || eyeIndex > 1) return;
    if (!pContext || !pTexture) return;
    // Forced passes render but go nowhere: every call below needs a session, and half the
    // function pointers are null without one.
    if (!g_SessionRunning) return;

    if (0 == eyeIndex) {
        if (!MirvVrXr_WantsPasses()) return;

        if (g_LowLatency) {
            // The engine thread waited and located at the top of this frame. If it has
            // not - the session only just started, say - there is nothing to begin.
            if (!g_FrameWaited) return;
            g_FrameWaited = false;
        } else {
            g_FrameState = { XR_TYPE_FRAME_STATE };
            XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
            LARGE_INTEGER waitStart = StageStart();
            if (!Check(xrWaitFrame_(g_Session, &waitInfo, &g_FrameState), "xrWaitFrame")) return;
            g_StageWaitFrame.Add(1000.0 * SecondsSince(waitStart));
        }

        XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
        if (!Check(xrBeginFrame_(g_Session, &beginInfo), "xrBeginFrame")) return;
        g_FrameBegun = true;

        if (!g_LowLatency) {
            ProcessInput();
            LocateViews(g_FrameState.predictedDisplayTime);
        }

        g_EyesCopied = 0;
        EnsureSwapchains(pTexture);

        // Report the poses the image was rendered from, not the ones just located. The
        // runtime then warps correctly from there to wherever the head is at display
        // time; reporting the fresh poses tells it no correction is needed, and the
        // world appears to swim as the head turns.
        XrView rendered[2];
        g_ProjViewsValid = false;
        {
            std::lock_guard<std::mutex> lock(g_ViewMutex);
            if (g_RenderedViewsValid) { rendered[0] = g_RenderedViews[0]; rendered[1] = g_RenderedViews[1]; g_ProjViewsValid = true; }
            else if (g_ViewsValid) { rendered[0] = g_Views[0]; rendered[1] = g_Views[1]; g_ProjViewsValid = true; }
        }

        // Monoscopic means both images really were drawn from the midpoint, so both must be
        // *reported* from the midpoint too. Reporting the true eye poses for identical
        // images is what the runtime is asked to reconcile, and it reconciles it by pulling
        // them apart - which made the one diagnostic that was supposed to be unambiguous
        // fail by construction.
        if (g_Monoscopic && g_ProjViewsValid) {
            XrVector3f mid;
            mid.x = 0.5f * (rendered[0].pose.position.x + rendered[1].pose.position.x);
            mid.y = 0.5f * (rendered[0].pose.position.y + rendered[1].pose.position.y);
            mid.z = 0.5f * (rendered[0].pose.position.z + rendered[1].pose.position.z);
            rendered[0].pose.position = mid;
            rendered[1].pose.position = mid;
        }

        // Nothing located yet. The frame still has to be ended -- leaving one open is what
        // stalls the runtime -- so fall through with no layer rather than returning.
        for (int eye = 0; g_ProjViewsValid && eye < 2; eye++) {
            g_ProjViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            g_ProjViews[eye].pose = rendered[eye].pose;

            // Report the symmetric frustum actually rendered, not the runtime's
            // asymmetric recommendation. The vertical half-angle follows from the image
            // aspect - equal to the horizontal one only when the image is square, which
            // is what the first 1080x1080 test happened to be.
            // What the image is claimed to cover must be what the image actually covers -
            // the frustum we WANTED, not the number we handed the engine to get it. With
            // the aspect fix off those are the same; with it on they differ by exactly the
            // engine's 4:3 convention, which is the point.
            float claimed = (g_ReportedFovOverrideDegrees > 0.0f)
                ? g_ReportedFovOverrideDegrees
                : WantedFovDegrees(rendered[eye].fov);
            float half = 0.5f * claimed * (float)(M_PI / 180.0);
            float vHalf = half;
            if (g_FovVerticalOverrideDegrees > 0.0f) {
                vHalf = 0.5f * g_FovVerticalOverrideDegrees * (float)(M_PI / 180.0);
            } else if (g_SwapchainWidth && g_SwapchainHeight) {
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
        LARGE_INTEGER copyStart = StageStart();
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
        // Acquire, wait, copy and release together: the interesting quantity is what one
        // eye costs to hand over, and splitting it further would only measure the D3D11
        // driver's queueing rather than any work.
        g_StageCopy.Add(1000.0 * SecondsSince(copyStart));
    }

    // A begun frame must always be ended, with or without a layer. Leaving one open is
    // what stalls the runtime.
    if (1 == eyeIndex) {
        for (int eye = 0; eye < 2; eye++) {
            // g_ProjViews is indexed by *view*, which the runtime requires to be left then
            // right. The swapchain it points at is the one the pass that rendered that
            // view wrote into, which is not the same index once the eyes are swapped.
            g_ProjViews[eye].subImage.swapchain = g_Swapchain[g_SwapEyes ? (1 - eye) : eye];
            g_ProjViews[eye].subImage.imageRect.offset = { 0, 0 };
            g_ProjViews[eye].subImage.imageRect.extent = { (int32_t)g_SwapchainWidth, (int32_t)g_SwapchainHeight };
            g_ProjViews[eye].subImage.imageArrayIndex = 0;
        }

        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = g_Space;
        layer.viewCount = 2;
        layer.views = g_ProjViews;

        // The panel goes on top of the world, so it comes second: the runtime composites
        // layers in the order given.
        XrCompositionLayerQuad panel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        panel.space = g_Space;
        panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        panel.pose = g_PanelPose;
        panel.subImage.swapchain = g_Swapchain[kPanelSwapchain];
        panel.subImage.imageRect.offset = { 0, 0 };
        panel.subImage.imageRect.extent = { (int32_t)g_SwapchainWidth, (int32_t)g_SwapchainHeight };
        panel.subImage.imageArrayIndex = 0;
        panel.size.width = g_PanelWidthMetres;
        // Keep the source aspect, whatever it happens to be. The window is the eye size,
        // so on this machine the panel is taller than it is wide -- ugly, but honest, and
        // stretching text is worse than an odd shape.
        panel.size.height = (g_SwapchainWidth > 0)
            ? g_PanelWidthMetres * (float)g_SwapchainHeight / (float)g_SwapchainWidth
            : g_PanelWidthMetres;

        const XrCompositionLayerBaseHeader * layers[2] = {
            (XrCompositionLayerBaseHeader*)&layer,
            (XrCompositionLayerBaseHeader*)&panel,
        };

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = g_FrameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

        bool haveWorld = (2 == g_EyesCopied) && g_ProjViewsValid;
        bool havePanel = g_PanelEnabled && g_PanelCopied && g_PanelPlaced
            && XR_NULL_HANDLE != g_Swapchain[kPanelSwapchain];

        if (haveWorld && havePanel)      { endInfo.layerCount = 2; endInfo.layers = layers; }
        else if (haveWorld)              { endInfo.layerCount = 1; endInfo.layers = layers; }
        else                             { endInfo.layerCount = 0; endInfo.layers = nullptr; }

        g_PanelCopied = false;

        LARGE_INTEGER endStart = StageStart();
        Check(xrEndFrame_(g_Session, &endInfo), "xrEndFrame");
        g_StageEndFrame.Add(1000.0 * SecondsSince(endStart));
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
                advancedfx::Message(
                    "AFXVR:   xrWaitFrame %.2f ms (worst %.2f)  locate %.2f  copy %.2f x%i  xrEndFrame %.2f\n",
                    g_StageWaitFrame.Mean(), g_StageWaitFrame.worstMs,
                    g_StageLocate.Mean(), g_StageCopy.Mean(),
                    g_StageCopy.count ? g_StageCopy.count / (g_StageWaitFrame.count ? g_StageWaitFrame.count : 1) : 0,
                    g_StageEndFrame.Mean());
            }
            g_StageWaitFrame.Reset();
            g_StageLocate.Reset();
            g_StageCopy.Reset();
            g_StageEndFrame.Reset();
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
        if (!_stricmp(arg1, "mono")) {
            g_Monoscopic = (3 <= args->ArgC()) ? (0 != atoi(args->ArgV(2))) : !g_Monoscopic;
            advancedfx::Message(
                "AFXVR: %s.\n"
                "  %s\n",
                g_Monoscopic ? "monoscopic - both eyes from the midpoint, zero separation"
                             : "stereo - eyes at the separation the runtime reports",
                g_Monoscopic
                    ? "The two images are now identical, so they must fuse. If they still do\n"
                      "  not, the fault is not the stereo - it is how the frames are submitted."
                    : "Back to normal.");
            return;
        }
        if (!_stricmp(arg1, "swap")) {
            g_SwapEyes = (3 <= args->ArgC()) ? (0 != atoi(args->ArgV(2))) : !g_SwapEyes;
            advancedfx::Message("AFXVR: eyes %s. Takes effect next frame.\n",
                g_SwapEyes ? "SWAPPED" : "in runtime order");
            return;
        }
        if (!_stricmp(arg1, "latency")) {
            bool want = g_LowLatency;
            if (3 <= args->ArgC()) {
                want = (0 == _stricmp(args->ArgV(2), "low"));
            } else {
                want = !g_LowLatency;
            }
            if (want != g_LowLatency) {
                // Switching mid-session would leave a wait outstanding on one side or the
                // other. Cheap to be strict about.
                if (g_SessionRunning) {
                    advancedfx::Warning(
                        "AFXVR: stop the session before changing this (mirv_vr_xr stop), or the\n"
                        "AFXVR: outstanding xrWaitFrame ends up on the wrong thread.\n");
                    return;
                }
                g_LowLatency = want;
            }
            advancedfx::Message(
                "AFXVR: %s.\n"
                "  %s\n",
                g_LowLatency ? "low latency: wait and locate on the engine thread"
                             : "safe: wait and locate on the render thread",
                g_LowLatency
                    ? "Poses belong to the frame being drawn. This is the canonical OpenXR\n"
                      "  arrangement and it has not yet been worn - if the world swims or the\n"
                      "  runtime complains about call order, switch back."
                    : "Poses are a frame old by construction, which the runtime reprojects\n"
                      "  away. Known to work.");
            return;
        }
        if (!_stricmp(arg1, "ui")) {
            if (3 <= args->ArgC()) {
                // "in" keeps the UI in the eyes, "out" takes it out.
                g_CaptureBeforeUi = (0 == _stricmp(args->ArgV(2), "out"));
            } else {
                g_CaptureBeforeUi = !g_CaptureBeforeUi;
            }
            advancedfx::Message(
                "AFXVR: the UI is now %s the eyes.\n"
                "  %s\n"
                "  Takes effect on the next frame; no need to restart the session.\n",
                g_CaptureBeforeUi ? "OUT of" : "IN",
                g_CaptureBeforeUi
                    ? "The world is clean, and the demo menu is not visible at all."
                    : "The menu is there, at screen depth, in both eyes - which is wrong but readable.");
            return;
        }
        if (!_stricmp(arg1, "passes")) {
            g_ForcedPasses = (3 <= args->ArgC()) ? atoi(args->ArgV(2)) : 2;
            if (g_ForcedPasses < 0) g_ForcedPasses = 0;
            advancedfx::Message(
                "AFXVR: forcing %i extra render passes with no session.\n"
                "  Nothing is submitted anywhere. This exists so the pass loop and the\n"
                "  per-pass camera can be watched without a headset - arm mirv_vr_log and\n"
                "  read game/csgo/console.log.\n",
                g_ForcedPasses);
            return;
        }
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
        "mirv_vr_xr fps [0|1] - log submitted frames per second, and where they go.\n"
        "mirv_vr_xr passes [n] - render n extra passes with no session, for debugging.\n"
        "mirv_vr_xr ui in|out - whether the HUD and demo menu are baked into the eyes.\n"
        "mirv_vr_xr latency safe|low - which thread waits for and locates the head.\n"
        "mirv_vr_xr swap [0|1] - exchange which eye gets which image, to test a hunch.\n"
        "\n"
        "Instance: %s, session: %s, submitting: %s\n"
        "State: %s\n"
        "Last measured: %.1f frames/s at %ux%u per eye.\n",
        MirvVrXr_IsRunning() ? "up" : "down",
        (XR_NULL_HANDLE != g_Session) ? "created" : "none",
        MirvVrXr_WantsPasses() ? "yes" : "no",
        SessionStateName(g_State),
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

CON_COMMAND(mirv_vr_frametime, "cs2-vr-spectator: sample frame times and report the distribution.")
{
    int argc = args->ArgC();

    if (2 <= argc) {
        double seconds = atof(args->ArgV(1));
        if (seconds <= 0.0) {
            g_SamplingFrames = false;
            advancedfx::Message("mirv_vr_frametime: stopped.\n");
            return;
        }
        if (seconds > 120.0) seconds = 120.0;

        g_SampleLabel[0] = '\0';
        if (3 <= argc) strcpy_s(g_SampleLabel, args->ArgV(2));

        g_FrameSampleCount = 0;
        g_SampledSeconds = 0.0;
        g_SampleUntilSeconds = seconds;
        g_LastFrameStamp.QuadPart = 0;
        g_SamplingFrames = true;

        advancedfx::Message("mirv_vr_frametime: sampling %.0f s%s%s...\n",
            seconds, g_SampleLabel[0] ? " as " : "", g_SampleLabel);
        return;
    }

    advancedfx::Message(
        "mirv_vr_frametime <seconds> [label] - sample frame times, then report.\n"
        "mirv_vr_frametime 0                 - stop early.\n"
        "\n"
        "Reports mean, median, p95, p99 and the worst frame. The mean is the least\n"
        "useful of those: a run averaging 60 that drops one frame in twenty feels worse\n"
        "in a headset than a steady 50, and only the tail shows it.\n"
        "\n"
        "Measured around the whole engine frame, so it works with no session and no\n"
        "headset -- which is what makes the graphics settings comparable at a desk.\n");
}

CON_COMMAND(mirv_vr_panel, "cs2-vr-spectator: the demo menu on a flat panel in space, instead of smeared across both eyes.")
{
    int argc = args->ArgC();

    if (2 <= argc) {
        const char * arg1 = args->ArgV(1);

        if (!_stricmp(arg1, "on") || !_stricmp(arg1, "1")) {
            g_PanelEnabled = true;
            // Place it where the viewer is looking now, rather than wherever it was left.
            {
                std::lock_guard<std::mutex> lock(g_ViewMutex);
                if (g_ViewsValid) PlacePanelFrom(g_Views[0].pose);
            }
            advancedfx::Message(
                "mirv_vr_panel: on, %.2f m wide at %.2f m.\n"
                "  Pair it with mirv_vr_xr ui out, or the menu is on the panel AND in both eyes.\n",
                g_PanelWidthMetres, g_PanelDistanceMetres);
            return;
        }
        if (!_stricmp(arg1, "off") || !_stricmp(arg1, "0")) {
            g_PanelEnabled = false;
            advancedfx::Message("mirv_vr_panel: off.\n");
            return;
        }
        if (!_stricmp(arg1, "place")) {
            std::lock_guard<std::mutex> lock(g_ViewMutex);
            if (g_ViewsValid) {
                PlacePanelFrom(g_Views[0].pose);
                advancedfx::Message("mirv_vr_panel: placed in front of you.\n");
            } else {
                advancedfx::Warning("mirv_vr_panel: no head pose yet; start the session first.\n");
            }
            return;
        }
        if (!_stricmp(arg1, "size") && 3 <= argc) {
            g_PanelWidthMetres = (float)atof(args->ArgV(2));
            if (g_PanelWidthMetres < 0.1f) g_PanelWidthMetres = 0.1f;
            if (g_PanelWidthMetres > 20.0f) g_PanelWidthMetres = 20.0f;
            advancedfx::Message("mirv_vr_panel: %.2f m wide.\n", g_PanelWidthMetres);
            return;
        }
        if (!_stricmp(arg1, "distance") && 3 <= argc) {
            g_PanelDistanceMetres = (float)atof(args->ArgV(2));
            if (g_PanelDistanceMetres < 0.3f) g_PanelDistanceMetres = 0.3f;
            if (g_PanelDistanceMetres > 20.0f) g_PanelDistanceMetres = 20.0f;
            advancedfx::Message("mirv_vr_panel: %.2f m away. Use 'place' to move it there.\n",
                g_PanelDistanceMetres);
            return;
        }
    }

    advancedfx::Message(
        "mirv_vr_panel on|off      - the demo menu as a flat panel in space.\n"
        "mirv_vr_panel place       - put it in front of where you are looking now.\n"
        "mirv_vr_panel size <m>    - how wide, in metres.\n"
        "mirv_vr_panel distance <m>- how far away the next 'place' puts it.\n"
        "\n"
        "The timeline, the scoreboard and the speed controls are a flat overlay the game\n"
        "draws at screen depth. Copied into each eye, that is doubled, at the wrong\n"
        "distance, and unreadable. A quad layer is what a flat panel wants to be in a\n"
        "headset, and the runtime composites it properly.\n"
        "\n"
        "It carries the main pass - the one image that still has the UI in it once\n"
        "mirv_vr_xr ui out has taken it out of the eyes. Use the two together.\n"
        "\n"
        "World-locked, not head-locked. A panel that follows your eyes cannot be looked\n"
        "away from, and looking away from the menu is most of what a viewer does.\n"
        "\n"
        "Current: %s, %.2f m wide, placed %s. Session %s.\n",
        g_PanelEnabled ? "on" : "off",
        g_PanelWidthMetres,
        g_PanelPlaced ? "yes" : "not yet",
        g_SessionRunning ? "running" : "not running");
}

CON_COMMAND(mirv_vr_views, "cs2-vr-spectator: what the runtime actually reports for each eye.")
{
    if (XR_NULL_HANDLE == g_Session) {
        advancedfx::Message("mirv_vr_views: no session.\n");
        return;
    }

    XrView v[2];
    bool valid = false;
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        if (g_ViewsValid) { v[0] = g_Views[0]; v[1] = g_Views[1]; valid = true; }
    }
    if (!valid) {
        advancedfx::Message("mirv_vr_views: nothing located yet.\n");
        return;
    }

    const double r2d = 180.0 / M_PI;

    // Along the head's own right axis, not the world's x. Comparing world coordinates
    // reports a separation that swings with which way the viewer is facing and reads as
    // "the eyes are swapped" whenever they turn round - which it did, and which cost an
    // entirely wrong diagnosis before it was noticed.
    const XrQuaternionf & q0 = v[0].pose.orientation;
    float rx = 1.0f - 2.0f * (q0.y * q0.y + q0.z * q0.z);
    float ry = 2.0f * (q0.x * q0.y + q0.w * q0.z);
    float rz = 2.0f * (q0.x * q0.z - q0.w * q0.y);

    float dx = v[1].pose.position.x - v[0].pose.position.x;
    float dy = v[1].pose.position.y - v[0].pose.position.y;
    float dz = v[1].pose.position.z - v[0].pose.position.z;

    float alongRight = dx * rx + dy * ry + dz * rz;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);

    advancedfx::Message(
        "mirv_vr_views - straight from xrLocateViews, before anything of ours touches it.\n"
        "\n"
        "  Eyes are %.1f mm apart, %+.1f mm of that along the head's right axis.\n"
        "  %s\n"
        "\n",
        1000.0f * distance, 1000.0f * alongRight,
        (alongRight > 0.0f) ? "view 0 is the left eye, as the specification says."
                            : "view 0 is the RIGHT eye - the eyes really are the wrong way round.");

    for (int eye = 0; eye < 2; eye++) {
        double l = v[eye].fov.angleLeft * r2d;
        double r = v[eye].fov.angleRight * r2d;
        double u = v[eye].fov.angleUp * r2d;
        double d = v[eye].fov.angleDown * r2d;

        // Symmetric frustums have equal and opposite half-angles. A canted display does
        // not, and the difference is what we currently throw away.
        double hAsym = (r + l);   // zero when symmetric, since l is negative
        double vAsym = (u + d);

        const XrQuaternionf & q = v[eye].pose.orientation;
        double yaw = atan2(2.0 * (q.w * q.y + q.z * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)) * r2d;

        advancedfx::Message(
            "  view %i  pos (%+.3f %+.3f %+.3f) m   yaw %+.2f deg\n"
            "          fov  left %+.2f  right %+.2f  up %+.2f  down %+.2f  (degrees)\n"
            "          asymmetry: horizontal %+.2f, vertical %+.2f\n"
            "          we render and report a symmetric %.2f deg\n",
            eye,
            v[eye].pose.position.x, v[eye].pose.position.y, v[eye].pose.position.z,
            yaw, l, r, u, d, hAsym, vAsym,
            SymmetricFovDegrees(v[eye].fov));
    }

    advancedfx::Message(
        "\n"
        "  What to look for. A horizontal asymmetry of more than a degree or two, or a\n"
        "  per-eye yaw that is not zero, means the headset's displays are canted and the\n"
        "  symmetric frustum we render is not the one we are claiming. That does not stop\n"
        "  the image appearing; it stops the two halves fusing, worse towards the edges.\n"
        "  Eyes currently %s.\n",
        g_SwapEyes ? "SWAPPED by mirv_vr_xr swap" : "in runtime order");
}

CON_COMMAND(mirv_vr_fov, "cs2-vr-spectator: override the frustum, when the automatic one does not fuse.")
{
    int argc = args->ArgC();

    if (2 <= argc && !_stricmp(args->ArgV(1), "auto")) {
        g_FovOverrideDegrees = 0.0f;
        g_FovScale = 1.0f;
        g_FovVerticalOverrideDegrees = 0.0f;
        advancedfx::Message("mirv_vr_fov: automatic.\n");
        return;
    }

    if (3 <= argc && !_stricmp(args->ArgV(1), "scale")) {
        g_FovScale = (float)atof(args->ArgV(2));
        if (g_FovScale < 0.2f) g_FovScale = 0.2f;
        if (g_FovScale > 2.0f) g_FovScale = 2.0f;
        advancedfx::Message("mirv_vr_fov: scale %.3f\n", g_FovScale);
        return;
    }

    if (3 <= argc && !_stricmp(args->ArgV(1), "report")) {
        g_ReportedFovOverrideDegrees = (float)atof(args->ArgV(2));
        advancedfx::Message("mirv_vr_fov: claiming %.1f degrees%s\n",
            g_ReportedFovOverrideDegrees,
            g_ReportedFovOverrideDegrees <= 0.0f ? " (same as rendered)" : " regardless of what is rendered");
        return;
    }

    if (3 <= argc && !_stricmp(args->ArgV(1), "reportstep")) {
        float base = (g_ReportedFovOverrideDegrees > 0.0f) ? g_ReportedFovOverrideDegrees : 108.0f;
        g_ReportedFovOverrideDegrees = base + (float)atof(args->ArgV(2));
        if (g_ReportedFovOverrideDegrees < 20.0f) g_ReportedFovOverrideDegrees = 20.0f;
        if (g_ReportedFovOverrideDegrees > 170.0f) g_ReportedFovOverrideDegrees = 170.0f;
        advancedfx::Message("mirv_vr_fov: claiming %.1f degrees\n", g_ReportedFovOverrideDegrees);
        return;
    }

    if (3 <= argc && !_stricmp(args->ArgV(1), "vertical")) {
        g_FovVerticalOverrideDegrees = (float)atof(args->ArgV(2));
        advancedfx::Message("mirv_vr_fov: vertical %s\n",
            g_FovVerticalOverrideDegrees > 0.0f ? "overridden" : "from the image aspect");
        return;
    }

    if (2 <= argc) {
        g_FovOverrideDegrees = (float)atof(args->ArgV(1));
        advancedfx::Message("mirv_vr_fov: %.1f degrees horizontal%s\n",
            g_FovOverrideDegrees,
            g_FovOverrideDegrees <= 0.0f ? " (automatic)" : "");
        return;
    }

    advancedfx::Message(
        "mirv_vr_fov <degrees>       - horizontal field of view. 0 or 'auto' for automatic.\n"
        "mirv_vr_fov scale <k>       - multiply the automatic value instead.\n"
        "mirv_vr_fov vertical <deg>  - override the vertical too. 0 derives it from aspect.\n"
        "\n"
        "The automatic value is the smallest symmetric frustum containing the runtime's\n"
        "asymmetric one - 2 x the larger half-angle. On a Quest 3 that is 108 degrees for a\n"
        "true frustum of 94 off-centre by 7, which is correct only if CS2 renders exactly\n"
        "the number it is given. On a portrait window it may not: the engine's fov\n"
        "convention is written around 4:3.\n"
        "\n"
        "If the two eyes will not fuse, this is the dial. Whatever it produces is used both\n"
        "for what the game renders and for what the projection layer claims, so the two\n"
        "cannot drift apart.\n"
        "\n"
        "Current: %s%.1f deg horizontal, scale %.3f, vertical %s.\n",
        g_FovOverrideDegrees > 0.0f ? "" : "automatic -> ",
        g_FovOverrideDegrees > 0.0f ? g_FovOverrideDegrees : 0.0f,
        g_FovScale,
        g_FovVerticalOverrideDegrees > 0.0f ? "overridden" : "from aspect");
}

CON_COMMAND(mirv_vr_fovfix, "cs2-vr-spectator: correct for Source computing its field of view against 4:3.")
{
    if (2 <= args->ArgC()) {
        g_SourceAspectFix = 0 != atoi(args->ArgV(1));
        advancedfx::Message("mirv_vr_fovfix: %s\n", g_SourceAspectFix ? "on" : "off");
        return;
    }

    float wanted = 108.0f;
    float asked = SourceFovForWanted(wanted);

    advancedfx::Message(
        "mirv_vr_fovfix 0|1 - ask Source for the angle that makes it render the frustum we\n"
        "want, instead of assuming it renders the number it is given.\n"
        "\n"
        "Source treats fov as the horizontal angle at 4:3, derives the vertical from it, and\n"
        "then recomputes the horizontal for the real aspect. This window is %ux%u - aspect\n"
        "%.3f, portrait - so the gap is large: asking for %.1f degrees renders about %.1f.\n"
        "\n"
        "Claiming a frustum the image does not have corrupts the disparity in proportion to\n"
        "the interpupillary distance. It hid for a long time because SteamVR reports 22 mm\n"
        "here; Meta's runtime reports the true 61 mm and the eyes stop fusing.\n"
        "\n"
        "Currently %s. At this aspect, wanting %.1f means asking for %.1f.\n",
        g_SwapchainWidth, g_SwapchainHeight, AspectOfImage(),
        wanted, RenderedFovForAsked(wanted),
        g_SourceAspectFix ? "on" : "off",
        wanted, asked);
}

CON_COMMAND(mirv_vr_calibrate, "cs2-vr-spectator: tune the stereo from the controllers, because the person who can see it is wearing a headset.")
{
    if (2 <= args->ArgC()) {
        g_Calibrating = 0 != atoi(args->ArgV(1));
    } else {
        g_Calibrating = !g_Calibrating;
    }

    if (!g_Calibrating) {
        advancedfx::Message(
            "mirv_vr_calibrate: off. Buttons do their usual jobs again.\n"
            "  Keeping: separation x%.2f, %s, %s\n",
            g_IpdScale,
            g_ReportedFovOverrideDegrees > 0.0f ? "claiming an overridden field of view" : "claiming what is rendered",
            g_Monoscopic ? "MONOSCOPIC" : "stereo");
        return;
    }

    advancedfx::Message(
        "mirv_vr_calibrate: ON. The face buttons and grips are now dials.\n"
        "\n"
        "  X / Y          eye separation  -/+ 0.1x     (for \"they diverge too strongly\")\n"
        "  A / B          claimed field of view -/+ 4 deg\n"
        "  left grip      monoscopic on/off - both eyes identical, so they MUST fuse\n"
        "  right grip     back to defaults\n"
        "  sticks         still fly, as usual\n"
        "\n"
        "Start with the left grip. If a monoscopic image still will not fuse, nothing here\n"
        "will help and the fault is in how frames are submitted, not in the stereo.\n"
        "If it does fuse, come back to stereo and wind the separation down with X until it\n"
        "does - whatever number that is tells us what is actually wrong.\n"
        "\n"
        "Now: separation x%.2f, %s, %s\n",
        g_IpdScale,
        g_ReportedFovOverrideDegrees > 0.0f ? "field of view overridden" : "claiming what is rendered",
        g_Monoscopic ? "MONOSCOPIC" : "stereo");
}
