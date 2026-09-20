#include "stdafx.h"

#include "MirvVrXr.h"
#include "MirvVr.h"
#include "MirvVrMath.h"
#include "MirvVrXrInternal.h"
#include "MirvTime.h"

#include "WrpConsole.h"

#include "../shared/AfxConsole.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#define _USE_MATH_DEFINES
#include <tlhelp32.h>
#include <sddl.h>
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

// The game's swap chain, owned by RenderSystemDX11Hooks.cpp. Wanted for one thing only:
// the window handle, so a hit on the sheet can be turned into a point in that window.
extern IDXGISwapChain * g_pSwapChain;

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
const int kSwapchainCount = 4;
const int kPanelSwapchain = 2;
// Eight pixels of solid white, for the pointer dot. Tiny and its own swapchain, because
// pointing a quad at a corner of the sheet would draw whatever menu pixel happened to be
// there.
const int kCursorSwapchain = 3;
const uint32_t kCursorTexels = 32;

// Enough for the HUD groups the sheet is cut into, with room to add one.
const int kMaxPanelQuads = 8;

XrSwapchain g_Swapchain[kSwapchainCount] = { XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE };
std::vector<ID3D11Texture2D*> g_SwapchainImages[kSwapchainCount];
uint32_t g_SwapchainWidth = 0, g_SwapchainHeight = 0;
// What the swapchains were actually created as, for anything else that has to copy into
// them - the cursor patch, which must be the same format or CopyResource refuses.
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

// The render thread's own copy, taken once at the top of a frame's submission and used
// for the whole of it. g_FrameState belongs to whichever thread last waited; this belongs
// to the frame being ended.
XrFrameState g_RtFrameState = { XR_TYPE_FRAME_STATE };

struct CursorSnapshot {
    bool hit = false;
    XrPosef pose = {};
};

// What each frame was rendered with, kept until the render thread gets to it.
//
// The engine thread decides the poses for frame N and queues the passes; the render thread
// submits them some unpredictable time later, by which point the engine thread may be one
// or two frames further on. Reading the current globals at that point reports a pose the
// image was never drawn from - and the amount it is wrong by changes frame to frame, which
// is what judder is. So each pass carries a ticket, and the ticket names an entry here.
//
// Eight is several frames of slack; if the render thread ever falls further behind than
// that the entry is simply gone and the projection falls back to the globals, which is
// the old behaviour. Its overlays are omitted: neither a mode nor an aim can be borrowed
// from another frame without putting the wrong thing over this image.
struct FrameRecord {
    unsigned long long serial = 0;
    bool valid = false;
    bool renderedValid = false;
    XrView rendered[2] = {};
    XrTime displayTime = 0;
    XrBool32 shouldRender = XR_FALSE;
    bool timingValid = false;   // only the low-latency path fills the timing in

    // A mode change can reach the engine while the render thread still owes an eye from
    // the old mode. Reading the live decision there abandoned that frame, or cut a team
    // picker into HUD pieces. The sheet and its pointer travel with the same decision:
    // moving the screen for the next frame must not move this frame's hit underneath it.
    AfxVrMath::ModeResult mode = {};
    bool sheetPlaced = false;
    XrPosef sheetPose = {};
    XrExtent2Df sheetSize = {};
    CursorSnapshot cursor;

    // Where the game was aiming when this frame was composed, and which way the viewer
    // was facing. The crosshair is drawn from these and not from the globals: the render
    // thread runs up to two thirds of a frame behind, so reading them at submit time can
    // get the NEXT frame's aim - a one-frame flash of the crosshair thirty degrees away
    // on every body snap, and a jitter against the picture while aiming. Experiment 17,
    // with a different quad.
    float aimYaw = 0.0f;
    float aimPitch = 0.0f;
    float bodyForwardWorld = 0.0f;
    float roomForwardRadians = 0.0f;
};
const int kFrameRing = 8;
FrameRecord g_FrameRing[kFrameRing];

// The record the frame being submitted belongs to. Taken when the first eye arrives and
// read when the layers are assembled with the second, because those are two calls.
FrameRecord g_SubmittedFrame;
unsigned long long g_SubmittedTicket = 0;
unsigned long long g_FrameSerial = 0;

bool ReadFrameRecord(unsigned long long ticket, FrameRecord & frame) {
    std::lock_guard<std::mutex> lock(g_ViewMutex);
    // A missing ticket must erase the previous answer. Projection poses have a fallback,
    // but neither an aim nor a mode from a different frame describes this image.
    frame = FrameRecord();
    const FrameRecord & record = g_FrameRing[ticket % kFrameRing];
    if (!ticket || !record.valid || record.serial != ticket) return false;
    frame = record;
    return true;
}

// How far behind the render thread was, in frames, when it submitted. Reported with the
// frame rate: anything but a constant means the pose being reported does not belong to the
// image, by a different amount every frame.
unsigned long long g_TicketLagSum = 0;
unsigned long long g_TicketLagCount = 0;
unsigned long long g_TicketLagWorst = 0;
unsigned long long g_TicketMisses = 0;

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

// A session with no map under it shows the game's own window on one screen instead of
// rendering a world into both eyes. See RenderThread_MenuFrame for why this exists and
// what it fixes.
//
// The switch is "is a demo loaded", read from the same clock the autostart gate reads. A
// PAUSED demo still has a tick, so pausing does not throw the viewer back to a flat
// screen; quitting to the menu does.
bool g_MenuMode = false;

// What the headset is showing and who the controllers belong to, decided once a frame by
// AfxVrMath::DecideMode from five facts and nothing else. These globals belong to the
// engine thread; the render thread gets the decision through the pass's FrameRecord,
// because even a locked read of the latest mode can describe a different frame.
AfxVrMath::ModeResult g_Mode = {};
AfxVrMath::ModeInputs g_ModeInputs = {};

// How the aim is driven.
//
//   hand   the right controller points, as every VR shooter does and as UEVR's
//          "Aim Method: Right Controller" does for flat games. The game's own aim is
//          servoed onto the direction the hand points; the crosshair shows where that
//          actually is, which during motion trails the hand by a frame or two.
//   stick  the right thumbstick moves the aim, with the deadzone cone. What was built
//          first, from a misreading: the user said "joystick" and meant the controller.
//   off    nothing touches the mouse.
//
// Off by default, and staying off: this drives the real mouse of a machine somebody else
// is sitting at, and nothing built on top of the hook should start doing that unasked.
// vr.cfg asks for hand, which is how a shipped release gets a game that can be aimed -
// the decision is in the config, where it can be read, rather than in this line.
//
// Declared here, above everything, rather than beside the servo that uses it, because
// PrintControls has to name the current method and runs long before that code.
enum AimMethod { kAimOff = 0, kAimHand = 1, kAimStick = 2 };
int g_AimMethod = kAimOff;

// Panorama shows the system cursor exactly when it wants something pointed at. Debounced,
// because the buy menu flickers it and a sheet that blinks in and out is worse than one
// that is late.
bool g_CursorShowing = false;
ULONGLONG g_CursorChangedAt = 0;
const ULONGLONG kCursorDebounceMs = 150;

bool SystemCursorShowing() {
    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) return false;
    return 0 != (info.flags & CURSOR_SHOWING);
}


// Whether the map under us is a recording or a game being played.
//
// Separate from g_MenuMode on purpose, because they answer different questions. "Is there
// a map" decides whether the headset gets a world or a screen; "is it a demo" decides what
// the HUD is. A demo has a timeline to scrub and a strip naming whoever is being watched;
// a game against bots has neither, and has ammunition and health instead. Showing a
// spectator timeline to somebody holding the gun is the kind of wrong that is worse than
// showing nothing.
bool g_PlayingDemo = false;
bool g_MenuPlaced = false;
XrPosef g_MenuPose = {};
XrExtent2Df g_MenuSize = {};
// Only the aspect crosses back from the render thread, under g_ViewMutex. The engine
// decides both the screen geometry and its hit test; a resize changes their shape
// together on the next ticket instead of moving one while the other is being read.
float g_MenuAspect = 1.0f;
float g_MenuWidthDegrees = 70.0f;
float g_MenuDistanceMetres = 2.0f;


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
bool g_ReportedFirstMenu = false;

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

// Where each hand is pointing.
//
// One pose action with two subaction paths rather than two actions, because that is what
// the runtime expects for a thing every hand has, and it keeps the binding list honest:
// the same idea bound to /user/hand/left and /user/hand/right, not two ideas that happen
// to look alike.
//
// "aim" and not "grip": grip is where the hand IS, aim is where the controller POINTS, and
// the difference is about twenty degrees of wrist. Pointing at a menu with the grip pose
// feels like aiming a torch held by its head.
XrAction g_AimAction = XR_NULL_HANDLE;

// The menu button on the left controller. The right one belongs to the runtime on this
// hardware and never reaches an application.
XrAction g_MenuButtonAction = XR_NULL_HANDLE;
bool g_PrevMenuButton = false;
ULONGLONG g_MenuButtonPressedAt = 0;

// Show the game's own window on a screen even though a map is loaded, so the settings and
// the pause menu are reachable without taking the headset off.
bool g_MenuOverride = false;
XrSpace g_AimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };   // 0 left, 1 right
XrPath g_HandPath[2] = { XR_NULL_PATH, XR_NULL_PATH };

// Located once a frame, on the engine thread, at the same instant as the views. Not read
// from a global by the render thread later: a ray drawn from a different instant than the
// quad it points at swims against it, which is the same fault as experiment 17 and would
// look like the pointer lagging the hand.
XrPosef g_AimPose[2] = {};
bool g_AimValid[2] = { false, false };

// Where the pointer last hit something, in the room. The render thread draws a dot there;
// the engine thread is what decided it, because that is also where the mouse is moved
// from and the two must not disagree. Published complete under g_ViewMutex and then
// copied into the ticket: publishing the hit first let the render thread draw a new
// hit with an unfinished position and orientation, sometimes from two different hits.
CursorSnapshot g_Cursor;
int g_CursorHand = -1;
float g_CursorU = 0.0f, g_CursorV = 0.0f;

// How big the dot is, in metres, and how far in front of the panel it floats so it is not
// fighting the panel for the same depth.
bool g_CursorPressed = false;

float g_CursorSizeMetres = 0.016f;
const float kCursorLiftMetres = 0.008f;

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

// What the triggers are for.
//
// Switching who you are watching is the most frequent thing a spectator does, and it
// belongs under the index fingers; seeking is rare and tolerates face buttons. That is the
// operator's own conclusion after a session with it the other way round.
//
// "fov" is the third mode, and a measuring instrument rather than a control: it was how
// the field-of-view convention got settled (experiment 18). Kept because the same dial is
// the right way to re-check it after any change to the crop or the window shape.
enum TriggerMode { kTriggersPlayers = 0, kTriggersSeek = 1, kTriggersFov = 2 };
int g_TriggerMode = kTriggersPlayers;

// Frames until the held trigger steps again. One press is one per cent, which is too fine
// to travel fifteen on, so holding repeats - slowly enough to stop where you meant to.
int g_TriggerRepeat = 0;
const int kTriggerRepeatFrames = 5;

ULONGLONG g_RecenterPressedAt = 0;
bool g_SlowMotion = false;
LARGE_INTEGER g_LastInputTicks = {};
bool g_HaveLastInputTick = false;

// Console commands raised by a controller button. They cannot be dispatched from the
// render thread where the input is read, so the engine thread drains this.
//
// `delay` is in engine frames. A button that maps to a game action has to be pressed and
// released on separate frames - dispatching "+attack" and "-attack" back to back in one
// frame is not a press the game notices.
struct PendingCommand { std::string cmd; int delay; };

std::mutex g_CmdMutex;
std::vector<PendingCommand> g_PendingCommands;

} // namespace

// The one function in this file that another translation unit calls, so it is the one that
// cannot live in the anonymous namespace above. The console pipe runs on a thread of its
// own in MirvVrXrPipe.cpp and every line it receives arrives here: a console command
// dispatched from a thread the engine has never heard of is not a command, it is a race
// against the command buffer.
//
// The queue it appends to stays private. Closing and reopening the anonymous namespace
// around this one definition is uglier than moving the queue out with it, and is the
// smaller change of the two: it keeps sixteen globals private and exports one function.
void MirvVrXrInternal::QueueCommand(const char * cmd, int delayFrames) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    if (g_PendingCommands.size() < 16) {
        PendingCommand p;
        p.cmd = cmd;
        p.delay = delayFrames;
        g_PendingCommands.push_back(p);
    }
}

// So that the forty-odd unqualified calls below - and every future one - keep reading the
// way they did when it was a local function.
using MirvVrXrInternal::QueueCommand;

namespace {

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

// Presses and releases a game action.
//
// This does NOT switch players, and it never did: the demo's spectator controls are driven
// from the key event, not from the button state a console "+attack" sets. The on-screen
// hint reads "[MOUSE1]: Next Player" because it is a binding lookup in the input layer, and
// that layer never sees a command dispatched through ExecuteClientCmd. The operator found
// it: the keyboard arrows switched players and the controller buttons did not, running the
// same "+attack" by two different routes.
//
// Kept for the things that ARE console commands. QueueKeyTap is what spectator input needs.
void QueueTap(const char * action) {
    std::string down("+"); down += action;
    std::string up("-");   up   += action;
    QueueCommand(down.c_str(), 0);
    QueueCommand(up.c_str(), 2);
}

// A real key, down now and up two frames later.
//
// Synthesised with SendInput and the scancode flag, which is how scripts/send-key.ps1
// reaches this game from outside and the only route the demo's spectator input is known to
// accept. The keys are the ones vr_keys.cfg binds - RIGHT, LEFT and UP - so the controller
// takes exactly the path the operator proved works, through the same binds, rather than a
// second mechanism that might behave differently.
//
// That is a real coupling: without those binds loaded the controller's player switching
// does nothing. Said here and in vr_keys.cfg, because a silent dependency between a config
// file and a DLL is the kind of thing that costs an evening.
struct PendingKey { unsigned short vk; int delay; bool down; };
std::vector<PendingKey> g_PendingKeys;

void SendGameKey(unsigned short vk, bool down) {
    // SendInput goes to whatever has focus. With a headset on, a window that has quietly
    // lost focus is invisible - and the key would land in another application.
    //
    // A RELEASE goes out anyway. The press already reached the game and the latch here is
    // already clear, so dropping the release leaves the key physically down with nothing
    // left that knows to lift it. An up with no matching down is a no-op everywhere else.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (down && pid != GetCurrentProcessId()) {
        static ULONGLONG lastWarned = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastWarned > 3000) {
            lastWarned = now;
            advancedfx::Warning(
                "AFXVR: the game window is not in the foreground, so controller buttons that\n"
                "AFXVR: work through a key press are going nowhere. Click the game window.\n");
        }
        return;
    }

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    // The arrows, like Home and the page keys, live on the extended part of the keyboard
    // and are not recognised without this.
    if (VK_LEFT == vk || VK_UP == vk || VK_RIGHT == vk || VK_DOWN == vk
        || VK_HOME == vk || VK_END == vk || VK_PRIOR == vk || VK_NEXT == vk
        || VK_INSERT == vk || VK_DELETE == vk) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &input, sizeof(input));
}

void QueueKeyTap(unsigned short vk) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    if (g_PendingKeys.size() < 16) {
        PendingKey down = { vk, 0, true };
        PendingKey up   = { vk, 2, false };
        g_PendingKeys.push_back(down);
        g_PendingKeys.push_back(up);
    }
}

// One key held or released, rather than tapped. Walking needs this: a tap is a step.
void QueueKeyState(unsigned short vk, bool down) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    if (g_PendingKeys.size() < 24) {
        PendingKey k = { vk, 0, down };
        g_PendingKeys.push_back(k);
    }
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

// Every key the controllers can hold DOWN, in one place.
//
// One place because of what happens otherwise. These are real key events: the game hears
// a key being down and acts until it comes up. Miss a release - a menu opens, the mode
// changes, focus is lost, the session stops - and the player walks into a wall for ever
// while wearing a headset, with no keyboard in reach. It has to be impossible by
// construction, so every exit path calls one function and the list lives next to it.
enum HeldKey {
    kHeldForward = 0, kHeldBack, kHeldLeft, kHeldRight,
    kHeldCrouch, kHeldJump, kHeldUse, kHeldWalk, kHeldCount
};
const unsigned short kHeldVk[kHeldCount] = {
    'W', 'S', 'A', 'D', VK_CONTROL, VK_SPACE, 'E', VK_SHIFT
};
bool g_HeldKey[kHeldCount] = { false, false, false, false, false, false, false, false };

void SetHeldKey(int which, bool want) {
    if (which < 0 || which >= kHeldCount) return;
    if (want == g_HeldKey[which]) return;
    g_HeldKey[which] = want;
    QueueKeyState(kHeldVk[which], want);
}

void ReleaseHeldKeys() {
    for (int i = 0; i < kHeldCount; i++) SetHeldKey(i, false);
}

// Walking.
//
// Two thresholds, not one. A single threshold at the edge of the deadzone chatters: a
// thumb resting at exactly that value sends down, up, down, up sixty times a second, and
// the game hears a player vibrating. Press at half throw, release at a third.
const float kWalkPress = 0.5f;
const float kWalkRelease = 0.3f;

void WalkFromStick(float x, float y) {
    // Into the frame the keys live in. The game walks along ITS yaw and the picture is
    // that yaw plus the head, so without this a push forward walks along the aim rather
    // than along the gaze - and with the head turned round, backwards.
    float forward = y, left = -x;
    AfxVrMath::StickToGameFrame(x, y,
        AfxVrMath::NormalizeDegrees(AfxVr_ViewYawDegrees() - AfxVr_BaseYawDegrees()),
        forward, left);

    // Forward, back, left, right as four one-sided amounts, so a diagonal presses two.
    const float amount[4] = { forward, -forward, left, -left };
    for (int i = 0; i < 4; i++) {
        bool now = g_HeldKey[i] ? (amount[i] > kWalkRelease) : (amount[i] > kWalkPress);
        SetHeldKey(i, now);
    }
}

XrCompositionLayerProjectionView g_ProjViews[2] = {};

// The sub-rectangle of each eye's image that the runtime's own frustum covers. See the
// long note where it is computed.
bool g_CropToRuntimeFov = true;

// OFF, and it must stay off until the measurement is of the right camera.
//
// The idea was sound: read the field of view the engine actually rendered out of its own
// projection matrix instead of trusting the number we handed it. The execution was not.
// The matrix is built once per frame, AFTER the passes, with the base camera restored -
// which this project's own patch notes say in as many words. So what gets measured is the
// demo camera's 90 degrees, not the eyes'. Claiming that wrecked a configuration that had
// just been reported as perfect, smoke and all.
//
// To make this work the projection would have to be sampled per pass, while the eye's
// camera is still in the struct. Until then, claim what we asked for.
bool g_UseMeasuredFov = false;
XrRect2Di g_CropRect[2] = {};
bool g_CropValid[2] = { false, false };

// Whether this runtime reads the fov we submit. Measured once, at startup: Oculus says no,
// SteamVR says yes, and nothing in this project checked it until the stereo would not fuse.
bool g_FovMutable = false;
bool g_FovMutableKnown = false;

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
//
// And it carries the HUD alone, not the main pass entire. The main pass has the finished
// world in it as well, so an opaque quad of it hangs a second copy of the world in front
// of the world - 48 degrees of it at the default size and distance. So the back buffer is
// wiped to transparent black between the world and the UI, the HUD draws onto nothing, and
// the quad is composited with source alpha.
bool g_PanelEnabled = false;
bool g_PanelTransparent = true;
bool g_PanelCopied = false;
unsigned long long g_PanelTicket = 0;
bool g_PanelPlaced = false;
XrPosef g_PanelPose = {};
float g_PanelWidthMetres = 1.6f;
float g_PanelDistanceMetres = 1.8f;

// The render target view used for that wipe. The game's back buffer is TYPELESS, so a
// view has to name a concrete format; it is cached because a view per frame would be a
// device object allocated and destroyed forty times a second. Keyed on the texture, which
// is how a resize or a session restart invalidates it without anyone having to remember.
ID3D11Texture2D * g_PanelClearFor = nullptr;
ID3D11RenderTargetView * g_PanelClearRtv = nullptr;

// --- one sheet, or several pieces of it ------------------------------------------------
//
// The sheet is the whole window, and the HUD lives around its edges: score across the top,
// radar in a corner, the player bar and timeline along the bottom. Hung in space as one
// quad that is a portrait sheet with an empty middle, parked at eye height - the score
// floats at the horizon in the middle of the map and the timeline lies across the floor.
// Nothing is where a HUD element wants to be, and making the sheet big enough to read
// walls off the view.
//
// So each group is its own quad, cut out of the SAME swapchain image with its own
// imageRect, and placed on its own. The rects are fractions of the sheet, which keeps them
// readable and independent of the window size; v runs down from the top, like imageRect.
//
// Everything not named here is simply never shown, which is also how the overhead name
// tags and the TrueView debug text stay off the panel without a cvar hunt.
struct PanelRegion {
    const char * name;
    float u0, v0, u1, v1;      // the piece of the sheet, as fractions
    float azimuthDegrees;      // around the anchor; positive is to the left
    float elevationDegrees;    // positive is up
    float widthDegrees;        // how wide it should look from where the viewer stands
    float distanceMetres;
    bool enabled;
    bool demoOnly;             // a piece that only exists while watching a recording
    bool gameOnly;             // and one that only exists while playing
};

// Measured off docs/experiments/screenshots/exp16-sheet-full.png: the same 0.909 aspect as
// the headset window, at 1264x1390 so the whole sheet fits on the screen and can be
// captured. That detail matters - the first attempt read fractions off a 2560x1600 grab of
// a 2528x2780 window, which is the top 58% of the sheet, and every number below v = 0.58
// was invented.
//
// They are fractions, so the resolution does not matter, but hud_scaling and the window's
// ASPECT both do: Panorama lays out to the shape of the window.
PanelRegion g_PanelRegions[] = {
    // Up high and wide, because it is what is being read at a glance. Both teams, the
    // score, the round timer, health and money.
    { "score",   0.26f, 0.00f, 0.74f, 0.18f,    0.0f,  44.0f, 40.0f, 1.2f, true,  false, false },
    // Low and to the left, where a spectator's eyes go when they want the map.
    { "radar",   0.00f, 0.00f, 0.21f, 0.28f,   78.0f, -36.0f, 20.0f, 1.0f, true,  false, false },
    // Low and central, like a dashboard. It is also what a controller ray will click one
    // day, so close and below the line of sight is right.
    //
    // It starts at 0.82 rather than at the timeline's own 0.93 because the bar naming the
    // player being watched - their name, health, money and weapon - sits just above it,
    // and it was missing from the capture the rects were measured on: that frame was a
    // freeze with no spectated target, so the strip did not exist to be measured. Without
    // it the viewer cannot see who they are watching, which is exactly what was needed to
    // tell whether the switch-player button had done anything.
    { "bar",     0.00f, 0.82f, 1.00f, 1.00f,    0.0f, -66.0f, 50.0f, 0.55f, true,  true,  false },
    // Off, and NOT measured: there were no kills on screen when the sheet was captured, so
    // this rect is a guess at where the feed appears. Turn it on with mirv_vr_panel region
    // killfeed on and correct it with mirv_vr_panel rect.
    { "killfeed",0.74f, 0.02f, 1.00f, 0.32f,  -40.0f,  26.0f, 24.0f, 2.0f, false, false, false },

    // The player's own HUD, which only exists while playing and for which the spectator
    // layout above has no place at all. Asked for from inside a bot game: "I cannot see
    // what weapon I have or how many rounds are left" - and the hands are hidden, so there
    // is not even a gun to look at.
    //
    // These two rects are ESTIMATES, unlike every other row here. The rest were measured
    // off a screenshot; these cannot be, because they live in the bottom 42 per cent of
    // the sheet, which the display clamps away and no desktop capture can reach. Correct
    // them with mirv_vr_panel rect while wearing it, then write the numbers back here.
    { "health",  0.00f, 0.88f, 0.30f, 1.00f,   30.0f, -42.0f, 20.0f, 0.9f, true,  false, true  },
    { "ammo",    0.68f, 0.86f, 1.00f, 1.00f,  -30.0f, -42.0f, 20.0f, 0.9f, true,  false, true  },
};
const int kPanelRegionCount = (int)(sizeof(g_PanelRegions) / sizeof(g_PanelRegions[0]));

// False puts the whole sheet back on one quad, which is what this started as and is still
// the honest way to see what the HUD actually contains.
bool g_PanelCutUp = true;

// How far out of the way the groups sit, as one number: every group's azimuth and
// elevation is multiplied by it.
//
// The operator's request was "spread the HUD further into the corners, so it is not so
// much in the centre", and answering that with three commands of three numbers each is not
// a control anyone will use twice. One factor is. The table above is 1.0.
//
// The Quest's usable field is about 44 degrees up, 55 down and 50 either side, so there is
// not much room above 1.3 before a group leaves the display entirely.
float g_PanelSpread = 1.0f;

// Whether the quads are submitted at all. Separate from g_PanelEnabled, which is whether
// the machinery runs: hiding the HUD keeps copying and clearing, so bringing it back does
// not cost a frame of stale sheet, and it comes back exactly where it was rather than
// being re-placed in front of wherever you happen to be looking.
bool g_PanelShown = true;

// The HUD follows the viewer's position, and only the viewer's position.
//
// World-locked in both was wrong in a way that took a worn screenshot to see. The anchor is
// captured when the session becomes FOCUSED - the moment the headset goes on a face, which
// is standing at the desk - and the viewer then steps back and sits down. Measured from
// that screenshot: the head ended up 1.05 m behind the anchor, consistently across all
// three groups. Everything followed. The timeline specified at 66 degrees below and 1.4 m
// out was lying on the floor ahead instead of in the lap; and since every quad is turned to
// face the ANCHOR, from a metre behind it they were all seen obliquely - the operator's
// "turned at a strange angle".
//
// Following the position and keeping the yaw gives a cockpit: the angles are true angles
// from the eyes whatever the body does, every quad faces the viewer by construction, and
// "look down and the bar is in my lap" holds while sitting, standing or leaning. The
// orientation stays world-locked, because a panel that follows the eyes cannot be looked
// away from and looking away from the HUD is most of what a viewer does.
// Where the head was when the viewer last recentred. Everything they do with their body
// afterwards is measured from here.
XrVector3f g_RoomRefPos = {};
bool g_RoomRefValid = false;

// A tracking glitch must not throw the camera across the map. Three metres is more room
// than anybody has in front of a desk.
const float kRoomOffsetLimitMetres = 3.0f;

bool g_PanelFollow = true;

XrVector3f g_PanelFollowPos = {};
bool g_PanelFollowValid = false;

// Low-passed, or head bob shows on a quad three quarters of a metre away. About a sixth of
// a second at this frame rate, which reads as a cockpit settling rather than as lag.
const float kPanelFollowAlpha = 0.2f;



// Frames of alpha reporting still owed, from mirv_vr_panel alpha.
//
// Whether any of this works comes down to one thing nobody knows: what Panorama writes to
// the alpha channel. Blending source-over onto a zeroed target gives premultiplied colour
// either way, but the alpha it leaves behind is the blend state's business. If alpha is
// write-masked it stays at zero, the runtime multiplies the panel by nothing, and the
// panel is invisible - which looks exactly like "the feature does not work". So it is
// measured rather than hoped for, and measured at a desk with no headset.
int g_PanelAlphaProbe = 0;

// Frames to wait before placing the panel, counted down after the session becomes FOCUSED.
//
// Placing it on the first pose available was wrong in a way only a worn session shows:
// scripts/start-vr.ps1 presses F9 itself, so the first pose is the headset lying on the
// desk or halfway to a face. The panel was then nailed to that yaw and that height, and
// the groups - at +16, -18 and -32 degrees around it - ended up behind or under the
// viewer. The user's report was simply "I did not see the HUD".
//
// FOCUSED is the right moment, because the Oculus runtime only grants it with the
// proximity sensor covered: it means "on a face". Every transition into it, not just the
// first, since taking the headset off and putting it back on is how a session is paused.
// The delay is for the swing of putting it on to settle.
int g_PanelPlaceCountdown = 0;
const int kPanelPlaceDelayFrames = 20;


PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_ = nullptr;

#define AFXVR_XR_FUNCS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetInstanceProperties) \
    X(xrGetSystem) X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) \
    X(xrGetViewConfigurationProperties) \
    X(xrResultToString) X(xrPollEvent) \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) \
    X(xrSyncActions) X(xrGetActionStateVector2f) X(xrGetActionStateBoolean) \
    X(xrCreateActionSpace) X(xrLocateSpace) X(xrGetActionStatePose)

#define AFXVR_DECL(name) PFN_##name name##_ = nullptr;
AFXVR_XR_FUNCS(AFXVR_DECL)
#undef AFXVR_DECL

PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR_ = nullptr;

// Where the loader is looked for, in order, and all of it relative to something rather
// than absolute.
//
// AFXVR_OPENXR_LOADER first, so an installation this code has never heard of has somewhere
// to say so. Then next to THIS DLL: the hook and the loader ship in one directory and the
// person who unpacked the zip chose where that is. Then the bare name, which lets
// LoadLibrary search the process's own directories and finds one only if it happens to sit
// next to the game.
//
// It used to be a literal "D:\Dev\cs2-vr-tools\openxr\...". That is one developer's disk,
// and it is the first thing that has to go before a stranger can run this at all
// (docs/07-release-plan.md, phase 0).
//
// Two entries and not one because the development tree keeps the hook in HLAE's x64
// directory with the loader a level above, while a release puts both in one folder. One
// table, not two code paths.
//
// Note this is the *loader*, not the runtime. Which runtime the loader then picks is the
// registry's business, or XR_RUNTIME_JSON's -- see scripts/openxr-runtime.ps1.
const wchar_t * const kLoaderBesideHook[] = {
    L"\\openxr_loader.dll",
    L"\\..\\openxr_loader.dll",
};

const wchar_t * const kLoaderByName = L"openxr_loader.dll";

// This DLL's own full path.
//
// From the address of something inside it rather than a handle remembered in DllMain: this
// translation unit has no DllMain, and asking for the module by name would find whichever
// AfxHookSource2.dll the loader happened to have mapped first.
bool HookModulePath(wchar_t * out, size_t outSize) {
    HMODULE self = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&kLoaderByName, &self)) {
        return false;
    }
    DWORD written = GetModuleFileNameW(self, out, (DWORD)outSize);
    return 0 != written && written < outSize;
}

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

    // Next to this DLL, which is where a release puts it.
    wchar_t self[MAX_PATH];
    self[0] = 0;
    if (!g_hLoader && HookModulePath(self, MAX_PATH)) {
        for (int i = 0; !g_hLoader && i < _countof(kLoaderBesideHook); i++) {
            wchar_t candidate[MAX_PATH];
            if (!AfxVrMath::PathRelativeToFile(self, 0, kLoaderBesideHook[i], candidate, MAX_PATH)) {
                continue;
            }
            g_hLoader = LoadLibraryW(candidate);
            if (g_hLoader) advancedfx::Message("AFXVR: OpenXR loader: %ls\n", candidate);
        }
    }

    if (!g_hLoader) g_hLoader = LoadLibraryW(kLoaderByName);

    if (!g_hLoader) {
        advancedfx::Warning(
            "AFXVR: could not load openxr_loader.dll.\n"
            "AFXVR: Looked next to this DLL, which is \"%ls\".\n"
            "AFXVR: Put it there, or set AFXVR_OPENXR_LOADER to its full path.\n",
            self[0] ? self : L"(unknown - this module's own path could not be read)");
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
// Rotate a vector by the inverse of a unit quaternion: world space into the frame that
// quaternion describes.
void RotateIntoFrame(const XrQuaternionf & q, float x, float y, float z,
                     float & ox, float & oy, float & oz) {
    // The conjugate is the inverse for a unit quaternion.
    float qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w;
    float tx = 2.0f * (qy * z - qz * y);
    float ty = 2.0f * (qz * x - qx * z);
    float tz = 2.0f * (qx * y - qy * x);
    ox = x + qw * tx + (qy * tz - qz * ty);
    oy = y + qw * ty + (qz * tx - qx * tz);
    oz = z + qw * tz + (qx * ty - qy * tx);
}

// An OpenXR orientation as Source's (pitch, yaw, roll) in degrees. Source inverts pitch
// and roll relative to OpenXR; yaw agrees.
void XrQuatToSourceAngles(const XrQuaternionf & q, float & dPitch, float & dYaw, float & dRoll) {
    double sinPitch = 2.0 * (q.w * q.x - q.y * q.z);
    if (sinPitch > 1.0) sinPitch = 1.0;
    if (sinPitch < -1.0) sinPitch = -1.0;

    double pitch = asin(sinPitch);
    double yaw   = atan2(2.0 * (q.w * q.y + q.z * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    double roll  = atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.x * q.x + q.z * q.z));

    const double r2d = 180.0 / M_PI;
    dPitch = (float)(-pitch * r2d);
    dYaw   = (float)( yaw   * r2d);
    dRoll  = (float)(-roll  * r2d);
}

void XrPoseToEye(const XrPosef & pose, const XrPosef & base,
                 float & right, float & forward, float & up,
                 float & dPitch, float & dYaw, float & dRoll) {
    // The offset has to be expressed in the HEAD's frame, because that is the frame the
    // caller applies it in - it multiplies these by the game camera's own right, forward
    // and up vectors. Handing over the raw world-space difference means the head rotation
    // is applied a second time, and the eyes separate along a direction that swings as the
    // viewer looks around: fusable straight ahead, worse the further the head turns or
    // pitches. That is exactly how it presents, and it is what this rotation fixes.
    float lx, ly, lz;
    RotateIntoFrame(base.orientation,
                    pose.position.x - base.position.x,
                    pose.position.y - base.position.y,
                    pose.position.z - base.position.z,
                    lx, ly, lz);

    right   =  lx * kUnitsPerMetre;
    up      =  ly * kUnitsPerMetre;
    forward = -lz * kUnitsPerMetre;

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

// The arithmetic all four of these do lives in MirvVrMath.h, where tests/ compiles it
// without a headset. What is left here is the part that cannot: reading an XrFovf, and
// reading the globals that a person wearing the headset has been turning.
float SymmetricFovDegrees(const XrFovf & fov) {
    return AfxVrMath::SymmetricFovDegrees(fov.angleLeft, fov.angleRight);
}

// A headset's frustum is not centred on the eye's forward axis. The Quest 3 reports
// [-54, +40] horizontally for the left eye and [-40, +54] for the right: the same 94
// degrees, but with the centre 7 degrees outward on each side, because the lens sits
// outboard of the pupil.
//
// CS2 can only render a frustum centred on its camera, so the centre has to be carried by
// rotating the camera instead. Rendering symmetrically about the forward axis and leaving
// the 7 degrees unaccounted leaves a constant angular offset between the eyes - 14 degrees
// of it, opposite in sign - which is independent of distance. That is exactly how it
// presents: a near wall fuses, because real parallax swamps it, and a far one does not,
// because at distance there is nothing left but the error.
struct FrustumCentre {
    float yawDegrees;    // positive turns left, matching both OpenXR and Source
    float pitchDegrees;  // Source sense: positive looks down
    float halfHorizontal;
    float halfVertical;
};

// On by default: rendering without it leaves 14 degrees of constant angular divergence,
// which no eye-separation setting can compensate because the error does not vary with
// distance. Off restores the old behaviour for comparison.
// 0 off, +1 outward as the runtime reports, -1 inverted.
//
// OFF, and it should stay off. The idea was that since the headset's frustum is not
// centred on the eye's forward axis - [-54, +40] for the left eye, mirrored for the right
// - and CS2 can only render a centred frustum, the offset should be carried by turning the
// camera instead.
//
// It is wrong, and wrong in an instructive way. The Quest 3 reports the *same orientation*
// for both eyes; the asymmetry is in the frustum bounds alone. Turning each camera outward
// introduces a relative rotation between the eyes that the displays do not have, and a
// relative rotation is the one thing two images cannot be fused through. It shows as a
// single horizontal line in the world appearing at two different angles - which is exactly
// how it was reported, with a drawing that made it unmistakable.
//
// The right answer for a renderer that can only do symmetric frustums is the original one:
// render the smallest symmetric frustum that contains the asymmetric one and report
// honestly that that is what was rendered. It wastes a seventh of the pixels and is
// correct.
int g_CentreFrustum = 0;

// Roll from the headset, into the game camera. Also a sign that has never been tested on
// its own: 0 forces it flat, -1 inverts it. A mismatch between the roll we render with and
// the roll we report shows up as the runtime rotating the submitted image, with black
// wedges where it has no pixels.
int g_RollMode = 1;

XrQuaternionf QuatAxisAngle(float ax, float ay, float az, float radians) {
    float s = sinf(0.5f * radians);
    XrQuaternionf q;
    q.x = ax * s; q.y = ay * s; q.z = az * s; q.w = cosf(0.5f * radians);
    return q;
}

XrQuaternionf QuatMul(const XrQuaternionf & a, const XrQuaternionf & b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

FrustumCentre CentreOfFrustum(const XrFovf & fov) {
    const double r2d = 180.0 / M_PI;
    FrustumCentre c;

    // Signed fov angles are measured from forward, positive to the right and up. A centre
    // at a negative horizontal angle is to the left, which is a positive yaw.
    c.yawDegrees   = (float)(-0.5 * (fov.angleRight + fov.angleLeft) * r2d);
    c.pitchDegrees = (float)(-0.5 * (fov.angleUp + fov.angleDown) * r2d);

    c.halfHorizontal = (float)(0.5 * (fov.angleRight - fov.angleLeft) * r2d);
    c.halfVertical   = (float)(0.5 * (fov.angleUp - fov.angleDown) * r2d);
    return c;
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
//
// OFF, and that is a measurement, not a preference. The 4:3 model above is correct for the
// number the view-setup TRAMPOLINE reads and for the matrix the engine builds once a
// frame - experiment 06 checked it against the engine's own projection matrix. It is
// wrong for the field the per-pass write goes into, because by the time a pass runs the
// engine has already rescaled that field in place for the window's aspect. Writing the
// 4:3 number there scales it a second time.
//
// Found by putting the dial on the triggers and handing it to someone wearing the headset
// (experiment 18). They converged on asking for 0.853 of what the aspect fix wanted;
// 108/127.3 = 0.848, which is inside one step of the dial. Three rounds of reading
// disassembly had not settled it.
//
// The function stays, because it is still the right arithmetic for anything written at the
// trampoline, should that ever be wanted again.
bool g_SourceAspectFix = false;

// A multiplier on the angle handed to the ENGINE, and on nothing else.
//
// Everything else in this file that touches the field of view moves the ask and the claim
// together, which - with the crop in place - is invisible by construction: the image and
// the rectangle cut out of it both change, and the result is identical apart from
// sharpness. That was the point of it, as an invariance test. This is the opposite dial,
// and the one that matters: it changes how much world goes into the image while the
// runtime keeps being told the truth about the frustum it will be shown in.
//
// It exists because of a measurement nobody has been able to make from a desk. Source's
// fov is defined at 4:3 and the engine widens it for the window's aspect, so to render 108
// degrees on this portrait buffer we ask for 127.3. What is not known is whether the field
// the per-pass write goes into is still in that convention by the time a pass runs, or
// whether the engine has already rescaled it in place - in which case we ask 127.3 and get
// 127.3, the crop assumes 108, and the world is shown at tan54/tan63.65 = 0.68 of its size
// with a residue on every head turn.
//
// A person in the headset can find the right number in thirty seconds where the code
// cannot: look at a far corner, turn and nod, and adjust until the corner stays nailed to
// the world. Ignore how big things look; the criterion is whether the world moves against
// the head. If it settles near 0.85 the field is already scaled and the aspect fix is
// double-counting; if near 1.0 the 4:3 model was right and the complaint is elsewhere.
//
// Deliberately NOT reset by mirv_vr_reset. It is a calibration, not a setting, and a reset
// that silently eats a calibration is how an evening goes.
float g_AskScale = 1.0f;
const float kAskScaleStep = 1.01f;


float AspectOfImage() {
    if (g_SwapchainWidth && g_SwapchainHeight) {
        return (float)g_SwapchainWidth / (float)g_SwapchainHeight;
    }
    return 1.0f;
}

// The engine's chain, forwards: what it actually renders when handed `askedDegrees`.
float RenderedFovForAsked(float askedDegrees) {
    return AfxVrMath::RenderedFovForAsked(askedDegrees, AspectOfImage());
}

// The angle to hand the engine so that it renders `wantedDegrees` horizontally.
float SourceFovForWanted(float wantedDegrees) {
    return AfxVrMath::SourceFovForWanted(wantedDegrees, AspectOfImage());
}

// What the frustum should be: the smallest symmetric one containing the runtime's
// asymmetric recommendation and still tall enough at this image's shape - unless an
// override says otherwise. AfxVrMath::ContainingFovDegrees carries the reasoning and the
// measurement it came from; what is here is the override and the calibration dial, which
// is everything about this function that a test could not see.
float WantedFovDegrees(const XrFovf & fov) {
    if (g_FovOverrideDegrees > 0.0f) {
        return AfxVrMath::ClampFovDegrees(g_FovOverrideDegrees * g_FovScale, 10.0f, 170.0f);
    }

    float degrees = AfxVrMath::ContainingFovDegrees(
        fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, AspectOfImage());

    return AfxVrMath::ClampFovDegrees(degrees * g_FovScale, 10.0f, 170.0f);
}

// What to hand the engine. Differs from WantedFovDegrees when the aspect fix is on, and by
// the calibration scale, which is the whole reason the two are separate functions.
float EffectiveFovDegrees(const XrFovf & fov) {
    float wanted = WantedFovDegrees(fov);
    float asked = g_SourceAspectFix ? SourceFovForWanted(wanted) : wanted;

    // 178 rather than 170: undoing the 4:3 convention inflates the number, and an ask that
    // is legitimately wider than any field of view we wanted is not the same thing as a bug.
    return AfxVrMath::ClampFovDegrees(asked * g_AskScale, 10.0f, 178.0f);
}

void DestroySwapchains();

// Eight by eight of opaque white, for the pointer dot. Declared here because the
// swapchains own its lifetime: it has to match their format, so it goes when they do.
ID3D11Texture2D * g_CursorWhite = nullptr;

void ReleaseCursorTexture() {
    if (g_CursorWhite) { g_CursorWhite->Release(); g_CursorWhite = nullptr; }
}

bool EnsureSwapchains(ID3D11Texture2D * pTexture) {
    if (!pTexture) return false;

    D3D11_TEXTURE2D_DESC probe = {};
    pTexture->GetDesc(&probe);
    if (probe.Width && probe.Height) {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        g_MenuAspect = (float)probe.Height / (float)probe.Width;
    }

    // The back buffer can change under us, and does: CS2's own video settings page changes
    // resolution without a restart, and the operator used it - from inside the headset,
    // through the menu this feature exists to show. The swapchains were created once and
    // never again, so afterwards every CopyResource had a source of one size and a
    // destination of another, which does nothing at all and silently. Worn, the menu
    // simply froze.
    //
    // Recreating here is also the answer to a question this project had filed as open:
    // whether the back buffer size can change while a session runs. It can, and the
    // game's own settings page is how.
    if (g_Swapchain[0] != XR_NULL_HANDLE) {
        if (probe.Width == g_SwapchainWidth && probe.Height == g_SwapchainHeight) return true;

        advancedfx::Message("AFXVR: back buffer is now %ux%u, was %ux%u - rebuilding the swapchains.\n",
            probe.Width, probe.Height, g_SwapchainWidth, g_SwapchainHeight);
        DestroySwapchains();
        ReleaseCursorTexture();
    }


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

    g_SwapchainFormat = chosen;

    for (int eye = 0; eye < kSwapchainCount; eye++) {
        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = (int64_t)chosen;
        info.sampleCount = 1;
        info.width  = (kCursorSwapchain == eye) ? kCursorTexels : desc.Width;
        info.height = (kCursorSwapchain == eye) ? kCursorTexels : desc.Height;
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

    advancedfx::Message("AFXVR: swapchains %ux%u, back buffer format %i -> swapchain %i, %u images each (two eyes and a panel).\n",
        desc.Width, desc.Height, (int)desc.Format, (int)chosen, (unsigned)g_SwapchainImages[0].size());
    return true;
}

void DestroySwapchains() {
    // A copy belongs to an image in these swapchains, not merely to this frame. Keeping
    // the flag after a resize submitted a new image that had never been released, and
    // xrEndFrame rejected the world along with the stale panel.
    g_PanelCopied = false;
    g_PanelTicket = 0;
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
    g_MenuButtonAction  = MakeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "gamemenu", "Game menu");

    // The pointer. Two subaction paths on one action, so left and right are the same idea.
    g_HandPath[0] = Path("/user/hand/left");
    g_HandPath[1] = Path("/user/hand/right");
    {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(info.actionName, "aim");
        strcpy_s(info.localizedActionName, "Pointer");
        info.countSubactionPaths = 2;
        info.subactionPaths = g_HandPath;
        if (!Check(xrCreateAction_(g_ActionSet, &info, &g_AimAction), "xrCreateAction (aim)")) {
            g_AimAction = XR_NULL_HANDLE;
        }
    }


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

        // Where each hand points. "aim" is the pointing pose; "grip" is where the hand
        // is, about twenty degrees of wrist away from it.
        { g_AimAction,         Path("/user/hand/left/input/aim/pose") },
        { g_AimAction,         Path("/user/hand/right/input/aim/pose") },

        // The game's own menu, on a screen, over whatever is loaded.
        { g_MenuButtonAction,  Path("/user/hand/left/input/menu/click") },
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
    // Two layouts, because there are two things to do and the same eight buttons have to
    // serve both. Which one is live is not a setting: the hook decides from what the game
    // is doing (a demo is playing, a map is loaded, a cursor is showing), so both are
    // printed and the live one is marked. PgUp is how somebody wearing the headset gets
    // this into console.log, which is the only place they can read it afterwards.
    //
    // This is also the table the README copies. It has been wrong once already - it still
    // claimed the right stick click recentred, long after a short press had become show and
    // hide the HUD - and a wrong table is worse than none: the person reading it cannot see
    // the code to check.
    bool playing = (AfxVrMath::kVrModePlay == g_Mode.mode);

    advancedfx::Message(
        "\n"
        "Controllers -- currently %s\n"
        "\n"
        "WATCHING a demo%s\n"
        "  left hand -- who you are watching, and where you are standing\n"
        "    stick            fly, in the direction you are looking\n"
        "    stick click      next camera mode: first person, chase, free\n"
        "    trigger          %s\n"
        "    grip             free look on / off  (currently %s)\n"
        "    X                back %.0f s\n"
        "    Y                forward %.0f s\n"
        "  right hand -- how time runs, and where the camera points\n"
        "    stick            turn%s, and rise or descend\n"
        "    stick click      short: hide or show the HUD.  long: recentre\n"
        "    trigger          %s\n"
        "    grip             back onto the player\n"
        "    A                pause / resume\n"
        "    B                slow motion / normal speed  (currently %s)\n",
        playing ? "PLAYING" : "watching, or idle",
        playing ? "" : "   <- live",
        (kTriggersPlayers == g_TriggerMode) ? "previous player"
      : (kTriggersSeek    == g_TriggerMode) ? "seek back"
                                            : "narrow the view",
        AfxVr_GetFreeLook() ? "on" : "off",
        g_SeekSeconds,
        g_SeekSeconds,
        (0.0f < g_SnapTurnDegrees) ? " (snaps)" : " (smoothly)",
        (kTriggersPlayers == g_TriggerMode) ? "next player"
      : (kTriggersSeek    == g_TriggerMode) ? "seek forward"
                                            : "widen the view",
        g_SlowMotion ? "slow" : "normal");

    advancedfx::Message(
        "\n"
        "PLAYING a map%s\n"
        "  left hand\n"
        "    stick            walk, relative to your body\n"
        "    stick click      slow walk on / off  (currently %s)\n"
        "    trigger          use: defuse, plant, open, pick a gun off the floor\n"
        "    grip             crouch  (held)\n"
        "    X / Y            pick a team, while the picker is up\n"
        "  right hand\n"
        "    stick            turn the body\n"
        "    stick click      reload\n"
        "    trigger          fire  (held)\n"
        "    grip             jump  (held)\n"
        "    A                next weapon\n"
        "    B                alternative fire  (held)\n"
        "  aim               %s\n"
        "\n"
        "EITHER WAY\n"
        "    left menu        short: CS2's own window on a screen.  long: Escape\n"
        "\n"
        "What the triggers do while watching is mirv_vr_triggers: players, seek or fov.\n"
        "Aiming is mirv_vr_aim hand|stick|off; vr.cfg asks for hand.\n"
        "Speeds and feel: mirv_vr_speed, mirv_vr_turn, mirv_vr_stick, mirv_vr_seek.\n",
        playing ? "   <- live" : "",
        g_HeldKey[kHeldWalk] ? "walking" : "running",
        (kAimHand  == g_AimMethod) ? "the right controller, inside a window around where you look"
      : (kAimStick == g_AimMethod) ? "the right stick"
                                   : "OFF -- nothing moves the mouse (mirv_vr_aim hand)");
}

bool AttachActions() {
    if (g_ActionsAttached) return true;
    if (XR_NULL_HANDLE == g_ActionSet || XR_NULL_HANDLE == g_Session) return false;

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &g_ActionSet;
    if (!Check(xrAttachSessionActionSets_(g_Session, &attach), "xrAttachSessionActionSets")) return false;

    g_ActionsAttached = true;

    // One space per hand, made after attaching because an action space needs an attached
    // action. A failure here costs the pointer and nothing else, so it warns rather than
    // refusing the whole session: the sticks and buttons are what a viewer needs first.
    if (XR_NULL_HANDLE != g_AimAction) {
        for (int hand = 0; hand < 2; hand++) {
            XrActionSpaceCreateInfo info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
            info.action = g_AimAction;
            info.subactionPath = g_HandPath[hand];
            info.poseInActionSpace.orientation.w = 1.0f;
            if (!Check(xrCreateActionSpace_(g_Session, &info, &g_AimSpace[hand]),
                       "xrCreateActionSpace (aim)")) {
                g_AimSpace[hand] = XR_NULL_HANDLE;
            }
        }
    }
    if (XR_NULL_HANDLE == g_AimSpace[0] && XR_NULL_HANDLE == g_AimSpace[1]) {
        advancedfx::Warning("AFXVR: no pointer: neither hand's aim pose could be tracked.\n");
    }

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

// The window the game presents into. Asked of the swap chain rather than remembered,
// because a swap chain can be recreated and a remembered handle then points at nothing.
// The window the game presents into. Defined below; the pointer needs it.
HWND SwapChainWindow();

// Where the two hands point, at the same instant the views were located.
void LocateAim(XrTime displayTime) {
    for (int hand = 0; hand < 2; hand++) {
        g_AimValid[hand] = false;
        if (XR_NULL_HANDLE == g_AimSpace[hand] || !xrLocateSpace_) continue;

        XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace_(g_AimSpace[hand], g_Space, displayTime, &location))) continue;

        // Both flags. A pose that is only "tracked" without being valid is last frame's,
        // and a pointer that keeps pointing after the controller has gone to sleep is
        // worse than no pointer.
        const XrSpaceLocationFlags wanted =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (wanted != (location.locationFlags & wanted)) continue;

        g_AimPose[hand] = location.pose;
        g_AimValid[hand] = true;
    }
}

// How a hit on the sheet is turned into a mouse.
//
// Two ways, because which one Panorama listens to is a question only a headset can answer,
// and rebuilding to find out costs three minutes a try.
//
// "post" sends WM_MOUSEMOVE and the button messages straight to the game's own window.
// Nothing global moves, so no other application's cursor can be disturbed and the game
// does not have to be in front - which matters more than it sounds: the first worn attempt
// clicked nothing at all because the window in front was the PowerShell one the game had
// been launched from, and the guard below dropped every event. It is also the only route
// that can reach a row the real cursor cannot: the coordinates are packed into lParam, not
// clamped to a screen, and the layout is taller than the display.
//
// "send" is the real system cursor, which is what a game that reads GetCursorPos or raw
// input needs. It requires the game to be in front, and it moves the actual pointer.
enum PointerInput { kPointerPost = 0, kPointerSend = 1 };
// The system cursor by default, because that is the one proven worn: with the game in
// front, aiming and clicking CS2's menu works. "post" is the untested alternative that
// would remove the focus requirement altogether - a dial, not a default, until a headset
// says otherwise.
int g_PointerInput = kPointerSend;

// One wheel notch at the point the ray is on, by the same route the click took.
//
// It has to follow mirv_vr_pointer, and until today it did not: the click was posted to
// the window while the wheel went out as global input, which needs the game in front and
// is normalised against the desktop. Half the pointer immune to a display-mode change and
// half of it not is worse than either, because the half that breaks breaks silently.
//
// WM_MOUSEWHEEL is the odd one out among the mouse messages: its lParam is in SCREEN
// coordinates, not client ones. Getting that wrong scrolls whatever is under the corner of
// the window instead of what is being pointed at.
void PostWheelToSheet(float u, float v, int notches) {
    HWND hwnd = SwapChainWindow();
    if (!hwnd || 0 == notches) return;

    RECT client = {};
    if (!GetClientRect(hwnd, &client)) return;
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    POINT point;
    point.x = (int)(u * (float)width);
    point.y = (int)(v * (float)height);
    if (point.x < 0) point.x = 0;
    if (point.y < 0) point.y = 0;
    if (point.x >= width)  point.x = width - 1;
    if (point.y >= height) point.y = height - 1;
    if (!ClientToScreen(hwnd, &point)) return;

    PostMessageW(hwnd, WM_MOUSEWHEEL,
                 MAKEWPARAM(0, (short)(notches * WHEEL_DELTA)),
                 MAKELPARAM(point.x, point.y));
}

void MoveMouseToSheet(float u, float v, bool clickDown, bool clickUp) {
    HWND hwnd = SwapChainWindow();
    if (!hwnd) return;

    RECT client = {};
    if (!GetClientRect(hwnd, &client)) return;
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    int x = (int)(u * (float)width);
    int y = (int)(v * (float)height);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= width)  x = width - 1;
    if (y >= height) y = height - 1;

    if (kPointerPost == g_PointerInput) {
        LPARAM where = MAKELPARAM(x, y);
        PostMessageW(hwnd, WM_MOUSEMOVE, g_CursorPressed ? MK_LBUTTON : 0, where);
        if (clickDown) PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, where);
        if (clickUp)   PostMessageW(hwnd, WM_LBUTTONUP, 0, where);
        return;
    }

    // The system cursor. Guarded on the game being in front, exactly like the synthetic
    // keys: moving another application's pointer because a hand drifted across a panel is
    // the kind of thing nobody forgives.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;

    POINT point;
    point.x = x;
    point.y = y;
    if (!ClientToScreen(hwnd, &point)) return;

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (screenW <= 1 || screenH <= 1) return;

    INPUT inputs[3] = {};
    int count = 0;

    inputs[count].type = INPUT_MOUSE;
    inputs[count].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    inputs[count].mi.dx = (LONG)((point.x - screenX) * 65535.0 / (screenW - 1));
    inputs[count].mi.dy = (LONG)((point.y - screenY) * 65535.0 / (screenH - 1));
    count++;

    if (clickDown) { inputs[count].type = INPUT_MOUSE; inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; count++; }
    if (clickUp)   { inputs[count].type = INPUT_MOUSE; inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTUP;   count++; }

    SendInput(count, inputs, sizeof(INPUT));
}

void EngineThread_PlaceMenu() {
    if (!g_Mode.sheet) return;

    XrPosef mid = {};
    float aspect;
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        if (!g_ViewsValid) return;
        mid = g_Views[0].pose;
        mid.position.x = 0.5f * (g_Views[0].pose.position.x + g_Views[1].pose.position.x);
        mid.position.y = 0.5f * (g_Views[0].pose.position.y + g_Views[1].pose.position.y);
        mid.position.z = 0.5f * (g_Views[0].pose.position.z + g_Views[1].pose.position.z);
        aspect = g_MenuAspect;
    }

    // Placed once, where the viewer was looking when the menu came up, and then left
    // alone. A screen that follows the eyes cannot be looked away from, and a menu is
    // exactly the thing somebody wants to glance away from and back to.
    if (!g_MenuPlaced) {
        const XrQuaternionf & q = mid.orientation;
        double yaw = atan2(2.0 * (q.w * q.y + q.z * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));

        g_MenuPose.position.x = mid.position.x - (float)sin(yaw) * g_MenuDistanceMetres;
        g_MenuPose.position.y = mid.position.y;
        g_MenuPose.position.z = mid.position.z - (float)cos(yaw) * g_MenuDistanceMetres;
        g_MenuPose.orientation.x = 0.0f;
        g_MenuPose.orientation.y = (float)sin(yaw * 0.5);
        g_MenuPose.orientation.z = 0.0f;
        g_MenuPose.orientation.w = (float)cos(yaw * 0.5);
        g_MenuPlaced = true;

        advancedfx::Message("AFXVR: menu screen placed, %.0f degrees wide at %.1f m.\n",
            g_MenuWidthDegrees, g_MenuDistanceMetres);
    }

    float halfWidth = tanf(0.5f * g_MenuWidthDegrees * (float)(M_PI / 180.0));
    g_MenuSize.width = 2.0f * g_MenuDistanceMetres * halfWidth;
    // The source aspect, whatever it is. The window is the eye size, so the sheet is
    // taller than it is wide; stretching text to a friendlier shape is worse than an odd
    // one.
    g_MenuSize.height = g_MenuSize.width * aspect;
}

void PublishCursor(const CursorSnapshot & cursor) {
    std::lock_guard<std::mutex> lock(g_ViewMutex);
    g_Cursor = cursor;
}

// Point at the menu screen, and click it.
//
// Runs on the engine thread, with the aim poses located this frame and the screen's pose
// already decided, so the ray and the thing it points at are from one instant. The render
// thread only draws the answer.
// Defined below, with the rest of the synthetic input. Wanted here so the pointer can
// scroll the page it is pointing at.
void QueueMouseWheel(int notches);

// When the last wheel notch went out, so a stick held over is a scroll rather than a fling.
ULONGLONG g_LastScrollMs = 0;

void EngineThread_PointAtMenu() {
    bool wasHit;
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        wasHit = g_Cursor.hit;
    }
    CursorSnapshot cursor;

    // A loaded map still has a sheet when team select or the buy menu owns the mouse.
    // Gating this on menu-only mode took gameplay input away without supplying a click.
    // Let go when the sheet closes too, or a trigger held over the last button stays down.
    if (!g_Mode.sheet || !g_Mode.pointer || !g_MenuPlaced) {
        if (g_CursorPressed) {
            MoveMouseToSheet(g_CursorU, g_CursorV, false, true);
            g_CursorPressed = false;
        }
        PublishCursor(cursor);
        return;
    }

    float quad[4] = { g_MenuPose.orientation.x, g_MenuPose.orientation.y,
                      g_MenuPose.orientation.z, g_MenuPose.orientation.w };
    float centre[3] = { g_MenuPose.position.x, g_MenuPose.position.y, g_MenuPose.position.z };

    float width = g_MenuSize.width;
    float height = g_MenuSize.height;

    // The right hand wins a tie, because most people point with it and a cursor that
    // flickers between two hands resting on a desk is unusable. Otherwise the nearer hit.
    float bestT = 0.0f;
    int bestHand = -1;
    float bestU = 0.0f, bestV = 0.0f;

    for (int hand = 0; hand < 2; hand++) {
        if (!g_AimValid[hand]) continue;

        float origin[3] = { g_AimPose[hand].position.x, g_AimPose[hand].position.y,
                            g_AimPose[hand].position.z };

        // The aim pose points along its own -Z, the same way a view does.
        float direction[3];
        AfxVrMath::QuatRotate(g_AimPose[hand].orientation.x, g_AimPose[hand].orientation.y,
                              g_AimPose[hand].orientation.z, g_AimPose[hand].orientation.w,
                              0.0f, 0.0f, -1.0f,
                              direction[0], direction[1], direction[2]);

        float t = 0.0f, u = 0.0f, v = 0.0f;
        if (!AfxVrMath::RayQuadHit(origin, direction, centre, quad, width, height, t, u, v)) continue;
        if (bestHand >= 0 && !(t < bestT) && hand != 1) continue;

        bestT = t; bestHand = hand; bestU = u; bestV = v;
    }

    // The trigger of the hand that is pointing. While a sheet owns the mouse the seek
    // actions are free, even over a loaded map, and the trigger means what it means
    // everywhere else: click the thing under the pointer.
    bool pressed = false;
    if (bestHand >= 0) {
        pressed = GetPressed(1 == bestHand ? g_SeekForwardAction : g_SeekBackAction);
    }

    if (bestHand < 0) {
        // Let go of a button that was held when the ray left the panel, or the game is
        // left with the mouse down.
        if (g_CursorPressed) {
            MoveMouseToSheet(g_CursorU, g_CursorV, false, true);
            g_CursorPressed = false;
        }
        if (wasHit) advancedfx::Message("AFXVR: pointer off the screen.\n");
        PublishCursor(cursor);
        return;
    }

    g_CursorHand = bestHand;
    g_CursorU = bestU;
    g_CursorV = bestV;
    cursor.pose.orientation = g_MenuPose.orientation;

    // The hit point, lifted a little towards the viewer so the dot is not fighting the
    // screen for the same depth.
    float normal[3];
    AfxVrMath::QuatRotate(quad[0], quad[1], quad[2], quad[3], 0.0f, 0.0f, 1.0f,
                          normal[0], normal[1], normal[2]);
    float right[3], up[3];
    AfxVrMath::QuatRotate(quad[0], quad[1], quad[2], quad[3], 1.0f, 0.0f, 0.0f, right[0], right[1], right[2]);
    AfxVrMath::QuatRotate(quad[0], quad[1], quad[2], quad[3], 0.0f, 1.0f, 0.0f, up[0], up[1], up[2]);

    float localX = (bestU - 0.5f) * width;
    float localY = (0.5f - bestV) * height;
    cursor.pose.position.x = centre[0] + right[0] * localX + up[0] * localY + normal[0] * kCursorLiftMetres;
    cursor.pose.position.y = centre[1] + right[1] * localX + up[1] * localY + normal[1] * kCursorLiftMetres;
    cursor.pose.position.z = centre[2] + right[2] * localX + up[2] * localY + normal[2] * kCursorLiftMetres;
    cursor.hit = true;
    PublishCursor(cursor);

    bool pressEdge   =  pressed && !g_CursorPressed;
    bool releaseEdge = !pressed &&  g_CursorPressed;
    g_CursorPressed = pressed;

    MoveMouseToSheet(bestU, bestV, pressEdge, releaseEdge);

    // Scrolling, on the stick of whichever hand is pointing.
    //
    // Found by trying to start a bot match from inside the headset and being unable to.
    // The menu is not cropped - CS2 lays the whole of it out inside the window - but the
    // PLAY page's map grid is simply LONGER than any window, with a second row of maps and
    // the button that starts the game below it. On a monitor you turn the wheel without
    // thinking about it; in here there was no wheel at all, so the bottom of a page was
    // unreachable and a mode that works perfectly looked broken.
    //
    // The wheel has to arrive while the pointer is over the thing being scrolled, which it
    // is: MoveMouseToSheet has just put the cursor there. One notch per step and a rate
    // limit, because Panorama animates each notch and a stream of them at the frame rate
    // flings the page past whatever was being read.
    if (bestHand >= 0) {
        float sx = 0.0f, sy = 0.0f;
        if (GetVec2(1 == bestHand ? g_TurnAction : g_MoveAction, sx, sy)) {
            float amount = AfxVrMath::ApplyDeadzone(sy, g_StickDeadzone);
            ULONGLONG now = GetTickCount64();
            // Full deflection is about eight notches a second, a gentle push about three.
            ULONGLONG gap = (ULONGLONG)(120.0f + 220.0f * (1.0f - fabsf(amount)));
            if (0.0f != amount && now - g_LastScrollMs >= gap) {
                g_LastScrollMs = now;
                int notches = amount > 0.0f ? 1 : -1;
                // By whichever route the click went, so the two cannot disagree.
                if (kPointerPost == g_PointerInput) PostWheelToSheet(bestU, bestV, notches);
                else                                QueueMouseWheel(notches);
            }
        }
    }

    if (g_AfxVrFrameIndex < g_AfxVrLogUntilFrame) {
        advancedfx::Message("AFXVR: pointer hand=%s t=%.2f m u=%.3f v=%.3f trigger=%i\n",
            bestHand ? "right" : "left", bestT, bestU, bestV, pressed ? 1 : 0);
    }
}

HWND SwapChainWindow() {
    if (!g_pSwapChain) return NULL;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(g_pSwapChain->GetDesc(&desc))) return NULL;
    return desc.OutputWindow;
}

// Aiming, firing and changing weapons, in a game rather than a recording.
//
// All of it goes through the same queue the keys do, drained on the engine thread, because
// the input is read on whichever thread is running the frame loop and SendInput from two
// threads at once is a race nobody would ever find.
struct PendingMouse {
    int dx, dy;          // relative counts, as a mouse would send
    int wheel;           // notches, positive is away from the hand
    bool leftDown, leftUp, rightDown, rightUp;
};
PendingMouse g_PendingMouse = {};

void QueueMouseMove(int dx, int dy) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    g_PendingMouse.dx += dx;
    g_PendingMouse.dy += dy;
}

void QueueMouseWheel(int notches) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    g_PendingMouse.wheel += notches;
}

void QueueMouseButton(bool right, bool down) {
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    if (right) { if (down) g_PendingMouse.rightDown = true; else g_PendingMouse.rightUp = true; }
    else       { if (down) g_PendingMouse.leftDown  = true; else g_PendingMouse.leftUp  = true; }
}

// Where the viewer's own forward points, in the room.
//
// Not zero. g_Space is LOCAL space, and its forward is wherever the headset happened to be
// pointing the last time the Oculus runtime recentred - which is not where the chair
// faces. Taking it for the body's forward centres the aiming cone that many degrees off to
// one side: the resting crosshair is not in front of the viewer, the thirty degrees of
// free aim is lopsided, and past thirty the very first touch of the stick snaps the world.
//
// The right answer is already on screen. The HUD panel is placed in front of the viewer
// when the session comes up, and its yaw IS "where I am facing". One reference for the
// HUD, the cone and the crosshair, so they cannot disagree about which way forward is.
float RoomForwardYawRadians() {
    if (!g_PanelPlaced) return 0.0f;
    return (float)(2.0 * atan2((double)g_PanelPose.orientation.y,
                               (double)g_PanelPose.orientation.w));
}

// The same, in degrees, as a world yaw: the direction the body is taken to face.
float BodyForwardWorldDegrees() {
    return AfxVrMath::NormalizeDegrees(
        AfxVr_YawOffsetDegrees() + (float)(RoomForwardYawRadians() * 180.0 / M_PI));
}

// What a mouse count is worth, learned from ordinary aiming, and the short closed loop
// that uses it to bring the aim to where the viewer is looking.
//
// Both written as pure state machines with tests; everything here is the wiring. The
// estimator exists because a count is not a degree - the conversion is the player's own
// sensitivity, and it changes when a weapon is scoped - so no number typed into a config
// can mean the same thing twice. The servo exists because stick aiming in a headset is
// only bearable if you can look at something and have the aim come to it.
AfxVrMath::CountGainEstimator g_GainYaw;
AfxVrMath::CountGainEstimator g_GainPitch;
AfxVrMath::AimServo g_ServoYaw;
AfxVrMath::AimServo g_ServoPitch;
bool g_GainInitialised = false;

// Frozen at the click, not followed live: if the target chased the gaze, head movement
// during the first frames would read as the first correction landing and the servo would
// mis-measure its own delay.
float g_GazeTargetYaw = 0.0f;
float g_GazeTargetPitch = 0.0f;

// Aim-to-gaze is on, but it refuses itself until the gain has been measured, which is a
// second or so of ordinary aiming. A press before then does nothing rather than something
// wrong.
bool g_AimGaze = true;

// The gain to act on, which is not always the gain that was measured.
//
// A measurement can be poisoned by a view that could not move: dead, spectating a fixed
// camera, frozen at a round start, or pitch against its own stop. Counts go out, the angle
// does not change, and the honest conclusion from that evidence is "a count is worth zero
// degrees" - after which the tracker refuses a zero gain, sends nothing, and there is
// never any new evidence. The aim is dead for the rest of the session.
//
// Seen exactly that way from inside the headset, twice: "the crosshair has stopped moving
// with the stick", with the log reading `hand 145.7/1.7  game 135.0/-0.0  sent 0/0  gain
// 0.00000` - the hand alive, the game's view frozen, and nothing being sent.
//
// Not feeding those frames is the first line and is done elsewhere. This is the second: a
// number far below anything a real mouse setting produces is not a measurement, it is the
// residue of one, and the seed is a better answer than refusing to work.
float EffectiveGain(const AfxVrMath::CountGainEstimator & estimator, float seed) {
    float measured = estimator.DegreesPerCount();
    if (fabsf(measured) < 0.2f * fabsf(seed)) return seed;
    return measured;
}

const float kSeedGainYaw = -0.0275f;
const float kSeedGainPitch = 0.0275f;

void ResetAimLearning() {
    g_ServoYaw.Cancel();
    g_ServoPitch.Cancel();
    g_GainYaw = AfxVrMath::CountGainEstimator();
    g_GainPitch = AfxVrMath::CountGainEstimator();
    g_GainPitch.wraps = false;

    // Seeded, or nothing ever moves.
    //
    // Without a seed the servo refuses a zero gain and sends no counts; with no counts
    // the estimator has nothing to learn from; so the gain stays zero and hand aiming is
    // dead on arrival. A deadlock I built and did not notice, because the headset was
    // charging and the review found it instead.
    //
    // 0.0275 degrees per count is m_yaw 0.022 at sensitivity 1.25, which is an ordinary
    // setting. Being wrong by a factor of two only changes how fast the first correction
    // converges; the estimator measures the truth within a second of movement and
    // replaces it. Yaw is negative because a mouse moved right lowers a Source yaw.
    g_GainYaw.Seed(kSeedGainYaw);
    g_GainPitch.Seed(kSeedGainPitch);

    g_GainInitialised = true;
}


AfxVrMath::AimTracker g_TrackYaw;
AfxVrMath::AimTracker g_TrackPitch;
bool g_HandAimReady = false;     // the servo has been run, so the delay is known

// Whether the hands and the gun belong to the body or to the headset while playing. Asked
// for from inside the headset: "locking the hands to the headset is not a good idea".
bool g_PlayFrameBody = true;

// One line a second saying what hand aiming decided FROM. On, because the thing it is
// diagnosing cannot be told from a thing that looks identical: the crosshair slides across
// the picture when the ROOM turns, whether or not the hand is doing anything at all.
bool g_HandAimLog = true;

// Where the right hand points, as world angles, from the poses that travelled with this
// frame rather than from whatever the engine thread has since moved on to.
//
// Not the hand's direction copied onto the eye: the hand is half a metre from the eye, so
// two parallel lines miss by that much at every distance. Aim the eye at a point fifteen
// metres along the hand's ray instead, and the two cross where it matters.
bool HandAimWorld(float & outYaw, float & outPitch) {
    if (!g_AimValid[1]) return false;
    if (!g_PanelFollowValid) return false;

    float direction[3];
    AfxVrMath::QuatRotate(g_AimPose[1].orientation.x, g_AimPose[1].orientation.y,
                          g_AimPose[1].orientation.z, g_AimPose[1].orientation.w,
                          0.0f, 0.0f, -1.0f, direction[0], direction[1], direction[2]);

    const float reach = 15.0f;
    float px = g_AimPose[1].position.x + direction[0] * reach;
    float py = g_AimPose[1].position.y + direction[1] * reach;
    float pz = g_AimPose[1].position.z + direction[2] * reach;

    float ex = px - g_PanelFollowPos.x;
    float ey = py - g_PanelFollowPos.y;
    float ez = pz - g_PanelFollowPos.z;

    double horizontal = sqrt((double)ex * ex + (double)ez * ez);
    if (horizontal < 1e-4 && fabs((double)ey) < 1e-4) return false;

    // Out of the room and into the world, the same mapping the crosshair uses backwards.
    double xrYaw = atan2(-(double)ex, -(double)ez);
    double xrPitch = atan2((double)ey, horizontal);

    outYaw = AfxVrMath::NormalizeDegrees(
        BodyForwardWorldDegrees()
        + (float)((xrYaw - RoomForwardYawRadians()) * 180.0 / M_PI));
    // Source counts pitch positive downwards.
    outPitch = (float)(-xrPitch * 180.0 / M_PI);
    if (outPitch >  89.0f) outPitch =  89.0f;
    if (outPitch < -89.0f) outPitch = -89.0f;
    return true;
}

void StopHandAim() {
    g_HandAimReady = false;
    g_ServoYaw.Cancel();
    g_ServoPitch.Cancel();
    g_TrackYaw.Reset();
    g_TrackPitch.Reset();
}

// Aiming with the stick while the head keeps the picture.
//
// Decided by the person who wears it, against the alternative of making the head aim. The
// scheme is the one Quakespasm-OpenVR ships and Cyberpunk's VR port calls decoupled
// pitch, and the part that makes it bearable is the deadzone cone: inside thirty degrees
// the aim moves and the world does NOT. The crosshair walks across a still picture. Only
// at the edge of the cone does the body follow, and then in a snap, because a world
// rotating smoothly under a head that is not turning is what makes people ill.
float g_AimDeadzoneDegrees = 30.0f;
float g_AimSnapDegrees = 30.0f;

// How far off the centre of the view the hand may pull the aim. Inside this the crosshair
// moves with the hand; beyond it the hand is simply pointing outside the picture, and the
// way to shoot something there is to look at it.
float g_AimWindowDegrees = 22.0f;

// Counts a second at full throw. A count is not a degree - the game's own sensitivity
// decides that - so this is a number for the person holding the controller to turn, and
// mirv_vr_aimspeed is how.
float g_AimSpeed = 1400.0f;

// And what it means once a count is worth a measured number of degrees, which is what
// anybody actually wants to set: the same feel on any player's sensitivity.
float g_AimDegreesPerSecond = 120.0f;

// What is left over from last frame, kept rather than thrown away. Truncating a per-frame
// amount loses up to a count every frame on every axis, which at this frame rate is a
// steady drift; and anything under a count produces nothing at all, which puts a dead band
// exactly where fine aiming happens.
float g_AimCarryX = 0.0f;
float g_AimCarryY = 0.0f;

bool g_FireHeld = false;
bool g_AltFireHeld = false;
bool g_PrevWeaponNext = false;
bool g_PrevWeaponPrev = false;
bool g_PrevReload = false;
bool g_PrevWalkToggle = false;
ULONGLONG g_HandAimRetryAt = 0;
bool g_PrevAimCentre = false;
bool g_PrevTeamT = false;
bool g_PrevTeamCt = false;

// Free look is forced on while playing; this puts back whatever it was before.
bool g_FreeLookWasOn = false;
bool g_FreeLookBeforePlay = false;

void ReleaseGameButtons() {
    if (g_FireHeld)    { g_FireHeld = false;    QueueMouseButton(false, false); }
    if (g_AltFireHeld) { g_AltFireHeld = false; QueueMouseButton(true, false); }
    // The pointer's own button is a separate latch, and loading a map with the trigger
    // held would otherwise leave the real mouse button down with nothing left to release
    // it - the pointer path returns early once there is a map.
    if (g_CursorPressed) { g_CursorPressed = false; QueueMouseButton(false, false); }
    g_AimCarryX = g_AimCarryY = 0.0f;

    // A servo left running across a mode change would keep sending counts at a target
    // that no longer means anything.
    g_ServoYaw.Cancel();
    g_ServoPitch.Cancel();
}

// The whole playing layout, kept in one function so it can be read as a layout rather
// than found scattered through the spectating one.
//
//   left stick    walk               right stick   aim
//   left grip     crouch             right grip    jump
//   left trigger  secondary fire     right trigger fire
//   B             previous weapon    A             next weapon
//   menu short    the window         menu long     escape
//
// Held things are held: crouch and jump are keys down, fire is a button down, and every
// one of them is released by ReleaseHeldKeys and ReleaseGameButtons on any way out of
// this mode. The weapons are edges, one notch of wheel each - which is what CS2 binds to
// invnext and invprev by default, so nothing has to be rebound inside the game.
void PlayingInput(float dt) {
    float x = 0.0f, y = 0.0f;

    if (GetVec2(g_MoveAction, x, y)) WalkFromStick(x, y);
    else ReleaseHeldKeys();

    // The aim.
    //
    // The angles are read BEFORE anything is sent this frame: that is what the gain
    // estimator needs, and reading them afterwards would credit this frame's counts to a
    // turn that has not happened yet.
    float base[3];
    AfxVr_GetBaseAngles(base);
    if (!g_GainInitialised) ResetAimLearning();

    int sentX = 0, sentY = 0;

    if (kAimHand == g_AimMethod) {
        float handYaw = 0.0f, handPitch = 0.0f;
        if (!HandAimWorld(handYaw, handPitch)) {
            StopHandAim();
        } else if (!g_HandAimReady) {
            // Bring the aim onto the hand once, with the servo, because it measures the
            // delay while it does it - and the tracker needs that number and does not
            // measure it for itself.
            if (!g_ServoYaw.Running() && !g_ServoPitch.Running()) {
                if (AfxVrMath::AimServo::kReached == g_ServoYaw.outcome
                    && AfxVrMath::AimServo::kReached == g_ServoPitch.outcome) {
                    g_TrackYaw.Reset();
                    g_TrackPitch.Reset();
                    g_TrackPitch.wraps = false;
                    g_TrackYaw.lagFrames = g_ServoYaw.lagFrames > 0 ? g_ServoYaw.lagFrames : 2;
                    g_TrackPitch.lagFrames = g_TrackYaw.lagFrames;
                    g_HandAimReady = true;
                    advancedfx::Message("AFXVR: aiming with the hand; the game answers in %i frame(s).\n",
                        g_TrackYaw.lagFrames);
                } else if (AfxVrMath::AimServo::kGaveUp == g_ServoYaw.outcome
                           || AfxVrMath::AimServo::kRefused == g_ServoYaw.outcome) {
                    // Try again, on a slower beat.
                    //
                    // Cancel alone was a deadlock, and a review named it before a headset
                    // did: Cancel does not clear the outcome, so the next frame reads the
                    // same refusal, cancels again, and never starts anything. Worn, that is
                    // "I can walk and shoot but the crosshair will not move", for ever,
                    // with the log showing ready 0 and nothing ever sent.
                    //
                    // Putting it back to idle is what lets the next frame start a run. Half
                    // a second between attempts, so a genuinely deaf game is not hammered
                    // sixty times a second while the player is in a menu or dead.
                    g_ServoYaw.Cancel();
                    g_ServoPitch.Cancel();
                    g_ServoYaw.outcome = AfxVrMath::AimServo::kIdle;
                    g_ServoPitch.outcome = AfxVrMath::AimServo::kIdle;
                    g_HandAimRetryAt = GetTickCount64() + 500;
                } else if (GetTickCount64() >= g_HandAimRetryAt) {

                    g_GazeTargetYaw = handYaw;
                    g_GazeTargetPitch = handPitch;
                    g_ServoYaw.Start();
                    g_ServoPitch.Start();
                }
            }
            if (g_ServoYaw.Running()) {
                sentX = g_ServoYaw.Step(AfxVrMath::NormalizeDegrees(g_GazeTargetYaw - base[1]),
                                        EffectiveGain(g_GainYaw, kSeedGainYaw));
            }
            if (g_ServoPitch.Running()) {
                sentY = g_ServoPitch.Step(g_GazeTargetPitch - base[0],
                                          EffectiveGain(g_GainPitch, kSeedGainPitch));
            }
        } else {
            // The hand aims inside the area you are looking at; the head decides where
            // that area is.
            //
            // Asked for from inside the headset, after the first attempt: "the turning is
            // horrible - past a point it starts turning my head, in jerks. Either turn only
            // when the head turns, and with the stick; and let the hand only aim within the
            // region we are looking at."
            //
            // So the target is the GAZE, with the hand allowed to pull it a limited
            // distance off centre. Turning the head moves the target and the body follows
            // it smoothly, through the same tracker - no threshold, no snap, nothing to
            // cross. The stick turns the room, which turns the gaze, which the body
            // follows. And the crosshair stays in front of you where it can be seen
            // instead of wandering to the edge of the picture.
            //
            // The cone this replaces was not wrong in principle - it is what a stick-aimed
            // headset needs - but with the hand aiming it put a lurch in the middle of
            // every wide movement, which is exactly the thing VR must not do.
            float headYaw = AfxVr_ViewYawDegrees();
            float headPitch = AfxVr_ViewPitchDegrees();

            float offYaw = AfxVrMath::NormalizeDegrees(handYaw - headYaw);
            float offPitch = handPitch - headPitch;
            if (offYaw >  g_AimWindowDegrees) offYaw =  g_AimWindowDegrees;
            if (offYaw < -g_AimWindowDegrees) offYaw = -g_AimWindowDegrees;
            if (offPitch >  g_AimWindowDegrees) offPitch =  g_AimWindowDegrees;
            if (offPitch < -g_AimWindowDegrees) offPitch = -g_AimWindowDegrees;

            float targetYaw = AfxVrMath::NormalizeDegrees(headYaw + offYaw);
            float targetPitch = headPitch + offPitch;
            if (targetPitch >  89.0f) targetPitch =  89.0f;
            if (targetPitch < -89.0f) targetPitch = -89.0f;

            sentX = g_TrackYaw.Step(targetYaw, base[1], EffectiveGain(g_GainYaw, kSeedGainYaw));
            sentY = g_TrackPitch.Step(targetPitch, base[0], EffectiveGain(g_GainPitch, kSeedGainPitch));
        }

        // What it decided FROM, not what it decided - the rule this project keeps learning.
        // Once a second, so it can be left on while playing.
        {
            static ULONGLONG lastLogged = 0;
            ULONGLONG now = GetTickCount64();
            if (g_HandAimLog && now - lastLogged >= 1000) {
                lastLogged = now;
                advancedfx::Message(
                    "AFXVR: hand %.1f/%.1f  game %.1f/%.1f  sent %i/%i  gain %.5f%s  lag %i  ready %i\n",
                    handYaw, handPitch, base[1], base[0], sentX, sentY,
                    EffectiveGain(g_GainYaw, kSeedGainYaw), g_GainYaw.Converged() ? " measured" : " seeded",
                    g_TrackYaw.lagFrames, g_HandAimReady ? 1 : 0);
            }
        }


    } else if (kAimStick == g_AimMethod) {
        // Look at something and click the right stick to bring the aim to it. Refuses
        // itself until a count is worth a known number of degrees.
        bool gaze = GetPressed(g_RecenterAction);
        if (gaze && !g_PrevAimCentre && g_AimGaze) {
            g_GazeTargetYaw = AfxVr_ViewYawDegrees();
            g_GazeTargetPitch = AfxVr_ViewPitchDegrees();
            if (g_GazeTargetPitch >  89.0f) g_GazeTargetPitch =  89.0f;
            if (g_GazeTargetPitch < -89.0f) g_GazeTargetPitch = -89.0f;
            g_ServoYaw.Start();
            g_ServoPitch.Start();
        }
        g_PrevAimCentre = gaze;

        if (g_ServoYaw.Running()) {
            sentX = g_ServoYaw.Step(AfxVrMath::NormalizeDegrees(g_GazeTargetYaw - base[1]),
                                    EffectiveGain(g_GainYaw, kSeedGainYaw));
        }
        if (g_ServoPitch.Running()) {
            sentY = g_ServoPitch.Step(g_GazeTargetPitch - base[0], EffectiveGain(g_GainPitch, kSeedGainPitch));
        }

        if (GetVec2(g_TurnAction, x, y)) {
            float turn = ShapeStick(x);
            float pitch = ShapeStick(y);
            float gainYaw = EffectiveGain(g_GainYaw, kSeedGainYaw);
            float gainPitch = EffectiveGain(g_GainPitch, kSeedGainPitch);

            if (!g_ServoYaw.Running()) {
                float wanted = (0.0f != gainYaw && g_GainYaw.Converged())
                    ? (turn * g_AimDegreesPerSecond * dt / gainYaw) * -1.0f
                    : turn * g_AimSpeed * dt;
                sentX = AfxVrMath::TakeWholeUnits(wanted, g_AimCarryX);
            }
            if (!g_ServoPitch.Running()) {
                float wanted = (0.0f != gainPitch && g_GainPitch.Converged())
                    ? (-pitch * g_AimDegreesPerSecond * dt / gainPitch)
                    : -pitch * g_AimSpeed * dt;
                sentY = AfxVrMath::TakeWholeUnits(wanted, g_AimCarryY);
            }
        }

        // And the body follows only when the aim reaches the edge of the cone.
        float relative = AfxVrMath::NormalizeDegrees(base[1] - BodyForwardWorldDegrees());
        float step = AfxVrMath::BodyTurnStep(relative, g_AimDeadzoneDegrees, g_AimSnapDegrees);
        if (0.0f != step) AfxVr_AddYaw(step);
    }

    // Turning, whatever is driving the aim - including nothing.
    //
    // This lived inside the hand and stick branches, so with aiming switched off, which is
    // the default, the right stick did nothing at all in a game. Reported from the headset
    // as "the right stick will not let me turn round": it was not that a hundred and eighty
    // degrees was too far, it was that no angle worked.
    //
    // In stick-aim the stick is the aim and the body follows through the cone, so turning
    // is handled there instead.
    if (kAimStick != g_AimMethod && GetVec2(g_TurnAction, x, y)) {
        if (0.0f < g_SnapTurnDegrees) {
            float step = AfxVrMath::SnapTurn(g_SnapTurn, x, g_SnapTurnDegrees);
            if (step) AfxVr_AddYaw(step);
        } else {
            float t = ShapeStick(x);
            if (t) AfxVr_AddYaw(-t * g_TurnSpeed * dt);
        }
    }

    if (sentX || sentY) QueueMouseMove(sentX, sentY);


    // Everything sent this frame, servo, tracker and stick together: those bursts are the
    // best training data there is.
    //
    // Except at a stop. Source clamps pitch at plus or minus 89 degrees, so once the view
    // is looking straight up or straight down, further counts change nothing - and an
    // estimator told "we sent a hundred counts and the angle did not move" learns that a
    // count is worth zero degrees. After that the tracker refuses a zero gain and the
    // vertical is dead for the rest of the session while the horizontal carries on working.
    //
    // That is exactly what was reported from inside the headset: "at first it moved in all
    // directions, then only horizontally". A clamp is not a measurement; feed nothing there.
    //
    // The same reasoning covers a view that is not ours to move at all - dead, spectating a
    // fixed camera, or frozen at a round start. A whole second of counts against an angle
    // that cannot move would teach the same lie, so a view that has not moved AT ALL while
    // counts were being sent is treated as not listening rather than as infinitely heavy.
    {
        bool pitchAtStop = (base[0] >= 88.5f) || (base[0] <= -88.5f);

        static float lastYaw = 0.0f, lastPitch = 0.0f;
        static int frozenFrames = 0;
        bool moved = (fabsf(AfxVrMath::NormalizeDegrees(base[1] - lastYaw)) > 0.001f)
                  || (fabsf(base[0] - lastPitch) > 0.001f);
        if (moved || (0 == sentX && 0 == sentY)) frozenFrames = 0;
        else frozenFrames++;
        lastYaw = base[1];
        lastPitch = base[0];
        bool deaf = frozenFrames > 15;

        if (!deaf) {
            g_GainYaw.Feed(sentX, base[1]);
            if (!pitchAtStop) g_GainPitch.Feed(sentY, base[0]);
        }
    }


    // The buttons.
    //
    //   left grip      crouch            right grip        jump
    //   left trigger   use / defuse      right trigger     fire
    //   left stick     walk              right stick       turn
    //   left click     slow walk         right click       reload
    //   X / Y          team, while a picker is up          A  next weapon
    //                                                      B  alternative fire
    //
    // "Use" earns a trigger because it is the most needed contextual action in the game:
    // defuse, plant, open, and pick a weapon off the ground - which CS2 does automatically
    // only into an empty slot, so without this key a gun you are standing on stays there.
    // Reported that way from the headset: "I step on a weapon and it does not pick up."
    SetHeldKey(kHeldCrouch, GetPressed(g_FreeLookAction));   // left grip
    SetHeldKey(kHeldJump,   GetPressed(g_ModeAction));       // right grip
    SetHeldKey(kHeldUse,    GetPressed(g_SeekBackAction));   // left trigger

    bool fire = GetPressed(g_SeekForwardAction);             // right trigger
    if (fire != g_FireHeld) { g_FireHeld = fire; QueueMouseButton(false, fire); }

    bool altFire = GetPressed(g_SlowMoAction);               // B
    if (altFire != g_AltFireHeld) { g_AltFireHeld = altFire; QueueMouseButton(true, altFire); }

    bool next = GetPressed(g_PauseAction);                   // A
    if (next && !g_PrevWeaponNext) QueueMouseWheel(1);
    g_PrevWeaponNext = next;

    // Reload on the right stick's click, asked for by name. A tap, not a hold.
    bool reload = GetPressed(g_RecenterAction);
    if (reload && !g_PrevReload) QueueKeyTap('R');
    g_PrevReload = reload;

    // Slow walk toggles rather than being held: Shift in this game is held for whole
    // crossings, and a thumb resting on a stick click for that long is not a thing anyone
    // should have to do.
    bool walkPress = GetPressed(g_ResetAction);              // left stick click
    if (walkPress && !g_PrevWalkToggle) {
        SetHeldKey(kHeldWalk, !g_HeldKey[kHeldWalk]);
        advancedfx::Message("AFXVR: %s.\n", g_HeldKey[kHeldWalk] ? "walking" : "running");
    }
    g_PrevWalkToggle = walkPress;
}

void ProcessInput(const AfxVrMath::ModeResult & mode) {

    // Not focused any more - the runtime dashboard is up, or the headset came off - so
    // the controllers are not ours. Everything being held has to come up FIRST: the mode
    // has not changed, so the transition cleanup will not run, and a player left walking
    // into a wall with the dashboard open cannot reach a keyboard to stop it.
    if (!g_ActionsAttached || XR_SESSION_STATE_FOCUSED != g_State) {
        ReleaseHeldKeys();
        ReleaseGameButtons();
        StopHandAim();
        return;
    }

    XrActiveActionSet active = { g_ActionSet, XR_NULL_PATH };
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions_(g_Session, &sync))) return;

    // The performance counter, not GetTickCount64.
    //
    // GetTickCount64 advances in steps of about 15.6 ms. At roughly 28 ms a frame it
    // therefore reads 16, 31, 31, 16, 47 - the total is right, every individual frame is
    // not. Anything scaled by dt then moves unevenly at a constant stick deflection: the
    // aim, and the free camera in a demo, which has been doing this all along.
    LARGE_INTEGER nowTicks = StageStart();
    float dt = 0.0f;
    if (g_HaveLastInputTick) dt = (float)SecondsSince(g_LastInputTicks);
    g_LastInputTicks = nowTicks;
    g_HaveLastInputTick = true;

    if (dt > 0.1f) dt = 0.1f; // a hitch must not teleport the viewer

    // The menu button, in every mode: a short press puts the game's own window on a screen
    // over whatever is loaded, a long one sends Escape.
    //
    // They were one press to begin with, doing both, and that is wrong in the case it
    // exists for: with a team picker or a buy menu ALREADY up, Escape dismisses the thing
    // the viewer wanted to point at.
    {
        bool b = GetPressed(g_MenuButtonAction);
        if (b && !g_PrevMenuButton) {
            g_MenuButtonPressedAt = GetTickCount64();
        } else if (!b && g_PrevMenuButton) {
            if (GetTickCount64() - g_MenuButtonPressedAt >= 500) {
                QueueKeyTap(VK_ESCAPE);
                advancedfx::Message("AFXVR: escape.\n");
            } else {
                g_MenuOverride = !g_MenuOverride;
                advancedfx::Message("AFXVR: the window %s.\n",
                    g_MenuOverride ? "is on a screen in front of you" : "is off the screen");
            }
        }
        g_PrevMenuButton = b;
    }

    // Picking a team, without needing the pointer to work.
    //
    // Reported from inside the headset: "the map loads, there is the T or CT choice, and
    // there is nothing to choose with". The sheet and the ray are the real answer, but
    // they are several things that all have to work at once, and a player staring at a
    // team picker they cannot answer is stuck completely. jointeam is a plain console
    // command, so X and Y answer it directly - and those two buttons have no other job
    // while a modal is up, so this overloads nothing.
    if (AfxVrMath::kVrModePlay == mode.mode && mode.pointer) {
        bool t  = GetPressed(g_PrevAction);   // X
        bool ct = GetPressed(g_NextAction);   // Y
        if (t && !g_PrevTeamT) {
            QueueCommand("jointeam 2");
            advancedfx::Message("AFXVR: joining T.\n");
        }
        if (ct && !g_PrevTeamCt) {
            QueueCommand("jointeam 3");
            advancedfx::Message("AFXVR: joining CT.\n");
        }
        g_PrevTeamT = t;
        g_PrevTeamCt = ct;
    } else {
        g_PrevTeamT = g_PrevTeamCt = false;
    }

    // Playing a map is a different instrument from watching a recording, so it is a


    // different layout rather than the same one with exceptions in it. While a sheet is up
    // neither applies: the pointer owns the mouse, and an absolute pointer fighting a
    // relative aim on one device is a fight the aim wins.
    if (AfxVrMath::ModeTakesGameInput(mode)) {
        PlayingInput(dt);
        return;
    }

    ReleaseHeldKeys();
    ReleaseGameButtons();

    if (mode.pointer || AfxVrMath::kVrModePlay == mode.mode) return;   // a sheet owns the mouse

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

    // The menu button used to be handled again here, the old way: one press toggling the
    // screen AND sending Escape. It had been dead for a while and nobody had noticed.
    //
    // Both copies read the same action and both wrote g_PrevMenuButton, and the one above -
    // which runs in every mode, before this branch - got there first each frame. So by the
    // time this one tested `b && !g_PrevMenuButton`, the flag already equalled b and the
    // condition could never be true. Dead code that still described itself confidently in
    // a comment, which is how it survived: it was read as the explanation of a behaviour
    // that was actually coming from somewhere else entirely, and the printed control table
    // was written from it and was wrong.
    //
    // Found by checking a diagram against the source rather than against the table.

    // Right stick click: a short press hides and shows the HUD, a long one recentres.
    //
    // The frequent action goes on the short press and the rare one on the long, which is
    // the only way round that does not surprise anybody. Hiding does not re-place: a
    // hide and a show must bring the panels back exactly where they were, or it is not a
    // hide, it is a move. Re-placing is the Menu button and F11.
    b = GetPressed(g_RecenterAction);

    if (b && !g_PrevRecenter) {
        g_RecenterPressedAt = GetTickCount64();
    } else if (!b && g_PrevRecenter) {
        ULONGLONG held = GetTickCount64() - g_RecenterPressedAt;
        if (held >= 500) {
            AfxVr_Recenter();
            g_RoomRefValid = false;   // and this is where you are standing now
            advancedfx::Message("AFXVR: recentred.\n");
        } else {
            g_PanelShown = !g_PanelShown;
            advancedfx::Message("AFXVR: HUD %s.\n", g_PanelShown ? "shown" : "hidden");
        }
    }
    g_PrevRecenter = b;

    // The demo's own camera cycle - first person, chase, free - on the left stick click.
    // It was on the right grip, where nobody found it: the operator's words were "I do not
    // understand how to switch the camera type", and while the base camera was frozen it
    // also did nothing visible, so it never got learned.
    //
    // Same two companions as switching players: end HLAE's free camera first, or the cycle
    // changes a view that is not being shown; and put the viewer back on the camera,
    // because entering first person from wherever the sticks had wandered does not look
    // like first person.
    b = GetPressed(g_ResetAction);
    if (b && !g_PrevReset) { QueueCommand("mirv_input end", 0); QueueKeyTap(VK_UP); AfxVr_ResetMove(); }
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

    // X and Y move through the demo. They swapped jobs with the triggers: seeking is rare
    // and a face button is fine for it.
    b = GetPressed(g_NextAction);       // Y
    if (b && !g_PrevNext) QueueSeek(+g_SeekSeconds);
    g_PrevNext = b;

    b = GetPressed(g_PrevAction);       // X
    if (b && !g_PrevPrev) QueueSeek(-g_SeekSeconds);
    g_PrevPrev = b;

    // Back onto whoever the demo is following, from wherever the sticks have taken you.
    b = GetPressed(g_ModeAction);       // right grip
    if (b && !g_PrevMode) AfxVr_ResetMove();
    g_PrevMode = b;

    bool forward = GetPressed(g_SeekForwardAction);
    bool back    = GetPressed(g_SeekBackAction);

    if (kTriggersPlayers == g_TriggerMode) {
        // Switching players puts the viewer back on that player rather than wherever the
        // sticks had wandered to - otherwise you follow someone from across the map.
        //
        // And it ends HLAE's free camera first. With that camera active it owns the view
        // entirely, so "next player" changes who the demo is following and the picture
        // does not move at all - which, with a headset on and no console in sight, is
        // indistinguishable from the button being broken. Somebody pressing "next player"
        // is asking to look at a player; the free camera is one press away again.
        if (forward && !g_PrevSeekForward) { QueueCommand("mirv_input end", 0); QueueKeyTap(VK_RIGHT); AfxVr_ResetMove(); }
        if (back    && !g_PrevSeekBack)    { QueueCommand("mirv_input end", 0); QueueKeyTap(VK_LEFT);  AfxVr_ResetMove(); }
    } else if (kTriggersFov == g_TriggerMode) {
        // Right widens, left narrows: the same hands as "later" and "earlier", which is
        // the only mapping anyone guesses right with a headset on.
        int direction = 0;
        if (forward && !back) direction = +1;
        else if (back && !forward) direction = -1;

        if (0 == direction) {
            g_TriggerRepeat = 0;
        } else {
            bool fresh = (forward && !g_PrevSeekForward) || (back && !g_PrevSeekBack);
            if (fresh) g_TriggerRepeat = 0;
            if (0 == g_TriggerRepeat) {
                g_TriggerRepeat = kTriggerRepeatFrames;
                g_AskScale *= (direction > 0) ? kAskScaleStep : (1.0f / kAskScaleStep);
                if (g_AskScale < 0.5f) g_AskScale = 0.5f;
                if (g_AskScale > 1.2f) g_AskScale = 1.2f;

                // The person turning this dial cannot see the console, but the log is what
                // the number gets read out of afterwards, so print it every time.
                float claimed = 0.0f, asked = 0.0f;
                {
                    std::lock_guard<std::mutex> lock(g_ViewMutex);
                    if (g_ViewsValid) {
                        claimed = WantedFovDegrees(g_Views[0].fov);
                        asked = EffectiveFovDegrees(g_Views[0].fov);
                    }
                }
                advancedfx::Message(
                    "AFXVR: ask scale %.3f - engine asked %.1f deg, runtime still told %.1f\n",
                    g_AskScale, asked, claimed);
            } else {
                g_TriggerRepeat--;
            }
        }
    } else {
        if (forward && !g_PrevSeekForward) QueueSeek(+g_SeekSeconds);
        if (back && !g_PrevSeekBack) QueueSeek(-g_SeekSeconds);
    }
    if (kTriggersFov != g_TriggerMode) g_TriggerRepeat = 0;

    g_PrevSeekForward = forward;
    g_PrevSeekBack = back;

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

// Where one piece of the sheet hangs, relative to the anchor the whole panel was placed
// at. Azimuth turns it around the viewer, elevation lifts it, and it is tilted to face
// the anchor - a quad three feet below eye level that is still vertical is read edge-on.
//
// OpenXR's quad shows the face whose normal is its local +Z, which is why the existing
// anchor pose is the head's yaw and not the head's yaw turned around. The pitch is a
// rotation about the quad's own X after that yaw, so the normal comes out at
// (sin(yaw)cos(el), -sin(el), cos(yaw)cos(el)) - exactly back along the direction the
// piece was placed in.
XrPosef PlaceRegion(const PanelRegion & region) {
    const double d2r = M_PI / 180.0;

    double anchorYaw = 2.0 * atan2((double)g_PanelPose.orientation.y,
                                   (double)g_PanelPose.orientation.w);
    double yaw = anchorYaw + region.azimuthDegrees * g_PanelSpread * d2r;
    double el  = region.elevationDegrees * g_PanelSpread * d2r;

    double cosEl = cos(el), sinEl = sin(el);

    // Where the viewer is now, not where they were when the anchor was placed. Only the
    // yaw comes from the anchor.
    const XrVector3f & from = (g_PanelFollow && g_PanelFollowValid)
        ? g_PanelFollowPos : g_PanelPose.position;

    XrPosef pose = {};
    pose.position.x = from.x + (float)(-sin(yaw) * cosEl * region.distanceMetres);
    pose.position.y = from.y + (float)( sinEl            * region.distanceMetres);
    pose.position.z = from.z + (float)(-cos(yaw) * cosEl * region.distanceMetres);

    // qYaw about Y, then qPitch about the quad's OWN X - composed with the same helper the
    // eye code uses, rather than expanded by hand.
    //
    // It was expanded by hand, and the z term came out +sin(yaw/2)sin(el/2) where the
    // product gives minus that. The difference is pitching about the WORLD's X instead of
    // the quad's, which rolls every quad by about sin(yaw)*elevation: zero at anchor yaw 0,
    // which is where it was checked, and gross at whatever yaw the viewer happened to be
    // facing. In the headset the score strip leaned ten degrees one way and the timeline
    // thirty-five the other.
    AfxVrMath::YawThenPitchQuat((float)yaw, (float)el,
                                pose.orientation.x, pose.orientation.y,
                                pose.orientation.z, pose.orientation.w);

    return pose;
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
    ProcessInput(g_Mode);

    LocateViews(g_FrameState.predictedDisplayTime);

    // The hands, at the same instant as the eyes. The caller places the sheet and then
    // tests these rays on this thread, so the mouse and the ticket's dot share one hit.
    LocateAim(g_FrameState.predictedDisplayTime);
}

void PublishFrameRecord(const XrView * rendered) {
    std::lock_guard<std::mutex> lock(g_ViewMutex);

    // One entry per frame, complete, written under the lock in one go. The previous
    // arrangement published the positions in one lock scope and the orientations in
    // another, so a reader between the two got half of one frame and half of another.
    // Publish even before the first locate succeeds: a menu pass still needs its own
    // mode and timing, and returning the previous ticket would silently reuse its mode.
    g_FrameSerial++;
    FrameRecord & record = g_FrameRing[g_FrameSerial % kFrameRing];
    record = FrameRecord();
    record.serial = g_FrameSerial;
    record.mode = g_Mode;
    record.sheetPlaced = g_Mode.sheet && g_MenuPlaced;
    record.sheetPose = g_MenuPose;
    record.sheetSize = g_MenuSize;
    record.cursor = g_Cursor;
    record.displayTime = g_FrameState.predictedDisplayTime;
    record.shouldRender = g_FrameState.shouldRender;
    // Only meaningful in low-latency mode: there the engine thread did the xrWaitFrame
    // this frame will be begun and ended against. In safe mode the render thread waits
    // for itself and g_FrameState here is a frame old.
    record.timingValid = g_LowLatency;

    if (rendered) {
        // Remember exactly what this frame is being rendered with, for the layer.
        g_RenderedViews[0] = record.rendered[0] = rendered[0];
        g_RenderedViews[1] = record.rendered[1] = rendered[1];
        g_RenderedViewsValid = record.renderedValid = true;

        float base[3];
        AfxVr_GetBaseAngles(base);
        record.aimYaw = base[1];
        record.aimPitch = base[0];
        record.bodyForwardWorld = BodyForwardWorldDegrees();
        record.roomForwardRadians = RoomForwardYawRadians();
    }
    record.valid = true;
}

// "session state 1" is not a diagnosis. IDLE in particular means the runtime has taken
// the session and is declining to run it - usually because the headset is not being worn,
// or because something else owns the compositor - and reading that as a bug in the hook
// wastes an evening.
// Is SteamVR running?
//
// On a Quest over Link, SteamVR is not an alternative to the Oculus runtime - it is a
// client of it, and while it is up it is the application the Oculus compositor is
// scheduling. A second, native session opened underneath it gets a session that says it is
// running and an xrWaitFrame that takes hundreds of milliseconds, until CS2's render thread
// is parked inside the wait and the game stops responding. From the outside that looks
// exactly like whatever shipped last being broken, which is an expensive thing for it to
// look like.
//
// One snapshot at session start; nothing here runs per frame.
bool SteamVrIsRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (INVALID_HANDLE_VALUE == snapshot) return false;

    bool found = false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (0 == _wcsicmp(entry.szExeFile, L"vrserver.exe")
             || 0 == _wcsicmp(entry.szExeFile, L"vrcompositor.exe")) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

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
            } else if (XR_SESSION_STATE_FOCUSED == g_State) {
                // On a face. Put the panel where the viewer is actually looking.
                g_PanelPlaceCountdown = kPanelPlaceDelayFrames;
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


// Starting the session with nobody at the keyboard.
//
// AFXVR_AUTOSTART=1 in the environment arms it; mirv_vr_autostart 0|1 overrides that from a
// config. It replaces the last third of start-vr.ps1: watch console.log for a line, sleep
// twenty seconds, synthesise F9. The sleep is a guess about how long a demo takes to load
// and the key needs the game window to have focus, which it does not while a headset is
// being put on - both of those have cost a session already.
//
// The preferred condition is not "the game is up", it is "a demo is playing": the view
// struct reads like a camera, a demo tick is available, and that tick has MOVED since the
// last frame. Started against a demo that is loaded but not yet running, the session begins
// before there is a camera worth wearing.
//
// But that condition alone leaves the menu unreachable, and the menu is now a place we can
// actually go: CS2's own interface on a quad, with a controller ray. Somebody who launched
// without a demo - to pick a map, or to start a game against bots - has a headset on, no
// demo tick to wait for, and NO CONSOLE to type mirv_vr_xr start into: the window is taller
// than the display, Windows clamps it, and the input line is off-screen. So a second signal
// is needed, and the only one available before a world exists is that the game is rendering
// at all. This function is called once per frame from MirvVrXr_EngineThread_Frame, demo or
// not, so counting its own calls is that signal.
//
// The two are not interchangeable and the log says which one fired. The frame count is the
// slower of the two on purpose: with a demo loading, the tick starts moving long before
// kAutoStartMenuFrames is reached, so the demo path still wins the race and the session
// still begins on a real camera rather than on the loading screen behind it.
int g_AutoStart = -1;          // -1 not read yet, 0 off, 1 armed
bool g_AutoStartDone = false;
int g_AutoStartLastTick = -1;
int g_AutoStartFrames = 0;

// It used to fire exactly once and then give up for good, on the reasoning that a session
// which refuses to start will refuse again sixty times a second and the person it is
// refusing for is wearing a headset. The reasoning was right and the conclusion was wrong,
// as the first launch through cs2vr.exe showed: the game reached its menu two seconds in,
// autostart fired, the runtime answered XR_ERROR_FORM_FACTOR_UNAVAILABLE because the
// headset was not awake yet, and that was that. Nobody puts a headset on in two seconds,
// and the advertised recovery - press F9 - is exactly what a worn launch cannot do.
//
// So: keep trying, slowly and for a bounded time. Half a minute is longer than it takes to
// pick a headset up, and the log says one line per attempt rather than sixty a second.
int g_AutoStartAttempts = 0;
ULONGLONG g_AutoStartLastAttempt = 0;
const int kAutoStartMaxAttempts = 15;
const ULONGLONG kAutoStartRetryMs = 2000;

// About four seconds at a menu's frame rate. Long enough for the D3D11 device and the swap
// chain to exist - SessionStart refuses without a device and would waste its one attempt -
// and long enough for a demo that is going to load to have started ticking. Short enough
// that somebody already wearing the headset does not conclude it is broken.
const int kAutoStartMenuFrames = 240;

void EngineThread_AutoStart() {
    if (g_AutoStartDone) return;

    if (-1 == g_AutoStart) {
        char value[16] = "";
        g_AutoStart = (0 < GetEnvironmentVariableA("AFXVR_AUTOSTART", value, sizeof(value))
            && 0 != atoi(value)) ? 1 : 0;
        if (1 == g_AutoStart) {
            advancedfx::Message(
                "AFXVR: AFXVR_AUTOSTART is set. The headset session will start by itself: as soon\n"
                "AFXVR: as a demo is running, or a few seconds in if there is no demo, so the menu\n"
                "AFXVR: can be used from inside the headset. mirv_vr_autostart 0 stops that.\n");
        }
    }
    if (1 != g_AutoStart) return;

    // Somebody got there first, by hand or by config. Nothing left to do.
    if (XR_NULL_HANDLE != g_Session) { g_AutoStartDone = true; return; }

    g_AutoStartFrames++;

    int tick = 0;
    bool haveDemo = g_MirvTime.GetCurrentDemoTick(tick);

    if (haveDemo) {
        // A demo: wait for the camera as well as for the clock.
        if (0 == AfxVr_PlausibleViewCount()) return;
        if (-1 == g_AutoStartLastTick || tick == g_AutoStartLastTick) {
            g_AutoStartLastTick = tick;
            return;
        }
    } else {
        // No demo. Nothing here can be waited for except the game drawing frames, and the
        // view struct is deliberately NOT required: at the menu there may be no 3D view
        // being set up at all, and menu mode does not write one. Anything that does write
        // it is still gated on the plausibility check, which has not been relaxed.
        g_AutoStartLastTick = -1;
        if (g_AutoStartFrames < kAutoStartMenuFrames) return;
    }

    // Slowly, so a failure reports once every two seconds rather than sixty times a second.
    ULONGLONG now = GetTickCount64();
    if (0 != g_AutoStartLastAttempt && now - g_AutoStartLastAttempt < kAutoStartRetryMs) return;
    g_AutoStartLastAttempt = now;
    g_AutoStartAttempts++;

    if (1 == g_AutoStartAttempts) {
        // The clock is what moved, and at CS2's own menu it moves without a demo being
        // loaded - so this does not claim one is.
        if (haveDemo) {
            advancedfx::Message("AFXVR: autostart: the game is running (tick %i); starting the session.\n", tick);
        } else {
            advancedfx::Message(
                "AFXVR: autostart: no demo after %i frames; starting the session so the menu can be\n"
                "AFXVR: used from inside the headset.\n", g_AutoStartFrames);
        }
    }

    if (MirvVrXr_SessionStart()) {
        g_AutoStartDone = true;
        return;
    }

    if (g_AutoStartAttempts < kAutoStartMaxAttempts) {
        advancedfx::Warning(
            "AFXVR: autostart: not yet (attempt %i of %i). If the headset is not awake, put it\n"
            "AFXVR: on now - this keeps trying for about %i more seconds.\n",
            g_AutoStartAttempts, kAutoStartMaxAttempts,
            (int)((kAutoStartMaxAttempts - g_AutoStartAttempts) * kAutoStartRetryMs / 1000));
        return;
    }

    g_AutoStartDone = true;
    advancedfx::Warning(
        "AFXVR: autostart: gave up after %i attempts. The headset was never available.\n"
        "AFXVR: F9 tries again, and so does mirv_vr_xr start - which a launcher can send down\n"
        "AFXVR: the pipe without anybody reaching the keyboard.\n", g_AutoStartAttempts);
}
} // namespace

bool MirvVrXr_IsRunning() {
    return XR_NULL_HANDLE != g_Instance;
}

bool MirvVrXr_CaptureBeforeUi() {
    return g_CaptureBeforeUi;
}

bool MirvVrXr_WantsPanel() {
    // Deliberately not gated on a session. The callbacks it queues each gate themselves,
    // and the alpha probe - the one thing that answers whether a transparent panel can
    // work at all - has to run at a desk with no headset in the building.
    // A sheet needs the main pass whether or not the HUD panel is on: over a map it
    // carries the team picker just as it carries the whole picture at the main menu.
    return g_PanelEnabled || g_Mode.sheet;
}

bool MirvVrXr_WantsPasses() {

    // As soon as the session is running, not once it is visible. The runtime only
    // advances a session past READY when the application starts its frame loop, so
    // waiting for SYNCHRONIZED before running it means it never starts.
    //
    // Or when forced, with no session and no headset, so the pass machinery can be
    // watched at a desk. Nothing is submitted in that case - see SubmitEye.
    // Not in menu mode: with no map the view struct is not a camera, and there is nothing
    // worth rendering twice. The main pass does the whole frame instead.
    return (g_SessionRunning && !g_MenuMode) || 0 < g_ForcedPasses;
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

    // Does this runtime read the field of view we submit, or composite with its own?
    // Nothing in this project asked until the stereo would not fuse, and the answer turned
    // out to be the whole problem.
    if (xrGetViewConfigurationProperties_) {
        XrViewConfigurationProperties props = { XR_TYPE_VIEW_CONFIGURATION_PROPERTIES };
        if (XR_SUCCEEDED(xrGetViewConfigurationProperties_(g_Instance, g_SystemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &props))) {
            g_FovMutable = (XR_TRUE == props.fovMutable);
            g_FovMutableKnown = true;
            advancedfx::Message(
                "AFXVR: fovMutable %s - a submitted field of view is %s.\n",
                g_FovMutable ? "TRUE" : "FALSE",
                g_FovMutable ? "honoured" : "IGNORED; the runtime uses its own frustum");
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

    // Cheap, once, and it turns an hour of bisecting a hang into one line of log.
    if (SteamVrIsRunning()) {
        advancedfx::Warning(
            "AFXVR: SteamVR is running.\n"
            "AFXVR: On a Quest over Link, SteamVR is a client of the Oculus runtime and holds\n"
            "AFXVR: the headset. A session opened underneath it is not scheduled: xrWaitFrame\n"
            "AFXVR: takes hundreds of milliseconds, the frame rate collapses, and CS2 stops\n"
            "AFXVR: responding with its render thread parked inside the wait.\n"
            "AFXVR: Close SteamVR, then start the game again. Both launchers refuse while it\n"
            "AFXVR: is up: cs2vr.exe in a release, scripts/start-vr.ps1 in the source tree.\n");
    }

    advancedfx::Message("AFXVR: session created; waiting for the runtime to make it ready.\n");

    return true;
}

void MirvVrXr_SessionStop() {
    // Stop wanting passes FIRST, so the pass loop queues no further eye submissions, then
    // give whatever frame is in flight a moment to finish before the swapchains it is
    // copying into are destroyed.
    //
    // Skipping this is how the game died: a key that stopped and restarted the session in
    // one frame tore the swapchains out from under the render thread, and the runtime
    // answered XR_ERROR_GRAPHICS_DEVICE_INVALID followed by DXGI_ERROR_DEVICE_REMOVED.
    // It is also the likeliest explanation for the XR_ERROR_RUNTIME_FAILURE this project
    // has hit before on repeated stop and start.
    bool wasRunning = g_SessionRunning;
    g_SessionRunning = false;

    if (wasRunning) {
        for (int i = 0; i < 200 && g_FrameBegun; i++) Sleep(1);
        if (g_FrameBegun) {
            advancedfx::Warning(
                "AFXVR: a frame was still open after 200 ms; stopping anyway.\n");
        }
    }

    if (wasRunning && xrEndSession_) xrEndSession_(g_Session);
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
    MirvVrXrInternal::EngineThread_Pipe();
    EngineThread_AutoStart();

    // A console command has to be dispatched from the engine thread, so the controller
    // handler only raises a flag.
    {
        std::vector<std::string> due;
        std::vector<PendingKey> dueKeys;
        PendingMouse dueMouse = {};
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

            dueMouse = g_PendingMouse;
            g_PendingMouse = PendingMouse();

            for (size_t i = 0; i < g_PendingKeys.size(); ) {
                if (0 >= g_PendingKeys[i].delay) {
                    dueKeys.push_back(g_PendingKeys[i]);
                    g_PendingKeys.erase(g_PendingKeys.begin() + i);
                } else {
                    g_PendingKeys[i].delay--;
                    i++;
                }
            }
        }

        // Outside the lock: SendInput can block, and the render thread wants this mutex.
        for (size_t i = 0; i < dueKeys.size(); i++) SendGameKey(dueKeys[i].vk, dueKeys[i].down);

        // The mouse, the same way and for the same reason.
        if (dueMouse.dx || dueMouse.dy || dueMouse.wheel
            || dueMouse.leftDown || dueMouse.leftUp || dueMouse.rightDown || dueMouse.rightUp) {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            // Releases go out whatever has focus, for the same reason the keys do: the press
            // has already landed and the latch is already clear, so dropping the release
            // strands a real mouse button down.
            bool foreground = (pid == GetCurrentProcessId());
            if (foreground || dueMouse.leftUp || dueMouse.rightUp) {
                // Five, not four: a move plus a down AND an up for both buttons, which a mode
                // change releasing what the render thread has just pressed can produce in one
                // drain. Four overflowed the array and handed SendInput the larger count.
                INPUT inputs[5] = {};
                int count = 0;
                if (foreground && (dueMouse.dx || dueMouse.dy)) {
                    inputs[count].type = INPUT_MOUSE;
                    inputs[count].mi.dwFlags = MOUSEEVENTF_MOVE;
                    inputs[count].mi.dx = dueMouse.dx;
                    inputs[count].mi.dy = dueMouse.dy;
                    count++;
                }
                if (foreground && dueMouse.leftDown)  { inputs[count].type = INPUT_MOUSE; inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;  count++; }
                if (dueMouse.leftUp)    { inputs[count].type = INPUT_MOUSE; inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTUP;    count++; }
                if (foreground && dueMouse.rightDown) { inputs[count].type = INPUT_MOUSE; inputs[count].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; count++; }
                if (dueMouse.rightUp)   { inputs[count].type = INPUT_MOUSE; inputs[count].mi.dwFlags = MOUSEEVENTF_RIGHTUP;   count++; }
                if (count) SendInput(count, inputs, sizeof(INPUT));

                if (foreground && dueMouse.wheel) {
                    INPUT wheel = {};
                    wheel.type = INPUT_MOUSE;
                    wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    wheel.mi.mouseData = (DWORD)(dueMouse.wheel * WHEEL_DELTA);
                    SendInput(1, &wheel, sizeof(INPUT));
                }
            }
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

    // A camera that has jumped means the viewer is somewhere else entirely: respawned, or
    // switched to watching another player, or teleported.
    //
    // The room offset must not survive that. It is measured from wherever the viewer was
    // standing when it was last zeroed, and while they were dead and being carried around
    // other people's cameras it went on accumulating - so the next spawn puts the eyes that
    // far out of the new body. Reported from the headset: "I spawned not in my body but a
    // metre to the right of it", and then the right guess about why: "maybe after I was
    // killed and watched other players' cameras... maybe we need a reset each round."
    //
    // A jump is a better signal than a round, because it also catches the camera changes
    // while dead, and it needs nothing from the game but the number already being read.
    {
        static float lastOrigin[3] = { 0.0f, 0.0f, 0.0f };
        static bool haveLastOrigin = false;

        float origin[3];
        AfxVr_GetBaseOrigin(origin);

        if (haveLastOrigin) {
            float dx = origin[0] - lastOrigin[0];
            float dy = origin[1] - lastOrigin[1];
            float dz = origin[2] - lastOrigin[2];
            // Two hundred units is about five metres - further than anybody runs in one
            // frame, and far short of the distances a spawn or a camera change moves.
            if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) {
                g_RoomRefValid = false;
                if (g_AfxVrFrameIndex < g_AfxVrLogUntilFrame) {
                    advancedfx::Message("AFXVR: the camera jumped; standing where you stand now.\n");
                }
            }
        }
        lastOrigin[0] = origin[0];
        lastOrigin[1] = origin[1];
        lastOrigin[2] = origin[2];
        haveLastOrigin = true;
    }

    // What the headset is showing, and therefore who the controllers belong to. Decided

    // here, once a frame, before anything depends on it - the pass loop asks
    // MirvVrXr_WantsPasses immediately afterwards.
    {
        AfxVrMath::ModeInputs in;
        in.sessionRunning = g_SessionRunning;
        in.demoPlaying = g_pEngineToClient && g_pEngineToClient->IsPlayingDemo();
        in.manualSheet = g_MenuOverride;

        const char * level = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : "";
        if (!level) level = "";
        // "<empty>" is a literal the engine hands back when nothing is loaded, not an
        // empty string. Testing level[0] alone reads the '<' as a map name, which is how
        // VR mode once believed it had a world with nothing loaded at all.
        in.mapLoaded = level[0] && 0 != strcmp(level, "<empty>");

        // Debounced: the buy menu flickers the cursor, and a sheet that blinks is worse
        // than one that is a sixth of a second late.
        bool cursorNow = SystemCursorShowing();
        ULONGLONG now = GetTickCount64();
        if (cursorNow != g_CursorShowing) {
            if (0 == g_CursorChangedAt) g_CursorChangedAt = now;
            else if (now - g_CursorChangedAt >= kCursorDebounceMs) {
                g_CursorShowing = cursorNow;
                g_CursorChangedAt = 0;
            }
        } else {
            g_CursorChangedAt = 0;
        }
        in.cursorShowing = g_CursorShowing;

        AfxVrMath::ModeResult next = AfxVrMath::DecideMode(in);

        if (next.mode != g_Mode.mode || next.sheet != g_Mode.sheet) {
            // The inputs, not just the verdict. Every mode fault this project has had was
            // a signal that was wrong and said nothing about why.
            advancedfx::Message(
                "AFXVR: mode %s (map \"%s\", demo %i, cursor %i, button %i) - %s%s\n",
                AfxVrMath::kVrModeMenu  == next.mode ? "MENU" :
                AfxVrMath::kVrModeWatch == next.mode ? "WATCH" :
                AfxVrMath::kVrModePlay  == next.mode ? "PLAY" : "idle",
                level, in.demoPlaying ? 1 : 0, in.cursorShowing ? 1 : 0, in.manualSheet ? 1 : 0,
                next.worldInEyes ? "world in both eyes" : "a screen",
                next.sheet ? (next.sheetOpaque ? "" : ", with the window over it") : "");

            // Anything the last mode was holding down. A player walking into a wall for
            // ever because a menu came up is the kind of thing a headset makes genuinely
            // unpleasant, and it must be impossible by construction rather than by care.
            ReleaseHeldKeys();
            ReleaseGameButtons();

            // Playing, the picture is the head alone plus whatever the viewer has turned
            // to - free look - and NOT the game's own yaw.
            //
            // Two reasons, and the second is the one that makes the aiming scheme work at
            // all. First: with the game's yaw in the composition, every movement of the
            // aim stick rotates the world smoothly under a head that is not turning, which
            // is exactly what makes people ill. Second: the deadzone cone measures the aim
            // against where the BODY faces, and with the game's yaw in both terms that
            // subtraction cancels to nothing - the cone could never fire, so the stick
            // could never turn the viewer, which is the thing a seated player cannot do
            // with their neck.
            //
            // Watching a demo it goes back off: there the base camera IS the shot, and
            // ignoring it would throw away the recording's own framing.
            // The hands and the gun are placed from the once-per-frame view, so writing the
            // head into it puts them on the headset. Off while playing gives them back to
            // the body; the eyes are a separate write and keep the head either way.
            AfxVr_SetHeadAnglesOncePerFrame(
                !(AfxVrMath::kVrModePlay == next.mode && g_PlayFrameBody));

            // Face the way the player is facing.
            //
            // Playing, the picture is the head plus the accumulated turn and does NOT
            // include the game's own yaw - that is what frees the hands from the head. The
            // cost is that the direction the map spawned the player in is not inherited by
            // anything, so the viewer arrives looking wherever the room happened to face.
            // Reported from the headset: "I spawned and the camera was looking at my own
            // back."
            //
            // So turn the room onto the player once, on the way in. Not every frame: the
            // whole point of this mode is that the world does not rotate unless the viewer
            // asks, and following the player's yaw continuously would put every mouse
            // movement back into the picture.
            if (AfxVrMath::kVrModePlay == next.mode && AfxVrMath::kVrModePlay != g_Mode.mode) {
                float base[3];
                AfxVr_GetBaseAngles(base);
                float turn = AfxVrMath::NormalizeDegrees(base[1] - BodyForwardWorldDegrees());
                if (0.0f != turn) AfxVr_AddYaw(turn);

                // And stand where the player stands.
                //
                // The room's origin is captured on the session's first frame. Everything the
                // viewer has walked or leaned since is added to the camera - so arriving in a
                // game after wandering a metre across the room puts the eyes a metre out of
                // the body, looking at it from outside. Reported exactly that way: "I spawned
                // not in my body but about a metre to the right of it, I can see myself."
                //
                // Zeroing it here means the room's origin is wherever the viewer happens to
                // be standing when the map starts, which is the only defensible answer: there
                // is no other moment that means anything to them.
                g_RoomRefValid = false;
            }


            if (AfxVrMath::kVrModePlay == next.mode) {

                if (!g_FreeLookWasOn) {
                    g_FreeLookBeforePlay = AfxVr_GetFreeLook();
                    g_FreeLookWasOn = true;
                }
                AfxVr_SetFreeLook(true);
            } else if (g_FreeLookWasOn) {
                g_FreeLookWasOn = false;
                AfxVr_SetFreeLook(g_FreeLookBeforePlay);
            }

            if (next.sheet && !g_Mode.sheet) {

                // A synthetic cursor only reaches the game while it is the foreground
                // window, and after a launch it usually is not: the window in front is
                // whatever shell started it. Worn, that looked exactly like a broken
                // pointer - the dot moved, because drawing it needs no focus, and nothing
                // ever clicked.
                if (HWND hwnd = SwapChainWindow()) {
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                }
            }
            if (next.sheet && (!g_Mode.sheet || next.mode != g_Mode.mode)) g_MenuPlaced = false;
        }

        g_ModeInputs = in;
        g_Mode = next;
        g_PlayingDemo = in.demoPlaying;
        g_MenuMode = !next.worldInEyes && next.sheet;
    }

    // In low-latency mode this is where the frame's poses come from, located a few
    // microseconds ago rather than a whole frame ago.
    EngineThread_WaitAndLocate();
    EngineThread_PlaceMenu();
    EngineThread_PointAtMenu();

    XrView views[2];
    bool haveViews;
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        haveViews = g_ViewsValid;
        if (haveViews) {
            views[0] = g_Views[0];
            views[1] = g_Views[1];
        }
    }
    if (!haveViews) { PublishFrameRecord(nullptr); return; }

    // The head is the midpoint between the eyes: the spectator camera stays where the
    // demo put it, and each eye is offset from it.
    XrPosef base = views[0].pose;
    base.position.x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
    base.position.y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
    base.position.z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);

    // One orientation per eye, built as a quaternion, and both halves derived from it.
    //
    // The previous attempt turned the eye on the runtime's side by post-multiplying the
    // quaternion - a rotation about the eye's own up axis - while turning it on the game's
    // side by adding degrees to a Source yaw, which rotates about the world's vertical.
    // Those agree only while the head is level. Pitch down and they diverge, oppositely
    // for each eye, which reads as the two images rolling apart: level at the top of the
    // view, splitting towards the bottom. Deriving both from the same quaternion makes
    // that class of mistake impossible rather than merely fixed.
    const float d2r = (float)(M_PI / 180.0);
    XrQuaternionf renderOrientation[2];

    for (int pass = 0; pass < 2; pass++) {
        int eye = g_SwapEyes ? (1 - pass) : pass;

        XrQuaternionf ori = views[eye].pose.orientation;
        float fovDegrees = EffectiveFovDegrees(views[eye].fov);

        if (0 != g_CentreFrustum) {
            FrustumCentre c = CentreOfFrustum(views[eye].fov);
            float sign = (g_CentreFrustum < 0) ? -1.0f : 1.0f;
            // Local axes, so the turn means the same thing whatever the head is doing.
            ori = QuatMul(ori, QuatAxisAngle(0.0f, 1.0f, 0.0f,  sign * c.yawDegrees * d2r));
            ori = QuatMul(ori, QuatAxisAngle(1.0f, 0.0f, 0.0f, -sign * c.pitchDegrees * d2r));

            fovDegrees = 2.0f * c.halfHorizontal;
            if (g_FovOverrideDegrees > 0.0f) fovDegrees = g_FovOverrideDegrees;
            fovDegrees *= g_FovScale;
        }

        renderOrientation[eye] = ori;

        // The eye offset goes into the frame of the camera that will render it - which is
        // this rotated one, not the head's.
        float right, forward, up, dPitch, dYaw, dRoll;
        XrPosef frame = base;
        frame.orientation = ori;
        XrPoseToEye(views[eye].pose, frame, right, forward, up, dPitch, dYaw, dRoll);

        // dPitch/dYaw/dRoll came out of the eye's own pose; the camera must use the
        // orientation we just built instead.
        XrQuatToSourceAngles(ori, dPitch, dYaw, dRoll);

        if (0 == g_RollMode) dRoll = 0.0f;
        else if (g_RollMode < 0) dRoll = -dRoll;

        if (g_Monoscopic) {
            right = forward = up = 0.0f;
        } else {
            right *= g_IpdScale; forward *= g_IpdScale; up *= g_IpdScale;
        }

        AfxVr_SetEye(pass + 1, true, right, forward, up, dPitch, dYaw, dRoll, fovDegrees);
    }

    // The panel needs a head pose to be placed in front of, and there is none until the
    // session is up - so "mirv_vr_panel on" in a startup config could never place it, and
    // the panel silently did not appear.
    //
    // Placed a moment after the session becomes FOCUSED, which is the runtime's way of
    // saying the headset is on a face, and again every time it comes back. See the note on
    // g_PanelPlaceCountdown.
    if (g_PanelPlaceCountdown > 0) {
        g_PanelPlaceCountdown--;
        if (0 == g_PanelPlaceCountdown && g_PanelEnabled) {
            XrPosef mid = views[0].pose;
            mid.position.x = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
            mid.position.y = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
            mid.position.z = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
            PlacePanelFrom(mid);
            advancedfx::Message("AFXVR: panel placed in front of where you are looking.\n");
        }
    }

    // And the head itself, once, for everything that reads the camera once a frame
    // rather than once a pass: the audio listener, the client's world-to-screen matrix,
    // culling. Those never saw the viewer at all while the head lived only between
    // passes, which is why the sound stayed where the demo camera was.
    //
    // `base` carries views[0]'s orientation and the midpoint of the two positions. Both
    // eyes share an orientation while frustum centring is off, and centring is off.
    {
        float hPitch, hYaw, hRoll;
        XrQuatToSourceAngles(base.orientation, hPitch, hYaw, hRoll);
        if (0 == g_RollMode) hRoll = 0.0f;
        else if (g_RollMode < 0) hRoll = -hRoll;

        // Where the body has got to since the last recentre. The first frame sets the
        // reference, so a session never starts with the viewer displaced.
        if (!g_RoomRefValid) { g_RoomRefPos = base.position; g_RoomRefValid = true; }

        float rx = base.position.x - g_RoomRefPos.x;
        float ry = base.position.y - g_RoomRefPos.y;
        float rz = base.position.z - g_RoomRefPos.z;

        // Three metres while watching, where the viewer is a floating camera and walking
        // across the room is a feature. A THIRD of a metre while playing: there the eyes
        // belong to a body with a hitbox, and leaning out of it would let someone see round
        // a corner their character is not round.
        float limit = (AfxVrMath::kVrModePlay == g_Mode.mode) ? 0.35f : kRoomOffsetLimitMetres;
        float distance = sqrtf(rx * rx + ry * ry + rz * rz);
        if (distance > limit) {
            float k = limit / distance;
            rx *= k; ry *= k; rz *= k;
        }

        AfxVr_SetRoomIpdScale(g_IpdScale);
        // Not in menu mode. There the picture is a flat screen in the room, and moving the
        // menu background camera with the head would make it swim behind a screen that is
        // not moving.
        if (g_MenuMode) AfxVr_SetHead(false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        else AfxVr_SetHead(true, hPitch, hYaw, hRoll, EffectiveFovDegrees(views[0].fov), rx, ry, rz);
    }

    // What the projection layer reports has to be what was rendered, so publish the
    // orientations actually used rather than the raw ones.
    views[0].pose.orientation = renderOrientation[0];
    views[1].pose.orientation = renderOrientation[1];
    PublishFrameRecord(views);
}

unsigned long long MirvVrXr_EngineThread_FrameTicket() {
    std::lock_guard<std::mutex> lock(g_ViewMutex);
    return g_FrameSerial;
}

// A concrete format to view a back buffer through. The game's is R8G8B8A8_TYPELESS, which
// no view can name.
static DXGI_FORMAT TypedFormatFor(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return format;
    }
}

void MirvVrXr_ReleasePanelClearView() {
    if (g_PanelClearRtv) { g_PanelClearRtv->Release(); g_PanelClearRtv = nullptr; }
    g_PanelClearFor = nullptr;
}

static bool ModeWantsPanelClear(const AfxVrMath::ModeResult & mode) {
    // Never in menu mode: there the whole window is the point, world and all.
    // Over a world the sheet must retain alpha even when the optional HUD panel is off
    // or opaque; otherwise team select hides the stereo world behind a second picture.
    if (mode.sheet) return !mode.sheetOpaque;
    return g_PanelEnabled && g_PanelTransparent;
}

bool MirvVrXr_WantsPanelClear() {
    return ModeWantsPanelClear(g_Mode);
}

void MirvVrXr_RenderThread_ClearForPanel(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                        unsigned long long ticket) {
    FrameRecord frame;
    if (!pContext || !pTexture || !ReadFrameRecord(ticket, frame)) return;
    if (!ModeWantsPanelClear(frame.mode)) return;

    if (pTexture != g_PanelClearFor) {
        MirvVrXr_ReleasePanelClearView();

        D3D11_TEXTURE2D_DESC desc = {};
        pTexture->GetDesc(&desc);
        if (0 == (desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                advancedfx::Warning("AFXVR: the back buffer is not bindable as a render target; "
                    "the panel cannot be made transparent. Use mirv_vr_panel opaque.\n");
            }
            return;
        }

        ID3D11Device * pDevice = nullptr;
        pTexture->GetDevice(&pDevice);
        if (!pDevice) return;

        D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = TypedFormatFor(desc.Format);
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = 0;

        HRESULT hr = pDevice->CreateRenderTargetView(pTexture, &rtv, &g_PanelClearRtv);
        pDevice->Release();

        if (FAILED(hr) || !g_PanelClearRtv) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                advancedfx::Warning("AFXVR: CreateRenderTargetView for the panel failed (0x%08x).\n",
                    (unsigned)hr);
            }
            g_PanelClearRtv = nullptr;
            return;
        }
        g_PanelClearFor = pTexture;
    }

    if (!g_PanelClearRtv) return;

    // Transparent black. Every pixel the HUD does not touch stays exactly this, which is
    // what the runtime needs in order to composite the panel away to nothing.
    const float nothing[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    pContext->ClearRenderTargetView(g_PanelClearRtv, nothing);
}

// What did Panorama actually leave in the alpha channel? Copies the finished main pass
// into a staging texture and counts. Needs neither a headset nor a session - run it with
// mirv_vr_xr passes 2 and mirv_vr_panel on from a desk.
static void ProbePanelAlpha(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture) {
    D3D11_TEXTURE2D_DESC desc = {};
    pTexture->GetDesc(&desc);

    ID3D11Device * pDevice = nullptr;
    pTexture->GetDevice(&pDevice);
    if (!pDevice) return;

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ID3D11Texture2D * pStaging = nullptr;
    HRESULT hr = pDevice->CreateTexture2D(&staging, nullptr, &pStaging);
    pDevice->Release();
    if (FAILED(hr) || !pStaging) {
        advancedfx::Warning("mirv_vr_panel alpha: CreateTexture2D failed (0x%08x).\n", (unsigned)hr);
        return;
    }

    pContext->CopyResource(pStaging, pTexture);

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (SUCCEEDED(pContext->Map(pStaging, 0, D3D11_MAP_READ, 0, &map))) {
        // Alpha is the fourth byte in both RGBA and BGRA, which is the only thing about
        // the layout this needs to be right about.
        size_t opaque = 0, clear = 0, partial = 0, colourNoAlpha = 0, total = 0;
        for (UINT y = 0; y < desc.Height; y++) {
            const unsigned char * row = (const unsigned char*)map.pData + (size_t)y * map.RowPitch;
            for (UINT x = 0; x < desc.Width; x++) {
                const unsigned char * px = row + (size_t)x * 4;
                unsigned char a = px[3];
                bool lit = (px[0] | px[1] | px[2]) != 0;
                if (0 == a) { clear++; if (lit) colourNoAlpha++; }
                else if (255 == a) opaque++;
                else partial++;
                total++;
            }
        }
        pContext->Unmap(pStaging, 0);

        double n = total ? (double)total : 1.0;
        advancedfx::Message(
            "mirv_vr_panel alpha: %ux%u, alpha 0 %.2f%%, alpha 255 %.2f%%, in between %.2f%%\n"
            "  pixels with colour but no alpha: %.3f%%  <- if this is large the alpha channel is\n"
            "  write-masked and the panel will be invisible; if it is about zero the panel works.\n",
            desc.Width, desc.Height,
            100.0 * clear / n, 100.0 * opaque / n, 100.0 * partial / n,
            100.0 * colourNoAlpha / n);
    } else {
        advancedfx::Warning("mirv_vr_panel alpha: Map failed.\n");
    }

    pStaging->Release();
}

// A whole frame from the main pass alone, for a session with no map under it.
//
// The frame loop used to live entirely inside the eye passes: xrBeginFrame at the first
// eye, xrEndFrame at the second. A session with no world therefore began no frames and
// ended none, the runtime stopped scheduling xrWaitFrame, and the engine thread parked in
// it forever - taking the console command queue with it. Measured, not guessed: started at
// CS2's menu, the session reached "begun" and then the log went silent, a command sent down
// the pipe forty seconds later was echoed by the pipe thread and never executed, and the
// headset showed the runtime's own loading placeholder while the game went on burning a
// core.
//
// So here the main pass does the whole thing: wait, begin, copy the window, end with one
// quad and no projection layer. No extra passes are asked for at all - the view struct is
// not a camera in the menu, and there is nothing worth rendering twice.
//
// The quad carries the WHOLE back buffer, 2528x2780, not the 2560x1600 of it that reaches
// the monitor. What the desktop shows is cropped; what the headset gets is not.
// The white patch is refilled every frame rather than made a static swapchain image: a
// static one is acquired and released exactly once, and if anything goes wrong during that
// single chance there is no second. Eight by eight costs nothing to copy.
bool EnsureCursorTexture(ID3D11Device * pDevice, DXGI_FORMAT format) {
    if (g_CursorWhite) return true;
    if (!pDevice) return false;

    // An actual crosshair, not a white square.
    //
    // A square was what the first version drew, and worn it reads as a blob rather than as
    // an aiming mark: "the cursor is a square, it should be a crosshair". Four bars with a
    // gap in the middle, which is what every game draws and what lets you see the thing you
    // are about to shoot instead of covering it up.
    //
    // Drawn here rather than loaded, because a file is a thing that can be missing and this
    // is thirty-two pixels of arithmetic.
    std::vector<unsigned char> texels(kCursorTexels * kCursorTexels * 4, 0);
    const int centre = (int)kCursorTexels / 2;
    const int gap = 3;                       // clear space around the exact centre
    const int arm = (int)kCursorTexels / 2;  // out to the edge
    const int halfThickness = 1;

    for (int i = gap; i < arm; i++) {
        for (int t = -halfThickness; t <= halfThickness; t++) {
            const int spots[4][2] = {
                { centre + i, centre + t }, { centre - i, centre + t },
                { centre + t, centre + i }, { centre + t, centre - i }
            };
            for (int s = 0; s < 4; s++) {
                int px = spots[s][0], py = spots[s][1];
                if (px < 0 || py < 0 || px >= (int)kCursorTexels || py >= (int)kCursorTexels) continue;
                unsigned char * p = &texels[(py * (int)kCursorTexels + px) * 4];
                p[0] = p[1] = p[2] = 0xFF;
                p[3] = 0xFF;
            }
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kCursorTexels;
    desc.Height = kCursorTexels;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = &texels[0];
    data.SysMemPitch = kCursorTexels * 4;

    if (FAILED(pDevice->CreateTexture2D(&desc, &data, &g_CursorWhite))) {
        g_CursorWhite = nullptr;
        return false;
    }
    return true;
}

// Copies the white patch into the cursor swapchain and fills in the quad that shows it.
// Returns false if there is nothing to draw.
bool BuildCursorQuad(ID3D11DeviceContext * pContext, XrCompositionLayerQuad & quad,
                     const FrameRecord & frame) {
    if (!frame.valid || !frame.mode.sheet || !frame.mode.pointer || !frame.cursor.hit) return false;
    if (XR_NULL_HANDLE == g_Swapchain[kCursorSwapchain]) return false;
    if (!EnsureCursorTexture(g_pDevice, g_SwapchainFormat)) return false;

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage_(g_Swapchain[kCursorSwapchain], &acquire, &imageIndex),
               "xrAcquireSwapchainImage (cursor)")) return false;

    bool ok = false;
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    if (Check(xrWaitSwapchainImage_(g_Swapchain[kCursorSwapchain], &wait), "xrWaitSwapchainImage (cursor)")) {
        pContext->CopyResource(g_SwapchainImages[kCursorSwapchain][imageIndex], g_CursorWhite);
        ok = true;
    }
    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g_Swapchain[kCursorSwapchain], &release);
    if (!ok) return false;

    quad = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_Space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.pose = frame.cursor.pose;
    quad.subImage.swapchain = g_Swapchain[kCursorSwapchain];
    quad.subImage.imageRect.offset = { 0, 0 };
    quad.subImage.imageRect.extent = { (int32_t)kCursorTexels, (int32_t)kCursorTexels };
    quad.subImage.imageArrayIndex = 0;
    quad.size.width = g_CursorSizeMetres;
    quad.size.height = g_CursorSizeMetres;
    return true;
}

// The crosshair, drawn by us at the aim the game is actually using.
//
// Necessary as soon as the stick aims and the head looks: Panorama's crosshair is at the
// centre of the flat image, the eyes capture before the UI so it is not in them anyway,
// and the centre of the image is where the HEAD is pointed. Where the bullet goes is the
// game's own angles. Those two are the same thing only while the head is still.
//
// So: a small quad along the game's forward direction, expressed in the room. Head still,
// it sits dead centre; turn the head and it walks off to the side and stays over the thing
// that would be hit, which is the truth told honestly rather than a centred lie.
//
// Both eyes at ten metres: the disparity error against a target at any real distance is
// under half a degree, which is invisible, and one eye only is a trick that costs more
// than it saves here.
bool g_CrosshairEnabled = true;
float g_CrosshairDistanceMetres = 10.0f;
float g_CrosshairSizeDegrees = 0.8f;

bool BuildCrosshairQuad(ID3D11DeviceContext * pContext, XrCompositionLayerQuad & quad,
                        const FrameRecord & frame, unsigned long long ticket) {
    if (!g_CrosshairEnabled) return false;
    // Fallback projection views do not supply an aim. A zero or expired ticket used to
    // leave the last submitted record here, drawing that frame's crosshair over a new
    // world; even the two eyes must name the same record before its aim can be used.
    if (!ticket || !frame.valid || !frame.renderedValid || frame.serial != ticket) return false;
    if (AfxVrMath::kVrModePlay != frame.mode.mode || frame.mode.pointer) return false;
    if (XR_NULL_HANDLE == g_Swapchain[kCursorSwapchain]) return false;
    if (!g_PanelFollowValid) return false;
    if (!EnsureCursorTexture(g_pDevice, g_SwapchainFormat)) return false;

    // Where the game was aiming when THIS frame was composed, from the ticket rather than
    // from the globals - the render thread is behind by a varying fraction of a frame.
    //
    // Measured in the room from where the viewer faces, which is the panel anchor and not
    // the runtime's own forward. See RoomForwardYawRadians. Source counts pitch positive
    // downwards and OpenXR positive up, hence the negation.
    double yaw = frame.roomForwardRadians
        + AfxVrMath::NormalizeDegrees(frame.aimYaw - frame.bodyForwardWorld) * (M_PI / 180.0);
    double pitch = -frame.aimPitch * (M_PI / 180.0);

    double cosPitch = cos(pitch);
    XrVector3f direction;
    direction.x = (float)(-sin(yaw) * cosPitch);
    direction.y = (float)( sin(pitch));
    direction.z = (float)(-cos(yaw) * cosPitch);

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!Check(xrAcquireSwapchainImage_(g_Swapchain[kCursorSwapchain], &acquire, &imageIndex),
               "xrAcquireSwapchainImage (crosshair)")) return false;

    bool ok = false;
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    if (Check(xrWaitSwapchainImage_(g_Swapchain[kCursorSwapchain], &wait), "xrWaitSwapchainImage (crosshair)")) {
        pContext->CopyResource(g_SwapchainImages[kCursorSwapchain][imageIndex], g_CursorWhite);
        ok = true;
    }
    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g_Swapchain[kCursorSwapchain], &release);
    if (!ok) return false;

    quad = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_Space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.pose.position.x = g_PanelFollowPos.x + direction.x * g_CrosshairDistanceMetres;
    quad.pose.position.y = g_PanelFollowPos.y + direction.y * g_CrosshairDistanceMetres;
    quad.pose.position.z = g_PanelFollowPos.z + direction.z * g_CrosshairDistanceMetres;
    AfxVrMath::YawThenPitchQuat((float)yaw, (float)pitch,
                                quad.pose.orientation.x, quad.pose.orientation.y,
                                quad.pose.orientation.z, quad.pose.orientation.w);
    quad.subImage.swapchain = g_Swapchain[kCursorSwapchain];
    quad.subImage.imageRect.offset = { 0, 0 };
    quad.subImage.imageRect.extent = { (int32_t)kCursorTexels, (int32_t)kCursorTexels };
    quad.subImage.imageArrayIndex = 0;

    // An angular size, so it looks the same whatever distance it is put at.
    float half = tanf(0.5f * g_CrosshairSizeDegrees * (float)(M_PI / 180.0));
    quad.size.width = 2.0f * g_CrosshairDistanceMetres * half;
    quad.size.height = quad.size.width;
    return true;
}

static bool BuildSheetQuad(XrCompositionLayerQuad & quad, const FrameRecord & frame) {
    if (!frame.valid || !frame.mode.sheet || !frame.sheetPlaced) return false;
    if (XR_NULL_HANDLE == g_Swapchain[kPanelSwapchain] || !g_SwapchainWidth || !g_SwapchainHeight) return false;

    // The whole window, with exactly the pose and size the engine hit-tested. Cutting it
    // into the HUD groups would drop the middle of team select; the sheet is one extra quad
    // over the eyes, while the existing groups remain the path when no sheet is requested.
    quad = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    quad.layerFlags = frame.mode.sheetOpaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_Space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.pose = frame.sheetPose;
    quad.subImage.swapchain = g_Swapchain[kPanelSwapchain];
    quad.subImage.imageRect.offset = { 0, 0 };
    quad.subImage.imageRect.extent = { (int32_t)g_SwapchainWidth, (int32_t)g_SwapchainHeight };
    quad.subImage.imageArrayIndex = 0;
    quad.size = frame.sheetSize;
    return true;
}

static void RenderThread_MenuFrame(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                   const FrameRecord & frame) {
    // A frame may already be open, and that is exactly the case this has to handle rather
    // than step around.
    //
    // The switch to menu mode happens on the engine thread, between one frame's passes and
    // the next. If it lands after the first eye has begun a frame and before the second
    // would have ended it, that frame is orphaned: begun, never ended, and every later
    // frame is refused by the runtime. The first version of this function returned early
    // on g_FrameBegun and did precisely that - the mode switched, the log said so, and the
    // headset kept showing a loading screen because one frame from the old mode was still
    // open. So: if a frame is open, finish it; only open a new one if there is none.
    if (!g_FrameBegun) {
        if (g_LowLatency) {
            if (!g_FrameWaited) return;
            if (frame.timingValid) {
                g_RtFrameState = { XR_TYPE_FRAME_STATE };
                g_RtFrameState.predictedDisplayTime = frame.displayTime;
                g_RtFrameState.shouldRender = frame.shouldRender;
            } else {
                std::lock_guard<std::mutex> lock(g_ViewMutex);
                g_RtFrameState = g_FrameState;
            }
            g_FrameWaited = false;
        } else {
            g_FrameState = { XR_TYPE_FRAME_STATE };
            XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
            if (!Check(xrWaitFrame_(g_Session, &waitInfo, &g_FrameState), "xrWaitFrame (menu)")) return;
            g_RtFrameState = g_FrameState;
        }

        XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
        if (!Check(xrBeginFrame_(g_Session, &beginInfo), "xrBeginFrame (menu)")) return;
        g_FrameBegun = true;
    }

    // Once, because EnsureSwapchains takes the XR swapchains' size and format from the
    // first texture it is handed, and in a menu-first launch that is now the swap chain's
    // buffer 0 rather than the in-game render target. If the two ever differ, going from
    // menu to demo would copy between mismatched textures and CopyResource would silently
    // do nothing.
    {
        static bool reported = false;
        if (!reported) {
            reported = true;
            D3D11_TEXTURE2D_DESC desc = {};
            pTexture->GetDesc(&desc);
            advancedfx::Message("AFXVR: menu frame source %ux%u format %i samples %u\n",
                desc.Width, desc.Height, (int)desc.Format, desc.SampleDesc.Count);
        }
    }

    g_EyesCopied = 0;


    // The eye swapchains are made here too. They are not used in this mode, but making
    // them now means going back to a demo does not cost a frame of nothing while they are
    // created - and it keeps one place that knows how big the back buffer is.
    bool haveSheet = EnsureSwapchains(pTexture) && g_RtFrameState.shouldRender
        && XR_NULL_HANDLE != g_Swapchain[kPanelSwapchain]
        && g_SwapchainWidth > 0 && g_SwapchainHeight > 0;

    if (haveSheet) {
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (Check(xrAcquireSwapchainImage_(g_Swapchain[kPanelSwapchain], &acquire, &imageIndex),
                  "xrAcquireSwapchainImage (menu)")) {
            XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wait.timeout = XR_INFINITE_DURATION;
            if (Check(xrWaitSwapchainImage_(g_Swapchain[kPanelSwapchain], &wait),
                      "xrWaitSwapchainImage (menu)")) {
                pContext->CopyResource(g_SwapchainImages[kPanelSwapchain][imageIndex], pTexture);
            } else {
                haveSheet = false;
            }
            XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage_(g_Swapchain[kPanelSwapchain], &release);
        } else {
            haveSheet = false;
        }
    }

    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    haveSheet = haveSheet && BuildSheetQuad(quad, frame);

    // The dot goes on top of the sheet, so it is submitted after it: layer order is paint
    // order, and a cursor behind the thing it points at is not a cursor.
    XrCompositionLayerQuad cursor = {};
    bool haveCursor = haveSheet && BuildCursorQuad(pContext, cursor, frame);

    const XrCompositionLayerBaseHeader * layers[2];
    int layerCount = 0;
    if (haveSheet) {
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
        if (haveCursor) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&cursor;
    }

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = g_RtFrameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = (uint32_t)layerCount;
    endInfo.layers = layerCount ? layers : nullptr;

    // An empty frame is submitted deliberately when the sheet is not ready yet. Ending
    // every frame that was begun is the whole point of this function: the alternative is
    // the silence it exists to fix.
    Check(xrEndFrame_(g_Session, &endInfo), "xrEndFrame (menu)");
    g_FrameBegun = false;
    g_PanelCopied = false;

    // The first frame that actually carried the sheet, said once, in words a launcher can
    // wait for. cs2vr.exe follows console.log and stops when it sees this; without it, the
    // most ordinary start of all - no demo, straight to CS2's own menu in the headset -
    // never announces itself, because "submitting frames to the headset" is reported on two
    // eyes being copied and menu mode copies none. A minute of silence and then "still
    // nothing from the hook" is what success looked like.
    if (!g_ReportedFirstMenu && layerCount > 0) {
        g_ReportedFirstMenu = true;
        advancedfx::Message("AFXVR: showing the menu in the headset. Point a controller at it.\n");
    }

    ULONGLONG now = GetTickCount64();
    if (0 == g_FpsWindowStart) g_FpsWindowStart = now;
    g_FpsFrames++;
    if (now - g_FpsWindowStart >= 2000) {
        g_SubmitFps = 1000.0f * g_FpsFrames / (float)(now - g_FpsWindowStart);
        g_FpsWindowStart = now;
        g_FpsFrames = 0;
        if (g_LogFps) {
            advancedfx::Message("AFXVR: menu screen, %.1f frames/s at %ux%u%s%s\n",
                g_SubmitFps, g_SwapchainWidth, g_SwapchainHeight,
                (XR_SESSION_STATE_FOCUSED == g_State) ? "" : "  session ",
                (XR_SESSION_STATE_FOCUSED == g_State) ? "" : SessionStateName(g_State));
        }
    }
}

void MirvVrXr_RenderThread_SubmitPanel(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                     unsigned long long ticket) {
    g_PanelCopied = false;
    g_PanelTicket = 0;
    if (!pContext || !pTexture) return;
    FrameRecord frame;
    bool haveRecord = ReadFrameRecord(ticket, frame);
    if (!g_PanelEnabled && !frame.mode.sheet) return;

    // Before the session gate: the whole value of the probe is that it answers the
    // question without a headset.
    if (g_PanelAlphaProbe > 0) {
        g_PanelAlphaProbe--;
        ProbePanelAlpha(pContext, pTexture);
    }

    if (!g_SessionRunning || !haveRecord) return;

    // No map: the whole frame happens here, because nothing else will.
    //
    // Reaching this at all needed a change in HLAE (patch 002). This callback is invoked
    // through g_BeforeUiRT, which is captured by CAfxRenderCallbackBeforeUi - and that is
    // queued only when the scene system announces "CSGOHud". At the menu the view is
    // called CSGOMainMenu, the name test fails, and nothing is captured. The patch falls
    // back to the swap chain's own buffer 0 when there is no capture, which is the
    // finished frame and is what this wants.
    if (!frame.mode.worldInEyes && frame.mode.sheet) {
        RenderThread_MenuFrame(pContext, pTexture, frame);
        return;
    }




    if (!EnsureSwapchains(pTexture) || XR_NULL_HANDLE == g_Swapchain[kPanelSwapchain]) return;

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
    bool copied = false;
    if (Check(xrWaitSwapchainImage_(g_Swapchain[kPanelSwapchain], &wait), "xrWaitSwapchainImage (panel)")) {
        pContext->CopyResource(g_SwapchainImages[kPanelSwapchain][imageIndex], pTexture);
        copied = true;
    }

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (Check(xrReleaseSwapchainImage_(g_Swapchain[kPanelSwapchain], &release), "xrReleaseSwapchainImage (panel)")
        && copied) {
        g_PanelCopied = true;
        g_PanelTicket = ticket;
    }
}

void MirvVrXr_RenderThread_SubmitEye(int eyeIndex, ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                     unsigned long long ticket) {
    if (eyeIndex < 0 || eyeIndex > 1) return;
    if (!pContext || !pTexture) return;
    // Forced passes render but go nowhere: every call below needs a session, and half the
    // function pointers are null without one.
    if (!g_SessionRunning) return;

    if (0 == eyeIndex) {
        ReadFrameRecord(ticket, g_SubmittedFrame);
        g_SubmittedTicket = ticket;
        // The pass loop already chose to queue these eyes. The current engine mode can
        // have changed since then; only this ticket can decide whether they own a world.
        // With a missing ticket keep the old projection fallback, but no mode overlays.
        if (g_SubmittedFrame.valid && !g_SubmittedFrame.mode.worldInEyes && g_ForcedPasses <= 0) return;

        if (g_LowLatency) {
            // The engine thread waited and located at the top of this frame. If it has
            // not - the session only just started, say - there is nothing to begin.
            if (!g_FrameWaited) return;
            // Take a copy now. The engine thread is free to wait again for the next frame
            // the moment g_FrameWaited is cleared, and it writes g_FrameState when it
            // does - so the display time this frame is ended with must not be read from
            // there later on.
            if (g_SubmittedFrame.timingValid) {
                g_RtFrameState = { XR_TYPE_FRAME_STATE };
                g_RtFrameState.predictedDisplayTime = g_SubmittedFrame.displayTime;
                g_RtFrameState.shouldRender = g_SubmittedFrame.shouldRender;
            } else {
                std::lock_guard<std::mutex> lock(g_ViewMutex);
                g_RtFrameState = g_FrameState;
            }
            g_FrameWaited = false;
        } else {
            g_FrameState = { XR_TYPE_FRAME_STATE };
            XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
            LARGE_INTEGER waitStart = StageStart();
            if (!Check(xrWaitFrame_(g_Session, &waitInfo, &g_FrameState), "xrWaitFrame")) return;
            g_StageWaitFrame.Add(1000.0 * SecondsSince(waitStart));
            // The render thread is the only writer in this mode, but the copy keeps the
            // rest of the function reading one thing rather than two.
            g_RtFrameState = g_FrameState;
        }

        XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
        if (!Check(xrBeginFrame_(g_Session, &beginInfo), "xrBeginFrame")) return;
        g_FrameBegun = true;

        if (!g_LowLatency) {
            ProcessInput(g_SubmittedFrame.mode);
            LocateViews(g_RtFrameState.predictedDisplayTime);
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

            const FrameRecord & record = g_SubmittedFrame;
            if (record.valid) {
                // A copy, so everything the crosshair needs comes from one instant and
                // not from whatever the engine thread has moved on to. It outlives this
                // block because the layers are assembled when the SECOND eye arrives.
                if (record.renderedValid) {
                    rendered[0] = record.rendered[0];
                    rendered[1] = record.rendered[1];
                    g_ProjViewsValid = true;
                }

                // In low-latency mode the engine thread did this frame's xrWaitFrame, so
                // the time it predicted travels with the frame too. In safe mode the
                // render thread's own wait, a few lines up, is the right one.
                if (record.timingValid) {
                    g_RtFrameState.predictedDisplayTime = record.displayTime;
                    g_RtFrameState.shouldRender = record.shouldRender;
                }

                unsigned long long lag = g_FrameSerial - ticket;
                g_TicketLagSum += lag;
                g_TicketLagCount++;
                if (lag > g_TicketLagWorst) g_TicketLagWorst = lag;
            } else {
                // Further behind than the ring is deep, or no frame published yet. The old
                // behaviour, which is wrong in exactly the way the ticket exists to fix -
                // so it is counted and reported rather than passed over.
                if (0 != ticket) g_TicketMisses++;
                if (g_RenderedViewsValid) { rendered[0] = g_RenderedViews[0]; rendered[1] = g_RenderedViews[1]; g_ProjViewsValid = true; }
                else if (g_ViewsValid) { rendered[0] = g_Views[0]; rendered[1] = g_Views[1]; g_ProjViewsValid = true; }
            }
        }

        // Monoscopic means both images really were drawn from the midpoint, so both must be
        // *reported* from the midpoint too. Reporting the true eye poses for identical
        // images is what the runtime is asked to reconcile, and it reconciles it by pulling
        // them apart - which made the one diagnostic that was supposed to be unambiguous
        // fail by construction.
        if (g_Monoscopic && g_ProjViewsValid) {
            XrVector3f mid;
            mid.x = 0.5f * (g_ProjViews[0].pose.position.x + g_ProjViews[1].pose.position.x);
            mid.y = 0.5f * (g_ProjViews[0].pose.position.y + g_ProjViews[1].pose.position.y);
            mid.z = 0.5f * (g_ProjViews[0].pose.position.z + g_ProjViews[1].pose.position.z);
            rendered[0].pose.position = mid;
            rendered[1].pose.position = mid;
        }

        // Nothing located yet. The frame still has to be ended -- leaving one open is what
        // stalls the runtime -- so fall through with no layer rather than returning.
        for (int eye = 0; g_ProjViewsValid && eye < 2; eye++) {
            g_ProjViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            g_ProjViews[eye].pose = rendered[eye].pose;

            // No rotation here. g_RenderedViews already carries the orientation each eye
            // was actually rendered with, centring included - which is the only way the
            // two can be guaranteed to agree.
            FrustumCentre centre = CentreOfFrustum(rendered[eye].fov);

            // Report the symmetric frustum actually rendered, not the runtime's
            // asymmetric recommendation. The vertical half-angle follows from the image
            // aspect - equal to the horizontal one only when the image is square, which
            // is what the first 1080x1080 test happened to be.
            // What the image is claimed to cover must be what the image actually covers -
            // the frustum we WANTED, not the number we handed the engine to get it. With
            // the aspect fix off those are the same; with it on they differ by exactly the
            // engine's 4:3 convention, which is the point.
            // Claim what the engine actually rendered, measured from its own projection
            // matrix, not what we asked it for.
            //
            // CS2 clamps the view field of view. Ask for 127 degrees on this window and it
            // renders 68.6 horizontal, 73.7 vertical - which is exactly fov 90 at this
            // aspect, the engine's own ceiling. We were claiming 108. The ratio of tangents
            // is 2.0, so the runtime shrank the image to half size and the world looked
            // twice as far away as it is, which is precisely how it was described.
            //
            // Measuring removes the whole question. Whatever the engine does with the
            // request - honours it, clamps it, ignores it - the claim matches the image.
            float measuredH = 0.0f, measuredV = 0.0f;
            bool measured = g_UseMeasuredFov && AfxVr_GetRenderedFovDegrees(&measuredH, &measuredV)
                            && measuredH > 1.0f && measuredV > 1.0f;

            float claimed = WantedFovDegrees(rendered[eye].fov);
            if (measured) claimed = measuredH;
            if (0 != g_CentreFrustum) {
                claimed = 2.0f * centre.halfHorizontal;
                if (g_FovOverrideDegrees > 0.0f) claimed = g_FovOverrideDegrees;
                claimed *= g_FovScale;
            }
            if (g_ReportedFovOverrideDegrees > 0.0f) claimed = g_ReportedFovOverrideDegrees;
            float half = 0.5f * claimed * (float)(M_PI / 180.0);
            float vHalf = half;
            if (g_FovVerticalOverrideDegrees > 0.0f) {
                vHalf = 0.5f * g_FovVerticalOverrideDegrees * (float)(M_PI / 180.0);
            } else if (measured) {
                // Measured too, rather than derived from the aspect. The engine's vertical
                // is its own business and it has now been read rather than modelled.
                vHalf = 0.5f * measuredV * (float)(M_PI / 180.0);
            } else if (g_SwapchainWidth && g_SwapchainHeight) {
                vHalf = atanf(tanf(half) * (float)g_SwapchainHeight / (float)g_SwapchainWidth);
            }

            g_ProjViews[eye].fov.angleLeft = -half;
            g_ProjViews[eye].fov.angleRight = half;
            g_ProjViews[eye].fov.angleUp = vHalf;
            g_ProjViews[eye].fov.angleDown = -vHalf;

            // Claiming a frustum only works on a runtime that reads the claim.
            //
            // XrViewConfigurationProperties::fovMutable says whether it does, and it was
            // never checked. Measured: the Oculus PC runtime reports FALSE and composites
            // with its OWN asymmetric frustum whatever the layer says; SteamVR reports
            // TRUE and honours it. Which is why the same build fused better on SteamVR.
            //
            // On a runtime that ignores the claim, our symmetric image is stretched onto
            // [-54, +40] for the left eye and its mirror for the right. The stretch is
            // linear in tangent space, so the centre ray lands at
            // atan((tan(40) - tan(54)) / 2) = -15 degrees, mirrored - about thirty degrees
            // of divergence that does not shrink with distance. Only something a few
            // centimetres away can be fused through that, which is exactly what was
            // reported: a weapon in front of the face fused, nothing beyond it did.
            //
            // The fix is Meta's own documented "symmetric projection": keep rendering
            // symmetric, hand back the runtime's frustum unchanged, and point it at the
            // sub-rectangle of our image that that frustum actually covers. A runtime that
            // honours fov and one that ignores it then produce the same picture, so this
            // is not a workaround for one runtime - it is simply correct.
            if (g_CropToRuntimeFov && g_SwapchainWidth && g_SwapchainHeight) {
                float t = tanf(half);
                float v = tanf(vHalf);
                if (t > 1e-6f && v > 1e-6f) {
                    const XrFovf & want = rendered[eye].fov;

                    // Tangent space, not angle space: perspective interpolates tangents.
                    float x0 = g_SwapchainWidth  * (t + tanf(want.angleLeft))  / (2.0f * t);
                    float x1 = g_SwapchainWidth  * (t + tanf(want.angleRight)) / (2.0f * t);
                    float y0 = g_SwapchainHeight * (v - tanf(want.angleUp))    / (2.0f * v);
                    float y1 = g_SwapchainHeight * (v - tanf(want.angleDown))  / (2.0f * v);

                    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0);
                    int ix1 = (int)ceilf(x1),  iy1 = (int)ceilf(y1);

                    if (ix0 < 0) ix0 = 0;
                    if (iy0 < 0) iy0 = 0;
                    if (ix1 > (int)g_SwapchainWidth)  ix1 = (int)g_SwapchainWidth;
                    if (iy1 > (int)g_SwapchainHeight) iy1 = (int)g_SwapchainHeight;

                    if (ix1 - ix0 >= 16 && iy1 - iy0 >= 16) {
                        // Re-derive the frustum from the rectangle after snapping to whole
                        // pixels, so the claim still matches the image to the pixel. On a
                        // runtime that ignores the claim this costs nothing; on one that
                        // reads it, it is the difference between right and nearly right.
                        g_ProjViews[eye].fov.angleLeft  = atanf(t * (2.0f * ix0 / (float)g_SwapchainWidth  - 1.0f));
                        g_ProjViews[eye].fov.angleRight = atanf(t * (2.0f * ix1 / (float)g_SwapchainWidth  - 1.0f));
                        g_ProjViews[eye].fov.angleUp    = atanf(v * (1.0f - 2.0f * iy0 / (float)g_SwapchainHeight));
                        g_ProjViews[eye].fov.angleDown  = atanf(v * (1.0f - 2.0f * iy1 / (float)g_SwapchainHeight));

                        g_CropRect[eye].offset = { ix0, iy0 };
                        g_CropRect[eye].extent = { ix1 - ix0, iy1 - iy0 };
                        g_CropValid[eye] = true;
                        continue;
                    }
                }
            }
            g_CropValid[eye] = false;
        }
    }

    if (!g_FrameBegun) return;

    // Never finish a stereo pair with an eye from another ticket. End the outstanding
    // frame empty instead; otherwise both the projection and the crosshair claim one
    // frame's camera for an image partly rendered by another.
    bool sameTicket = ticket == g_SubmittedTicket;
    if (!sameTicket) { g_EyesCopied = 0; g_ProjViewsValid = false; }
    bool canCopy = sameTicket && g_RtFrameState.shouldRender && XR_NULL_HANDLE != g_Swapchain[eyeIndex];

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
            if (g_CropValid[eye]) {
                g_ProjViews[eye].subImage.imageRect = g_CropRect[eye];
            } else {
                g_ProjViews[eye].subImage.imageRect.offset = { 0, 0 };
                g_ProjViews[eye].subImage.imageRect.extent = { (int32_t)g_SwapchainWidth, (int32_t)g_SwapchainHeight };
            }
            g_ProjViews[eye].subImage.imageArrayIndex = 0;
        }

        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = g_Space;
        layer.viewCount = 2;
        layer.views = g_ProjViews;

        // The panel goes on top of the world, so its quads come after: the runtime
        // composites layers in the order given.
        //
        // The HUD was drawn source-over onto a zeroed target, so its colour is already
        // premultiplied by its alpha. Which is why UNPREMULTIPLIED is deliberately not set:
        // that would ask the runtime to divide the colour by the alpha a second time and
        // the edge of every glyph would bloom.
        const XrCompositionLayerFlags panelFlags = g_PanelTransparent
            ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
            : 0;

        XrCompositionLayerQuad quads[kMaxPanelQuads];
        int quadCount = 0;

        bool havePanelImage = g_SubmittedFrame.valid && g_PanelTicket == g_SubmittedFrame.serial && g_PanelCopied
            && XR_NULL_HANDLE != g_Swapchain[kPanelSwapchain]
            && g_SwapchainWidth > 0 && g_SwapchainHeight > 0;
        bool havePanel = havePanelImage && !g_SubmittedFrame.mode.sheet
            && g_PanelEnabled && g_PanelShown && g_PanelPlaced;
        bool haveSheet = havePanelImage && g_SubmittedFrame.mode.worldInEyes
            && BuildSheetQuad(quads[quadCount], g_SubmittedFrame);
        if (haveSheet) quadCount++;

        // From the same poses the frame is being submitted with, so the panels and the
        // world cannot disagree about where the head is.
        if (g_ProjViewsValid) {
            XrVector3f mid;
            mid.x = 0.5f * (g_ProjViews[0].pose.position.x + g_ProjViews[1].pose.position.x);
            mid.y = 0.5f * (g_ProjViews[0].pose.position.y + g_ProjViews[1].pose.position.y);
            mid.z = 0.5f * (g_ProjViews[0].pose.position.z + g_ProjViews[1].pose.position.z);
            if (!g_PanelFollowValid) {
                g_PanelFollowPos = mid;
                g_PanelFollowValid = true;
            } else {
                g_PanelFollowPos.x += (mid.x - g_PanelFollowPos.x) * kPanelFollowAlpha;
                g_PanelFollowPos.y += (mid.y - g_PanelFollowPos.y) * kPanelFollowAlpha;
                g_PanelFollowPos.z += (mid.z - g_PanelFollowPos.z) * kPanelFollowAlpha;
            }
        }

        if (havePanel && g_PanelCutUp) {
            for (int i = 0; i < kPanelRegionCount && quadCount < kMaxPanelQuads; i++) {
                const PanelRegion & region = g_PanelRegions[i];
                if (!region.enabled) continue;
                // The timeline and the strip naming who is being watched exist only in a
                // recording. In a game they are not there to copy, and the rect would
                // carry whatever the player's own HUD happens to put in those rows.
                if (region.demoOnly && AfxVrMath::kVrModeWatch != g_SubmittedFrame.mode.mode) continue;
                if (region.gameOnly && AfxVrMath::kVrModePlay != g_SubmittedFrame.mode.mode) continue;

                int x0 = (int)(region.u0 * (float)g_SwapchainWidth  + 0.5f);
                int x1 = (int)(region.u1 * (float)g_SwapchainWidth  + 0.5f);
                int y0 = (int)(region.v0 * (float)g_SwapchainHeight + 0.5f);
                int y1 = (int)(region.v1 * (float)g_SwapchainHeight + 0.5f);
                if (x1 <= x0 || y1 <= y0) continue;

                XrCompositionLayerQuad & quad = quads[quadCount++];
                quad = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
                quad.layerFlags = panelFlags;
                quad.space = g_Space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.pose = PlaceRegion(region);
                quad.subImage.swapchain = g_Swapchain[kPanelSwapchain];
                quad.subImage.imageRect.offset = { x0, y0 };
                quad.subImage.imageRect.extent = { x1 - x0, y1 - y0 };
                quad.subImage.imageArrayIndex = 0;

                // The angular width is the setting; the metres follow from the distance,
                // and the height follows from the piece's own shape so nothing is
                // stretched.
                float halfWidth = tanf(0.5f * region.widthDegrees * (float)(M_PI / 180.0));
                quad.size.width = 2.0f * region.distanceMetres * halfWidth;
                quad.size.height = quad.size.width * (float)(y1 - y0) / (float)(x1 - x0);
            }
        } else if (havePanel) {
            XrCompositionLayerQuad & quad = quads[quadCount++];
            quad = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
            quad.layerFlags = panelFlags;
            quad.space = g_Space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.pose = g_PanelPose;
            quad.subImage.swapchain = g_Swapchain[kPanelSwapchain];
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { (int32_t)g_SwapchainWidth, (int32_t)g_SwapchainHeight };
            quad.subImage.imageArrayIndex = 0;
            quad.size.width = g_PanelWidthMetres;
            // Keep the source aspect, whatever it happens to be. The window is the eye
            // size, so on this machine the sheet is taller than it is wide -- ugly, but
            // honest, and stretching text is worse than an odd shape.
            quad.size.height = g_PanelWidthMetres * (float)g_SwapchainHeight / (float)g_SwapchainWidth;
        }

        // The world, the sheet or HUD groups, and the pointer or crosshair.
        const XrCompositionLayerBaseHeader * layers[3 + kMaxPanelQuads];
        int layerCount = 0;

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = g_RtFrameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

        bool haveWorld = (2 == g_EyesCopied) && g_ProjViewsValid;

        // Where the bullet would go, which is not the centre of the picture as soon as the
        // stick aims and the head looks.
        XrCompositionLayerQuad crosshair = {};
        bool haveCrosshair = haveWorld && BuildCrosshairQuad(pContext, crosshair, g_SubmittedFrame, ticket);

        // As in the menu-only path, the pointer is painted after the whole-window sheet.
        // It must not appear on a failed panel copy or on the cut-up HUD in its place.
        XrCompositionLayerQuad cursor = {};
        bool haveCursor = haveWorld && haveSheet && BuildCursorQuad(pContext, cursor, g_SubmittedFrame);

        if (haveWorld) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&layer;
        // Only with a world layer under them: a frame of nothing but HUD quads is a frame
        // with no world in it, which is worse than a frame with no HUD.
        if (haveWorld) {
            for (int i = 0; i < quadCount; i++) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quads[i];
            if (haveCursor) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&cursor;
            if (haveCrosshair) layers[layerCount++] = (XrCompositionLayerBaseHeader*)&crosshair;
        }

        endInfo.layerCount = (uint32_t)layerCount;
        endInfo.layers = layerCount ? layers : nullptr;

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
                // The state, but only when it is not the one that means "we have the
                // headset". A collapsed frame rate with the session merely VISIBLE is the
                // compositor declining to schedule us, not a regression in the renderer,
                // and reading it as the latter has already cost an afternoon.
                advancedfx::Message("AFXVR: %.1f frames/s submitted at %ux%u per eye (%.2f ms)%s%s\n",
                    g_SubmitFps, g_SwapchainWidth, g_SwapchainHeight,
                    g_SubmitFps > 0.0f ? 1000.0f / g_SubmitFps : 0.0f,
                    (XR_SESSION_STATE_FOCUSED == g_State) ? "" : "  session ",
                    (XR_SESSION_STATE_FOCUSED == g_State) ? "" : SessionStateName(g_State));
                advancedfx::Message(
                    "AFXVR:   xrWaitFrame %.2f ms (worst %.2f)  locate %.2f  copy %.2f x%i  xrEndFrame %.2f\n",
                    g_StageWaitFrame.Mean(), g_StageWaitFrame.worstMs,
                    g_StageLocate.Mean(), g_StageCopy.Mean(),
                    g_StageCopy.count ? g_StageCopy.count / (g_StageWaitFrame.count ? g_StageWaitFrame.count : 1) : 0,
                    g_StageEndFrame.Mean());
                // How far behind the engine thread the render thread was when it reported
                // each frame's pose. A constant is fine - the runtime corrects for a known
                // offset. A varying one is judder, and used to be invisible.
                advancedfx::Message(
                    "AFXVR:   render thread %.2f frames behind (worst %llu)%s\n",
                    g_TicketLagCount ? (double)g_TicketLagSum / (double)g_TicketLagCount : 0.0,
                    (unsigned long long)g_TicketLagWorst,
                    g_TicketMisses ? "  SOME FRAMES FELL OUT OF THE RING" : "");
                g_TicketLagSum = 0; g_TicketLagCount = 0; g_TicketLagWorst = 0; g_TicketMisses = 0;
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
        if (!_stricmp(arg1, "stop"))    { MirvVrXr_SessionStop(); AfxVr_SetEye(1,false,0,0,0,0,0,0,0); AfxVr_SetEye(2,false,0,0,0,0,0,0,0); AfxVr_SetHead(false,0,0,0,0,0,0,0); g_RoomRefValid = false; advancedfx::Message("AFXVR: session stopped.\n"); return; }
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

CON_COMMAND(mirv_vr_playframe, "cs2-vr-spectator: whether the hands belong to the body or to the headset.")
{
    if (2 <= args->ArgC()) {
        const char * arg1 = args->ArgV(1);
        if (!_stricmp(arg1, "body")) g_PlayFrameBody = true;
        else if (!_stricmp(arg1, "head")) g_PlayFrameBody = false;
        AfxVr_SetHeadAnglesOncePerFrame(
            !(AfxVrMath::kVrModePlay == g_Mode.mode && g_PlayFrameBody));
    }

    advancedfx::Message(
        "mirv_vr_playframe body|head - where the weapon model and the arms live.\n"
        "\n"
        "The engine places them from the view it builds once a frame. This project writes\n"
        "the head into that view, for the audio listener and the client's matrices - and\n"
        "the hands came along, so the gun is glued to the headset and turns with it.\n"
        "\n"
        "body: leave that view's ANGLES to the game while playing. The gun goes back onto\n"
        "      the aim, so it stays where the hands are when you look away from it. The\n"
        "      eyes are a separate write and still get the head.\n"
        "head: as before.\n"
        "\n"
        "The cost of body, said plainly: the listener is one of those once-a-frame\n"
        "consumers, so in a game the sound field may follow the gun hand rather than the\n"
        "head. That is a partial give-back of something confirmed worn this morning, and\n"
        "it is not yet known whether a live game takes the listener from this view at all.\n"
        "\n"
        "Watching a demo is untouched either way.\n"
        "Current value: %s\n",
        g_PlayFrameBody ? "body" : "head");
}

CON_COMMAND(mirv_vr_aim, "cs2-vr-spectator: what moves the aim while playing a map.")

{
    if (2 <= args->ArgC()) {
        const char * arg1 = args->ArgV(1);
        int wanted = g_AimMethod;
        if (!_stricmp(arg1, "hand"))       wanted = kAimHand;
        else if (!_stricmp(arg1, "stick")) wanted = kAimStick;
        else if (!_stricmp(arg1, "off"))   wanted = kAimOff;

        if (wanted != g_AimMethod) {
            g_AimMethod = wanted;
            StopHandAim();
            ResetAimLearning();
        }
    }

    advancedfx::Message(
        "mirv_vr_aim hand|stick|off\n"
        "\n"
        "hand  - the right controller points and the game's aim follows it, which is what\n"
        "        every VR shooter does and what UEVR calls \"Aim Method: Right Controller\".\n"
        "        The right thumbstick turns the room, as it does in a demo. The crosshair\n"
        "        is the only truthful sign of where a shot goes: during a fast movement\n"
        "        the game's aim trails the hand by a frame or two, and the crosshair shows\n"
        "        where it actually IS rather than where the hand is pointing.\n"
        "stick - the right thumbstick moves the aim, inside a thirty degree cone in which\n"
        "        the world holds still. Built first, from a misreading of \"joystick\".\n"
        "off   - nothing of ours touches the mouse.\n"
        "\n"
        "Both driven methods work by sending mouse counts, so they need to know what a\n"
        "count is worth in degrees - the player's own sensitivity, which also changes when\n"
        "a weapon is scoped. That is measured from ordinary aiming rather than configured;\n"
        "until it has converged, aiming falls back to counts a second and the aim-to-look\n"
        "click refuses itself.\n"
        "\n"
        "Current: %s, a count is worth %.5f degrees%s\n",
        kAimHand == g_AimMethod ? "hand" : (kAimStick == g_AimMethod ? "stick" : "off"),
        EffectiveGain(g_GainYaw, kSeedGainYaw),
        g_GainYaw.Converged() ? " (measured)" : " (not measured yet)");
}

CON_COMMAND(mirv_vr_pointer, "cs2-vr-spectator: the controller ray that points at the screen.")

{
    int argc = args->ArgC();
    if (2 <= argc) {
        const char * arg1 = args->ArgV(1);
        if (!_stricmp(arg1, "post") || !_stricmp(arg1, "send")) {
            g_PointerInput = (0 == _stricmp(arg1, "post")) ? kPointerPost : kPointerSend;
            advancedfx::Message("mirv_vr_pointer: %s\n",
                kPointerPost == g_PointerInput
                    ? "messages posted to the game's window. Nothing global moves."
                    : "the real system cursor. The game has to be in front.");
            return;
        }
        if (!_stricmp(arg1, "size") && 3 <= argc) {
            g_CursorSizeMetres = (float)atof(args->ArgV(2));
            if (g_CursorSizeMetres < 0.002f) g_CursorSizeMetres = 0.002f;
            if (g_CursorSizeMetres > 0.2f)   g_CursorSizeMetres = 0.2f;
            advancedfx::Message("mirv_vr_pointer: dot %.0f mm.\n", g_CursorSizeMetres * 1000.0f);
            return;
        }
    }

    CursorSnapshot cursor;
    {
        std::lock_guard<std::mutex> lock(g_ViewMutex);
        cursor = g_Cursor;
    }
    advancedfx::Message(
        "mirv_vr_pointer post|send  - how a hit becomes a mouse.\n"
        "mirv_vr_pointer size <m>   - how big the dot is. Now %.0f mm.\n"
        "\n"
        "post: WM_MOUSEMOVE and the button messages straight to the game's window. Nothing\n"
        "      global moves, no other application's cursor can be touched, and the game\n"
        "      does not have to be in front - which it often is not, because the window\n"
        "      that launched it is. It can also address a row the real cursor cannot: the\n"
        "      coordinates are packed into lParam, not clamped to a screen.\n"
        "send: the real system cursor, for anything reading GetCursorPos or raw input.\n"
        "      Refuses unless the game is the foreground window.\n"
        "\n"
        "Aim with either hand; that hand's trigger clicks. The right hand wins if both are\n"
        "on the screen, because a cursor flickering between two resting hands is unusable.\n"
        "Currently: %s, pointing at %s\n",
        g_CursorSizeMetres * 1000.0f,
        kPointerPost == g_PointerInput ? "post" : "send",
        cursor.hit ? "the screen" : "nothing");
}

CON_COMMAND(mirv_vr_menu, "cs2-vr-spectator: the game's own window on one screen, when there is no demo to show.")

{
    int argc = args->ArgC();
    if (2 <= argc) {
        const char * arg1 = args->ArgV(1);
        if (!_stricmp(arg1, "place")) {
            g_MenuPlaced = false;
            advancedfx::Message("mirv_vr_menu: it will be put in front of wherever you are looking now.\n");
            return;
        }
        if (!_stricmp(arg1, "width") && 3 <= argc) {
            g_MenuWidthDegrees = (float)atof(args->ArgV(2));
            if (g_MenuWidthDegrees < 20.0f)  g_MenuWidthDegrees = 20.0f;
            if (g_MenuWidthDegrees > 120.0f) g_MenuWidthDegrees = 120.0f;
            advancedfx::Message("mirv_vr_menu: %.0f degrees wide.\n", g_MenuWidthDegrees);
            return;
        }
        if (!_stricmp(arg1, "distance") && 3 <= argc) {
            g_MenuDistanceMetres = (float)atof(args->ArgV(2));
            if (g_MenuDistanceMetres < 0.5f) g_MenuDistanceMetres = 0.5f;
            if (g_MenuDistanceMetres > 8.0f) g_MenuDistanceMetres = 8.0f;
            g_MenuPlaced = false;
            advancedfx::Message("mirv_vr_menu: %.1f m away; putting it there now.\n", g_MenuDistanceMetres);
            return;
        }
    }

    advancedfx::Message(
        "mirv_vr_menu width <deg>    - how wide the screen looks. Now %.0f.\n"
        "mirv_vr_menu distance <m>   - how far away. Now %.1f.\n"
        "mirv_vr_menu place          - put it in front of where you are looking now.\n"
        "\n"
        "There is nothing to switch on. With a session running and no demo loaded, the\n"
        "game's own window goes on one screen in front of you; load a demo and both eyes\n"
        "go back to rendering the world. A PAUSED demo is still a demo.\n"
        "\n"
        "The screen carries the WHOLE window - %ux%u - not the part of it that fits on the\n"
        "monitor. The desktop shows the top-left corner of the back buffer and nothing\n"
        "else; the headset is not cropped.\n"
        "\n"
        "Currently: %s\n",
        g_MenuWidthDegrees, g_MenuDistanceMetres,
        g_SwapchainWidth, g_SwapchainHeight,
        g_MenuMode ? "showing the menu screen" : "not in menu mode");
}

CON_COMMAND(mirv_vr_pipe, "cs2-vr-spectator: a console for a launch that has none.")

{
    if (2 <= args->ArgC()) {
        int wanted = (0 != atoi(args->ArgV(1))) ? 1 : 0;
        if (wanted != (MirvVrXrInternal::PipeIsOpen() ? 1 : 0)) {
            if (wanted) MirvVrXrInternal::PipeStart(); else MirvVrXrInternal::PipeShutdown();
        }
        // And stop the once-per-frame reader from undoing this decision with AFXVR_PIPE
        // on some later frame.
        MirvVrXrInternal::PipeSetWanted(wanted);
    }

    advancedfx::Message(
        "mirv_vr_pipe 0|1 - a named pipe that takes console lines from outside the game.\n"
        "\n"
        "There is no console in a worn launch: the window is taller than the display and\n"
        "Panorama puts the input line off the bottom of it. This is the way in. One line,\n"
        "one command, dispatched on the engine thread in order with everything else.\n"
        "\n"
        "    scripts/send-command.ps1 \"mirv_vr_panel spread 1.2\"\n"
        "\n"
        "Open to this process's own user and SYSTEM, nobody else. Local only - remote\n"
        "clients are rejected by the pipe itself. AFXVR_PIPE=0 keeps it shut from the start.\n"
        "Current value: %i\n",
        MirvVrXrInternal::PipeIsOpen() ? 1 : 0);
}

CON_COMMAND(mirv_vr_autostart, "cs2-vr-spectator: start the headset session by itself when a demo begins to play.")

{
    if (2 <= args->ArgC()) {
        g_AutoStart = (0 != atoi(args->ArgV(1))) ? 1 : 0;
        g_AutoStartDone = false;
        g_AutoStartLastTick = -1;
        // Re-arming means re-arming both signals. Leaving the frame count where it was
        // would make a hand re-arm fire on the very next frame, which is the one case
        // where the person doing it has not had time to put the headset on.
        g_AutoStartFrames = 0;
    }

    advancedfx::Message(
        "mirv_vr_autostart 0|1 - whether the session starts without F9 being pressed.\n"
        "\n"
        "Armed from the environment by AFXVR_AUTOSTART=1, which is how a launcher will do it;\n"
        "this command overrides that from a config.\n"
        "\n"
        "With a demo it waits for it to be PLAYING, not merely loaded: the view struct has to\n"
        "read like a camera and the demo tick has to have moved. Started any earlier the\n"
        "viewer gets a frozen first frame.\n"
        "\n"
        "With no demo it starts a few seconds in anyway, and the headset shows CS2's own menu\n"
        "on a quad with a controller ray. That is not a fallback, it is the only way in: a\n"
        "worn launch has no reachable console to type this into.\n"
        "\n"
        "It fires once. If the session will not start it says so and stops trying, because\n"
        "the person it would be complaining to has a headset on.\n"
        "Current value: %i\n",
        g_AutoStart > 0 ? 1 : 0);
}

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
            // Let go of the back buffer. Keeping a view on it across a resize would stop
            // the swap chain resizing at all, and "off" is the moment there is no reason
            // to hold it.
            MirvVrXr_ReleasePanelClearView();
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
        if (!_stricmp(arg1, "transparent") || !_stricmp(arg1, "opaque")) {
            g_PanelTransparent = (0 == _stricmp(arg1, "transparent"));
            if (!g_PanelTransparent) MirvVrXr_ReleasePanelClearView();
            advancedfx::Message(
                "mirv_vr_panel: %s.\n",
                g_PanelTransparent
                    ? "transparent - the HUD alone, on nothing"
                    : "opaque - the whole main pass, world included");
            return;
        }

        if (!_stricmp(arg1, "sheet") || !_stricmp(arg1, "groups")) {
            g_PanelCutUp = (0 == _stricmp(arg1, "groups"));
            advancedfx::Message("mirv_vr_panel: %s.\n",
                g_PanelCutUp
                    ? "cut into groups, each placed on its own"
                    : "one sheet, the whole window on one quad");
            return;
        }

        if (!_stricmp(arg1, "spread")) {
            if (3 <= argc) {
                // "more" and "less" rather than a number, because the operator has no
                // console: the window is taller than the display, so Panorama puts the
                // console's input line below the bottom of the screen. Everything they
                // adjust has to reach them through a key or a controller.
                if (!_stricmp(args->ArgV(2), "more")) g_PanelSpread *= 1.1f;
                else if (!_stricmp(args->ArgV(2), "less")) g_PanelSpread /= 1.1f;
                else g_PanelSpread = (float)atof(args->ArgV(2));
                if (g_PanelSpread < 0.0f) g_PanelSpread = 0.0f;
                if (g_PanelSpread > 2.0f) g_PanelSpread = 2.0f;
            }
            advancedfx::Message(
                "mirv_vr_panel: spread %.2f - every group's azimuth and elevation, times that.\n"
                "  0 stacks them all in front of you; the Quest runs out of display somewhere\n"
                "  above 1.3.\n",
                g_PanelSpread);
            return;
        }

        if (!_stricmp(arg1, "follow") || !_stricmp(arg1, "fixed")) {
            g_PanelFollow = (0 == _stricmp(arg1, "follow"));
            advancedfx::Message("mirv_vr_panel: %s\n",
                g_PanelFollow
                    ? "following you - the angles are true angles from your eyes wherever you stand."
                    : "fixed in the room at the anchor, whatever you do with your body.");
            return;
        }

        if (!_stricmp(arg1, "show") || !_stricmp(arg1, "hide")) {
            g_PanelShown = (0 == _stricmp(arg1, "show"));
            advancedfx::Message("mirv_vr_panel: %s.\n", g_PanelShown ? "shown" : "hidden");
            return;
        }

        if (!_stricmp(arg1, "layout")) {
            advancedfx::Message("mirv_vr_panel layout (%s, %s, spread %.2f):\n",
                g_PanelCutUp ? "groups" : "one sheet, so none of this is in use",
                g_PanelShown ? "shown" : "hidden",
                g_PanelSpread);
            for (int i = 0; i < kPanelRegionCount; i++) {
                const PanelRegion & r = g_PanelRegions[i];
                advancedfx::Message(
                    "  %-8s %s  u %.2f-%.2f v %.2f-%.2f   az %+6.1f  el %+6.1f  %5.1f deg wide at %.2f m\n",
                    r.name, r.enabled ? "on " : "off",
                    r.u0, r.u1, r.v0, r.v1,
                    r.azimuthDegrees, r.elevationDegrees, r.widthDegrees, r.distanceMetres);
            }
            return;
        }

        if (3 <= argc && (!_stricmp(arg1, "region") || !_stricmp(arg1, "rect"))) {
            PanelRegion * found = nullptr;
            for (int i = 0; i < kPanelRegionCount; i++) {
                if (!_stricmp(args->ArgV(2), g_PanelRegions[i].name)) { found = &g_PanelRegions[i]; break; }
            }
            if (!found) {
                advancedfx::Warning("mirv_vr_panel: no group called \"%s\". mirv_vr_panel layout lists them.\n",
                    args->ArgV(2));
                return;
            }

            if (!_stricmp(arg1, "rect")) {
                if (7 > argc) {
                    advancedfx::Warning("mirv_vr_panel rect <group> <u0> <v0> <u1> <v1>\n");
                    return;
                }
                found->u0 = (float)atof(args->ArgV(3));
                found->v0 = (float)atof(args->ArgV(4));
                found->u1 = (float)atof(args->ArgV(5));
                found->v1 = (float)atof(args->ArgV(6));
                advancedfx::Message("mirv_vr_panel: %s takes u %.3f-%.3f, v %.3f-%.3f of the sheet.\n",
                    found->name, found->u0, found->u1, found->v0, found->v1);
                return;
            }

            if (4 == argc && (!_stricmp(args->ArgV(3), "on") || !_stricmp(args->ArgV(3), "off"))) {
                found->enabled = (0 == _stricmp(args->ArgV(3), "on"));
                advancedfx::Message("mirv_vr_panel: %s %s.\n", found->name, found->enabled ? "on" : "off");
                return;
            }

            if (6 > argc) {
                advancedfx::Warning(
                    "mirv_vr_panel region <group> <azimuth> <elevation> <width-degrees> [distance]\n"
                    "mirv_vr_panel region <group> on|off\n");
                return;
            }
            found->azimuthDegrees   = (float)atof(args->ArgV(3));
            found->elevationDegrees = (float)atof(args->ArgV(4));
            found->widthDegrees     = (float)atof(args->ArgV(5));
            if (found->widthDegrees < 2.0f) found->widthDegrees = 2.0f;
            if (found->widthDegrees > 140.0f) found->widthDegrees = 140.0f;
            if (7 <= argc) {
                found->distanceMetres = (float)atof(args->ArgV(6));
                if (found->distanceMetres < 0.3f) found->distanceMetres = 0.3f;
                if (found->distanceMetres > 20.0f) found->distanceMetres = 20.0f;
            }
            advancedfx::Message("mirv_vr_panel: %s at az %+.1f el %+.1f, %.1f deg wide at %.2f m.\n",
                found->name, found->azimuthDegrees, found->elevationDegrees,
                found->widthDegrees, found->distanceMetres);
            return;
        }

        if (!_stricmp(arg1, "alpha")) {
            int frames = (3 <= argc) ? atoi(args->ArgV(2)) : 1;
            if (frames < 1) frames = 1;
            if (frames > 30) frames = 30;
            g_PanelAlphaProbe = frames;
            advancedfx::Message(
                "mirv_vr_panel: reading the alpha channel of the next %i main pass(es).\n"
                "  Needs the panel on; needs no session and no headset.\n",
                frames);
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
        "mirv_vr_panel transparent - carry the HUD alone, on nothing. The default.\n"
        "mirv_vr_panel opaque      - carry the whole main pass, world and all.\n"
        "mirv_vr_panel alpha [n]   - read back what the HUD left in the alpha channel.\n"
        "mirv_vr_panel groups      - cut the sheet up, one quad per HUD group. The default.\n"
        "mirv_vr_panel sheet       - the whole window on one quad, as it used to be.\n"
        "mirv_vr_panel layout      - print the groups and where each one hangs.\n"
        "mirv_vr_panel region <g> <az> <el> <deg> [m]  - move and size one group.\n"
        "mirv_vr_panel region <g> on|off\n"
        "mirv_vr_panel rect <g> <u0> <v0> <u1> <v1>    - which part of the sheet it is.\n"
        "mirv_vr_panel spread <k>  - how far out of the way every group sits, in one number.\n"
        "mirv_vr_panel show|hide   - without moving anything. Also the right stick click.\n"
        "mirv_vr_panel follow|fixed- whether it comes with you when you move, keeping its\n"
        "                            direction. Following is the default and is what makes\n"
        "                            \"look down and it is in my lap\" true after you sit.\n"
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
        g_PanelEnabled ? (g_PanelTransparent ? "on, transparent" : "on, opaque") : "off",
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

    // What the runtime says is only half the story. This is what the game was actually
    // told, at the point of the write - the only place that can distinguish "the setting
    // is not arriving" from "the setting arrives and does nothing".
    advancedfx::Message("\n  What the camera was actually given, per pass:\n");
    float o1[3], a1[3], f1, o2[3], a2[3], f2;
    bool have1 = AfxVr_GetLastApplied(1, o1, a1, &f1);
    bool have2 = AfxVr_GetLastApplied(2, o2, a2, &f2);

    if (have1) advancedfx::Message("    pass 1  org (%.2f %.2f %.2f)  ang (%.2f %.2f %.2f)  fov %.1f\n",
        o1[0], o1[1], o1[2], a1[0], a1[1], a1[2], f1);
    else advancedfx::Message("    pass 1  nothing written yet\n");

    if (have2) advancedfx::Message("    pass 2  org (%.2f %.2f %.2f)  ang (%.2f %.2f %.2f)  fov %.1f\n",
        o2[0], o2[1], o2[2], a2[0], a2[1], a2[2], f2);
    else advancedfx::Message("    pass 2  nothing written yet\n");

    advancedfx::Message("\n  Ask scale %.3f%s\n",
        g_AskScale,
        (g_AskScale == 1.0f) ? " (untouched)" : " - the engine is asked for that much of the claimed angle");

    // What the engine's own projection matrix says it rendered, against what we claimed.
    // These two disagreeing is what makes the world look small and far away: the runtime
    // is told the image covers more than it does, so it shrinks it to fit.
    {
        float renderedH = 0.0f, renderedV = 0.0f;
        if (AfxVr_GetRenderedFovDegrees(&renderedH, &renderedV)) {
            advancedfx::Message(
                "\n  The engine's own projection matrix, for the camera it last built one for:\n"
                "    rendered %.1f deg horizontal, %.1f deg vertical\n"
                "    (a measurement, not the 4:3 model - if these disagree with what we claim,\n"
                "     the world looks the wrong size)\n",
                renderedH, renderedV);
        } else {
            advancedfx::Message("\n  The engine's projection matrix has not been seen yet.\n");
        }
    }

    if (have1 && have2) {
        float dx = o2[0] - o1[0], dy = o2[1] - o1[1], dz = o2[2] - o1[2];
        float separation = sqrtf(dx * dx + dy * dy + dz * dz);
        advancedfx::Message(
            "    the two cameras are %.3f units apart (%.1f mm), and %.2f degrees of yaw\n"
            "    %s\n",
            separation, separation * 25.4f, a2[1] - a1[1],
            separation < 0.01f
                ? "    ZERO - the eye offsets are not reaching the render at all"
                : "");
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

CON_COMMAND(mirv_vr_triggers, "cs2-vr-spectator: what the controller triggers do - tune the field of view, or seek.")
{
    if (2 <= args->ArgC()) {
        if (!_stricmp(args->ArgV(1), "players")) g_TriggerMode = kTriggersPlayers;
        else if (!_stricmp(args->ArgV(1), "seek")) g_TriggerMode = kTriggersSeek;
        else if (!_stricmp(args->ArgV(1), "fov")) g_TriggerMode = kTriggersFov;
        else {
            advancedfx::Warning("mirv_vr_triggers players|seek|fov\n");
            return;
        }
        advancedfx::Message("mirv_vr_triggers: %s\n",
            (kTriggersPlayers == g_TriggerMode) ? "right is the next player, left the previous."
          : (kTriggersSeek    == g_TriggerMode) ? "right seeks forward, left seeks back."
                                                : "right widens the view, left narrows it. Hold to repeat.");
        return;
    }

    advancedfx::Message(
        "mirv_vr_triggers players|seek|fov - what the two triggers are for.\n"
        "\n"
        "players: right is the next player, left the previous. The default, because it is\n"
        "      the most frequent thing a spectator does and it belongs under the index\n"
        "      fingers. X and Y seek instead.\n"
        "\n"
        "seek: right forward, left back. HOME and END also do it from the keyboard.\n"
        "\n"
        "fov:  right widens what the engine is asked to render, left narrows it, one per\n"
        "      cent a step, held to repeat. What the runtime is TOLD does not change, so\n"
        "      this is the one dial that is not invisible: it changes how much world goes\n"
        "      into the image while the frustum it is shown in stays honest.\n"
        "\n"
        "      A measuring instrument, not a preference. Look at a far corner, turn and\n"
        "      nod, and stop when the corner stays nailed to the world; ignore how big\n"
        "      things look. It is how the convention was settled in the first place, and\n"
        "      the right way to re-check it after any change to the crop or the window.\n"
        "\n"
        "Current: %s, ask scale %.3f.\n",
        (kTriggersPlayers == g_TriggerMode) ? "players"
      : (kTriggersSeek    == g_TriggerMode) ? "seek" : "fov",
        g_AskScale);
}

CON_COMMAND(mirv_vr_askscale, "cs2-vr-spectator: the multiplier on the angle handed to the engine.")
{
    if (2 <= args->ArgC()) {
        g_AskScale = (float)atof(args->ArgV(1));
        if (g_AskScale < 0.5f) g_AskScale = 0.5f;
        if (g_AskScale > 1.2f) g_AskScale = 1.2f;
    }
    advancedfx::Message("mirv_vr_askscale: %.3f\n", g_AskScale);
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

CON_COMMAND(mirv_vr_centre, "cs2-vr-spectator: render each eye around the real centre of its frustum.")
{
    if (2 <= args->ArgC()) {
        g_CentreFrustum = 0 != atoi(args->ArgV(1));
        advancedfx::Message("mirv_vr_centre: %s\n", g_CentreFrustum ? "on" : "off");
        return;
    }

    advancedfx::Message(
        "mirv_vr_centre 0|1 - point each eye's camera down the middle of its own frustum.\n"
        "\n"
        "A headset's frustum is not centred on the eye's forward axis, because the lens sits\n"
        "outboard of the pupil. A Quest 3 reports [-54, +40] horizontally for the left eye\n"
        "and [-40, +54] for the right: the same 94 degrees, centred 7 degrees outward on\n"
        "each side.\n"
        "\n"
        "CS2 can only render a frustum centred on its camera, so that offset has to be\n"
        "carried by turning the camera. Without it there is a constant 14 degrees of angular\n"
        "divergence between the eyes - and being angular, it does not shrink with distance.\n"
        "A near wall still fuses because real parallax swamps it; a far one does not,\n"
        "because at distance nothing else is left. Converging by eye at one distance then\n"
        "walking away makes it worse, which is what gave it away.\n"
        "\n"
        "Off restores the old behaviour, for comparison. Current: %s.\n",
        g_CentreFrustum ? "on" : "off");
}

CON_COMMAND(mirv_vr_reset, "cs2-vr-spectator: put every stereo setting back to its default, in one command.")
{
    g_IpdScale = 1.0f;
    g_Monoscopic = false;
    g_SwapEyes = false;
    g_CentreFrustum = 0;      // off: see the note where it is declared
    g_RollMode = 1;
    g_FovOverrideDegrees = 0.0f;
    g_FovScale = 1.0f;
    g_FovVerticalOverrideDegrees = 0.0f;
    g_ReportedFovOverrideDegrees = 0.0f;
    g_SourceAspectFix = false; // off: measured, experiment 18. The per-pass field is already
                               // aspect-scaled by the engine; applying the 4:3 chain scales it twice.
    g_Calibrating = false;

    advancedfx::Message(
        "mirv_vr_reset: separation x1, stereo, eyes in runtime order, frustum centring OFF,\n"
        "  roll as reported, no field-of-view overrides, calibration off.\n"
        "  Everything is now at its default, which is the only state worth comparing from.\n");
}

CON_COMMAND(mirv_vr_crop, "cs2-vr-spectator: submit the runtime's own frustum with a cropped image rectangle.")
{
    if (2 <= args->ArgC()) {
        g_CropToRuntimeFov = 0 != atoi(args->ArgV(1));
        advancedfx::Message("mirv_vr_crop: %s\n", g_CropToRuntimeFov ? "on" : "off");
        return;
    }

    advancedfx::Message(
        "mirv_vr_crop 0|1 - hand the runtime its own frustum and point it at the part of\n"
        "our symmetric image that frustum covers.\n"
        "\n"
        "Claiming a field of view only works on a runtime that reads the claim.\n"
        "XrViewConfigurationProperties::fovMutable says whether it does. Measured here:\n"
        "  Oculus PC   FALSE - composites with its own frustum, ignores ours\n"
        "  SteamVR     TRUE  - honours ours\n"
        "This runtime: %s.\n"
        "\n"
        "When it is ignored, a symmetric image gets stretched onto an asymmetric frustum.\n"
        "The stretch is linear in tangent space, so for a Quest 3 the centre ray lands\n"
        "about 15 degrees out, mirrored between the eyes - thirty degrees of divergence\n"
        "that does not shrink with distance. Only something a few centimetres from your\n"
        "face can be fused through it.\n"
        "\n"
        "With this on, the two cases become identical: render symmetric as before, submit\n"
        "the runtime's frustum unchanged, and crop the image rectangle to match. Meta\n"
        "documents exactly this and calls it symmetric projection.\n"
        "\n"
        "Current: %s. Last rectangles: eye 0 %dx%d at (%d,%d), eye 1 %dx%d at (%d,%d).\n",
        g_FovMutableKnown ? (g_FovMutable ? "fovMutable TRUE" : "fovMutable FALSE") : "not queried yet",
        g_CropToRuntimeFov ? "on" : "off",
        g_CropValid[0] ? g_CropRect[0].extent.width : 0, g_CropValid[0] ? g_CropRect[0].extent.height : 0,
        g_CropValid[0] ? g_CropRect[0].offset.x : 0, g_CropValid[0] ? g_CropRect[0].offset.y : 0,
        g_CropValid[1] ? g_CropRect[1].extent.width : 0, g_CropValid[1] ? g_CropRect[1].extent.height : 0,
        g_CropValid[1] ? g_CropRect[1].offset.x : 0, g_CropValid[1] ? g_CropRect[1].offset.y : 0);
}
