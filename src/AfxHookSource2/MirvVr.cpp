#include "stdafx.h"

#include "MirvVr.h"
#include "MirvVrMath.h"

#include "WrpConsole.h"

#include "../shared/AfxConsole.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// For GetModuleFileNameA: the game's own steam.inf is found relative to the running
// executable, which is the only way to learn the build number from in here.
#include <windows.h>

// Field offsets into the view struct the trampoline receives, which is CViewRender+0x10
// for CS2 build 2000908. These move on game updates - see docs/05-view-setup-point.md.
//
// When they do move, nothing fails to load and nothing complains: the hook writes floats
// into whatever now lives at those addresses. Everything below the offsets exists to make
// that loud instead of silent - the build number is compared against the one they were
// measured on, and every write is gated on the values reading back like a camera.
#define AFXVR_OFS_FOV     0x498
// The float immediately after the view fov is the weapon model's own. The gun is drawn
// with its own frustum, so leaving this at the game's ~68 while the view renders at 108
// projects the weapon about twice oversized, at the wrong stereo depth, and it swims
// against the world whenever the head turns.
#define AFXVR_OFS_WEAPONFOV 0x49c
#define AFXVR_OFS_ORIGIN  0x4a0
#define AFXVR_OFS_ANGLES  0x4b8

// The CS2 ClientVersion the offsets above were measured against.
#define AFXVR_TESTED_CLIENT_VERSION "2000908"

int g_AfxVrLogUntilFrame = 0;

namespace {

struct Eye {
    bool enabled = false;
    float right = 0.0f, forward = 0.0f, up = 0.0f;
    float dPitch = 0.0f, dYaw = 0.0f, dRoll = 0.0f;
    float fov = 0.0f; // 0 = keep the game's
};

// Index by pass: 0 is the main pass and is deliberately left alone, 1 and 2 are the eyes,
// 3 is the pass loop's spare.
Eye g_Eyes[4];

// The last values actually written into the view struct, per pass, for reporting.
float g_AppliedOrigin[4][3] = {};
float g_AppliedAngles[4][3] = {};
float g_AppliedFov[4] = {};
bool g_Applied[4] = {};

void * g_ViewStruct = nullptr;
float g_BaseOrigin[3] = { 0.0f, 0.0f, 0.0f };
float g_BaseAngles[3] = { 0.0f, 0.0f, 0.0f };
float g_BaseFov = 90.0f;
bool g_Dirty = false;

// The last thing this module wrote into the view struct, whatever wrote it. Compared with
// what is in there at the top of the next frame to tell "nobody has touched it since"
// from "the engine has computed a new camera" - see ShouldRestoreBaseView in MirvVrMath.h,
// and the frozen camera it exists to prevent.
AfxVrMath::ViewTriple g_LastWritten = {};
bool g_HaveLastWritten = false;

void RememberWritten(const float origin[3], const float angles[3], float fov) {
    for (int i = 0; i < 3; i++) {
        g_LastWritten.origin[i] = origin[i];
        g_LastWritten.angles[i] = angles[i];
    }
    g_LastWritten.fov = fov;
    g_HaveLastWritten = true;
}


// The head: one orientation, no eye offset, delivered once a frame. See AfxVr_SetHead.
struct Head {
    bool enabled = false;
    float dPitch = 0.0f, dYaw = 0.0f, dRoll = 0.0f;
    float fov = 0.0f;
    // Where the head is in the room, relative to where it was at the last recentre, in
    // OpenXR metres.
    float roomX = 0.0f, roomY = 0.0f, roomZ = 0.0f;
};
Head g_Head;

bool g_RoomScale = true;

// Passes still owed a report of what was in the fov field when they began, from
// mirv_vr_fovraw.
//
// The number handed to the engine at the view-setup trampoline is in Source's 4:3
// convention - 90 means 90 at 4:3, and the engine widens it for the window's aspect
// afterwards. What is not known is whether that widening happens in place, in the same
// field, before the passes run. If it does, the per-pass write is in the wrong convention
// and the eyes render an angle the crop does not expect - which makes the world the wrong
// size and leaves a residue on every head turn that no reprojection can remove.
//
// One printed float settles it. 90 means the field is untouched since the trampoline.
int g_RawFovProbe = 0;

// What was in the fov field when this frame's passes began, before anything of ours was
// written over it.
//
// A pass with no eye assigned has to put back what the engine intended, and g_BaseFov is
// not that: it is the number the trampoline read, in Source's 4:3 convention, and by pass
// time the engine has rescaled the field in place for the window's aspect (experiment 18).
// Writing the trampoline's number into the pass's field narrows the main pass by the
// aspect ratio - on this portrait buffer, visibly.
//
// So the field's own value is kept and handed back, which needs no convention at all.
// What to do with the main pass, when its world is going to be thrown away anyway.
//
// Three scene traversals cost about 11 ms of a 29 ms frame, and with the HUD panel on, the
// world the MAIN pass renders is wiped to transparent black before the UI is composited -
// so a third of the traversal budget is spent producing an image that is erased a moment
// later. Pass 0 cannot be skipped; the engine needs it. But it can be made to see almost
// nothing, and frustum culling then throws the map away for that pass.
//
//   off   what the engine intended.
//   tiny  a two-degree frustum, still pointing where the head points. The safer of the
//         two: anything the engine fits to pass 0's view - shadow cascades, probe choice,
//         streaming priorities - stays pointed the right way, just narrow.
//   up    the engine's frustum, aimed at the sky. Culls about as well and leaves the
//         frustum shape alone, at the cost of pointing it somewhere the head is not.
//
// Off until measured. The thing to watch is whether the EYE images change at all between
// the three: if they do, something per-pass leaks from the main pass into them, and the
// saving is not free.
enum Pass0Mode { kPass0Normal = 0, kPass0Tiny = 1, kPass0Up = 2 };
int g_Pass0Mode = kPass0Normal;

float g_PassEntryFov = 0.0f;

bool g_HavePassEntryFov = false;


// Mirrors mirv_vr_ipd's scale, so a lean has the same world scale as the eye separation.
float g_IpdScaleForRoom = 1.0f;

bool g_FreeLook = false;

// How much of the demo camera's own orientation the headset sits on top of, when free
// look is off.
//
// "Level" takes the yaw only. Composing a pure yaw with the head is the one case where
// the horizon cannot move: the result is the head's own pitch and roll with a constant
// added to its yaw, so what is rendered is exactly what the pose handed to the compositor
// says was rendered, and reprojection during a turn is exact.
//
// "Full" takes the demo camera's pitch and roll as well, composed properly. It is
// geometrically correct and physically unpleasant - spectating a player who looks down
// pitches the viewer's world - so it is not the default. It exists because the question
// "what does the demo camera actually see" has to stay answerable.
//
// What is gone is the third option, which was never a choice: adding the two sets of
// Euler angles together. See ComposeSourceAngles in MirvVrMath.h.
bool g_HorizonLevel = true;

float g_YawOffset = 0.0f;   // added to the head's yaw so the room lines up with the map
float g_LastHeadYaw = 0.0f; // whatever the headset last reported, for Recenter

// Where the viewer has moved to, relative to the demo camera. World space, units.
float g_MoveOffset[3] = { 0.0f, 0.0f, 0.0f };
// The yaw the last frame was actually composed with, so a stick move goes where the
// viewer is looking rather than where the map's X axis points.
float g_LastViewYaw = 0.0f;
float g_LastViewPitch = 0.0f;

bool AnyEyeEnabled() {
    for (int i = 1; i < 4; i++) if (g_Eyes[i].enabled) return true;
    return false;
}

// See the note in MirvVr.h. The view struct we hold is CViewRender+0x10; the matrix
// builder wants CViewRender itself.
AfxVr_MakeMatrix_t g_MakeMatrix = nullptr;
bool g_HudFix = false;

// Project the weapon model with the same frustum as the world.
//
// OFF, and off because it was tried. "The float after the view fov is the weapon fov" is
// read from HLAE's own dead override code and was never measured by this project; I turned
// it on by default anyway. The first game with it on went black at seven frames a second
// the moment a team was picked. Whatever +0x49c is, writing 108 into it every pass is not
// what it wants.
//
// Kept as a switch rather than deleted, because the problem it was meant to solve is real:
// the gun IS drawn with its own frustum, and at the world's fov it will be the wrong size.
// The next step is measuring what that float does - one pass, one value, look - not
// guessing again.
bool g_WeaponFov = false;
bool g_WarnedNoMakeMatrix = false;

void RebuildViewMatrices() {
    if (!g_HudFix || nullptr == g_ViewStruct) return;
    if (nullptr == g_MakeMatrix) {
        if (!g_WarnedNoMakeMatrix) {
            g_WarnedNoMakeMatrix = true;
            advancedfx::Warning(
                "AFXVR: mirv_vr_remakematrix has nothing to call - the matrix builder was never\n"
                "AFXVR: hooked. This build of the hook cannot do it.\n");
        }
        return;
    }
    g_MakeMatrix((unsigned char*)g_ViewStruct - 0x10);
}

// --- does the view struct still look like a view struct? ---------------------------

// Latched so the log does not fill with the same sentence sixty times a second, and so
// the first transition in either direction is reported exactly once.
bool g_ViewPlausible = true;
unsigned int g_PlausibleViews = 0;

bool g_ReportedImplausible = false;
bool g_ReportedRecovered = false;
const char * g_LastImplausibleReason = nullptr;

// What steam.inf says, read once. Empty when it could not be read at all, which is not
// itself a problem - a missing file says nothing about the offsets.
char g_GameClientVersion[64] = "";
bool g_CheckedGameBuild = false;
bool g_GameBuildMatches = false;

bool ReadWholeFile(const char * path, char * out, size_t outSize) {
    FILE * f = nullptr;
    if (0 != fopen_s(&f, path, "rb") || !f) return false;
    size_t n = fread(out, 1, outSize - 1, f);
    fclose(f);
    out[n] = '\0';
    return true;
}

void CheckGameBuildOnce() {
    if (g_CheckedGameBuild) return;
    g_CheckedGameBuild = true;

    char exePath[MAX_PATH] = "";
    if (0 == GetModuleFileNameA(NULL, exePath, sizeof(exePath))) return;

    char infPath[MAX_PATH] = "";
    if (!AfxVrMath::SteamInfPathFromExe(exePath, infPath, sizeof(infPath))) return;

    char text[4096];
    if (!ReadWholeFile(infPath, text, sizeof(text))) return;

    if (!AfxVrMath::SteamInfValue(text, "ClientVersion", g_GameClientVersion, sizeof(g_GameClientVersion))) {
        g_GameClientVersion[0] = '\0';
        return;
    }

    g_GameBuildMatches = (0 == strcmp(g_GameClientVersion, AFXVR_TESTED_CLIENT_VERSION));
    if (g_GameBuildMatches) {
        advancedfx::Message("AFXVR: CS2 build %s, the one the view offsets were measured on.\n",
            g_GameClientVersion);
    } else {
        advancedfx::Warning(
            "AFXVR: CS2 build %s, but the view field offsets were measured on %s.\n"
            "AFXVR: They may have moved. If the camera behaves oddly or the game crashes on\n"
            "AFXVR: entering VR, that is the first thing to suspect. Re-measuring is described\n"
            "AFXVR: in docs/05-view-setup-point.md; mirv_vr_selftest reports what is read back.\n",
            g_GameClientVersion, AFXVR_TESTED_CLIENT_VERSION);
    }
}

// Every write into the view struct goes through this. The offsets are a guess about
// another program's memory layout, and the moment the values stop looking like a camera
// the honest thing to do is stop writing rather than corrupt whatever is there now.
bool ViewIsPlausible() {
    AfxVrMath::ViewCheck r = AfxVrMath::CheckView(g_BaseOrigin, g_BaseAngles, g_BaseFov);

    if (!r.ok) {
        g_ViewPlausible = false;
        g_LastImplausibleReason = r.why;
        if (!g_ReportedImplausible) {
            g_ReportedImplausible = true;
            g_ReportedRecovered = false;
            advancedfx::Warning(
                "AFXVR: refusing to write the eye pose: %s.\n"
                "AFXVR: read back origin=(%f,%f,%f) angles=(%f,%f,%f) fov=%f\n"
                "AFXVR: This is what a game update looks like from in here. The offsets in\n"
                "AFXVR: MirvVr.cpp are for CS2 build %s; this game is build %s.\n",
                r.why,
                g_BaseOrigin[0], g_BaseOrigin[1], g_BaseOrigin[2],
                g_BaseAngles[0], g_BaseAngles[1], g_BaseAngles[2], g_BaseFov,
                AFXVR_TESTED_CLIENT_VERSION,
                g_GameClientVersion[0] ? g_GameClientVersion : "unknown");
        }
        return false;
    }

    g_ViewPlausible = true;
    if (g_ReportedImplausible && !g_ReportedRecovered) {
        g_ReportedRecovered = true;
        g_ReportedImplausible = false;
        advancedfx::Message("AFXVR: the view struct reads like a camera again; writing resumed.\n");
    }
    return true;
}

// The one place the viewer's orientation is built, so the head written once a frame and
// the eyes written once a pass cannot drift apart.
//
// Composition, not addition. Adding two sets of Euler angles is the same rotation only
// while the base camera is level; see ComposeSourceAngles in MirvVrMath.h for what it
// looks like when it is not.
void ComposeViewAngles(float dPitch, float dYaw, float dRoll, float out[3]) {
    if (g_FreeLook) {
        // Position from the demo, orientation from the headset alone.
        out[0] = dPitch;
        out[1] = dYaw + g_YawOffset;
        out[2] = dRoll;
        return;
    }

    // The yaw offset applies here too, or turning with the stick would silently do
    // nothing whenever the demo camera owns the orientation.
    const float base[3] = {
        g_HorizonLevel ? 0.0f : g_BaseAngles[0],
        g_BaseAngles[1],
        g_HorizonLevel ? 0.0f : g_BaseAngles[2],
    };
    const float head[3] = { dPitch, dYaw + g_YawOffset, dRoll };
    AfxVrMath::ComposeSourceAngles(base, head, out);
}

// Which world direction "forward in the room" points, which is the view yaw with the
// head's own yaw taken back out. Read from the same place ComposeViewAngles reads it, so
// leaning and turning cannot end up disagreeing about where forward is.
float RoomYawDegrees() {
    return (g_FreeLook ? 0.0f : g_BaseAngles[1]) + g_YawOffset;
}

// The head's position in the room, as an offset in the map.
//
// Scaled by the same factor as the eye separation: world scale is one number, and eyes
// that are scaled while the head is not give a lean the wrong parallax for the stereo the
// viewer is being shown.
void RoomOffsetWorld(float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!g_RoomScale || !g_Head.enabled) return;

    AfxVrMath::RoomOffsetToWorld(RoomYawDegrees(),
        g_Head.roomX * g_IpdScaleForRoom, g_Head.roomY * g_IpdScaleForRoom,
        g_Head.roomZ * g_IpdScaleForRoom, out);
}

} // namespace

void AfxVr_BeforeViewSetupRead(void * pViewStruct) {
    if (!g_Dirty || pViewStruct != g_ViewStruct) return;

    float * pOrigin = (float*)((unsigned char*)pViewStruct + AFXVR_OFS_ORIGIN);
    float * pAngles = (float*)((unsigned char*)pViewStruct + AFXVR_OFS_ANGLES);
    float * pFov    = (float*)((unsigned char*)pViewStruct + AFXVR_OFS_FOV);

    AfxVrMath::ViewTriple now;
    for (int i = 0; i < 3; i++) { now.origin[i] = pOrigin[i]; now.angles[i] = pAngles[i]; }
    now.fov = *pFov;

    // Only if nobody else has been here since. On a playing demo the engine writes a fresh
    // camera into this struct every frame, and putting the base back over it froze the
    // viewer at the point the session started - for the whole session.
    if (AfxVrMath::ShouldRestoreBaseView(g_Dirty, g_HaveLastWritten, g_LastWritten, now)) {
        pOrigin[0] = g_BaseOrigin[0]; pOrigin[1] = g_BaseOrigin[1]; pOrigin[2] = g_BaseOrigin[2];
        pAngles[0] = g_BaseAngles[0]; pAngles[1] = g_BaseAngles[1]; pAngles[2] = g_BaseAngles[2];
        *pFov = g_BaseFov;
        RememberWritten(g_BaseOrigin, g_BaseAngles, g_BaseFov);
    }

    g_Dirty = false;
}

void AfxVr_SetHead(bool enabled, float dPitch, float dYaw, float dRoll, float fov,
                   float roomX, float roomY, float roomZ) {
    g_Head.enabled = enabled;
    g_Head.dPitch = dPitch;
    g_Head.dYaw = dYaw;
    g_Head.dRoll = dRoll;
    g_Head.fov = fov;
    g_Head.roomX = roomX;
    g_Head.roomY = roomY;
    g_Head.roomZ = roomZ;
}

void AfxVr_SetRoomScale(bool enabled) { g_RoomScale = enabled; }
bool AfxVr_GetRoomScale() { return g_RoomScale; }

unsigned int AfxVr_PlausibleViewCount() { return g_PlausibleViews; }


void AfxVr_SetRoomIpdScale(float scale) { g_IpdScaleForRoom = scale; }

bool AfxVr_AfterViewSetup(void * pViewStruct, float & tx, float & ty, float & tz,
                          float & rx, float & ry, float & rz, float & fov) {
    g_ViewStruct = pViewStruct;
    g_BaseOrigin[0] = tx; g_BaseOrigin[1] = ty; g_BaseOrigin[2] = tz;
    g_BaseAngles[0] = rx; g_BaseAngles[1] = ry; g_BaseAngles[2] = rz;
    g_BaseFov = fov;

    // Cheap, and the first frame is the right moment: the game is fully up by the time a
    // view is being set up, and the answer is wanted before anything is written.
    CheckGameBuildOnce();

    // Counted here and not in ViewIsPlausible, which only runs once something is enabled.
    // Whoever is deciding whether the game is far enough along to put a headset on needs
    // the answer before that.
    if (AfxVrMath::CheckView(g_BaseOrigin, g_BaseAngles, g_BaseFov).ok) g_PlausibleViews++;


    if (g_AfxVrFrameIndex < g_AfxVrLogUntilFrame) {
        advancedfx::Message(
            "AFXVR: frame=%i pass=%i SetupView this=%p org=(%f,%f,%f) ang=(%f,%f,%f) fov=%f\n",
            g_AfxVrFrameIndex, g_AfxVrPassIndex, pViewStruct, tx, ty, tz, rx, ry, rz, fov);
    }

    // The head, once a frame, for everything that is not the image.
    if (!g_Head.enabled || !AnyEyeEnabled()) return false;
    if (!ViewIsPlausible()) return false;

    float angles[3];
    ComposeViewAngles(g_Head.dPitch, g_Head.dYaw, g_Head.dRoll, angles);

    // The stick offset is already in world space; the head adds no offset of its own,
    // which is the difference between this and an eye.
    float room[3];
    RoomOffsetWorld(room);

    tx = g_BaseOrigin[0] + g_MoveOffset[0] + room[0];
    ty = g_BaseOrigin[1] + g_MoveOffset[1] + room[1];
    tz = g_BaseOrigin[2] + g_MoveOffset[2] + room[2];

    rx = angles[0];
    ry = angles[1];
    rz = angles[2];

    // Deliberately NOT the field of view. The head is written here for the consumers that
    // read the camera once a frame - the audio listener, the world-to-screen matrix,
    // culling - and none of them wants an eye's frustum. Writing it made the picture worse
    // the moment it shipped, which says something the passes alone never revealed: the
    // engine derives something from this number once a frame that reaches the eye image.
    // Until a desk measurement says what, this leaves it alone.
    (void)g_Head.fov;

    // The same flag the passes set, and for the same reason: the next frame's
    // AfxVr_BeforeViewSetupRead has to put the base camera back before the engine reads
    // the struct, or the head is added to itself forever.
    g_Dirty = true;
    g_LastHeadYaw = g_Head.dYaw;
    g_LastViewYaw = angles[1];
    g_LastViewPitch = angles[0];

    // The trampoline writes these back into the struct on our behalf, so they are what
    // will be in there - record them as ours.
    {
        const float origin[3] = { tx, ty, tz };
        const float written[3] = { rx, ry, rz };
        RememberWritten(origin, written, fov);
    }
    return true;
}

void AfxVr_OnBeginRenderPass(int passIndex) {
    if (nullptr == g_ViewStruct) return;
    if (passIndex < 0 || passIndex > 3) return;

    // What the engine left in the fov field for this frame's passes. Captured before
    // anything of ours is written over it, and handed back to any pass that has no eye.
    if (0 == passIndex) {
        float raw = *(float*)((unsigned char*)g_ViewStruct + AFXVR_OFS_FOV);
        if (raw > 1.0f && raw < 179.0f) {
            g_PassEntryFov = raw;
            g_HavePassEntryFov = true;
        }
    }

    // Before anything is written, and before the early returns, because the whole point is
    // to see what the engine left there.
    if (0 < g_RawFovProbe && 0 == passIndex) {
        g_RawFovProbe--;
        float raw = *(float*)((unsigned char*)g_ViewStruct + AFXVR_OFS_FOV);
        advancedfx::Message(
            "AFXVR: pass 0 entry fov field = %.4f, the trampoline was handed %.4f.\n"
            "  Equal means the field is still in Source's 4:3 convention at pass time and\n"
            "  the per-pass write is right. Different means the engine rescaled it in place\n"
            "  for the window's aspect, and the passes must write the scaled number.\n",
            raw, g_BaseFov);
    }

    if (!AnyEyeEnabled()) return;

    // Gate on what came back out of the struct rather than on a version number: a build
    // can change without moving these fields, and these fields can move without the tool
    // knowing which build it is. What matters is whether the last read looked like a
    // camera.
    if (!ViewIsPlausible()) return;

    const Eye & eye = g_Eyes[passIndex];

    float * pOrigin = (float*)((unsigned char*)g_ViewStruct + AFXVR_OFS_ORIGIN);
    float * pAngles = (float*)((unsigned char*)g_ViewStruct + AFXVR_OFS_ANGLES);
    float * pFov    = (float*)((unsigned char*)g_ViewStruct + AFXVR_OFS_FOV);

    if (!eye.enabled) {
        // A pass with no eye gets the base camera back, so the main pass is not left
        // rendering with the previous frame's last eye offset.
        pOrigin[0] = g_BaseOrigin[0]; pOrigin[1] = g_BaseOrigin[1]; pOrigin[2] = g_BaseOrigin[2];
        pAngles[0] = g_BaseAngles[0]; pAngles[1] = g_BaseAngles[1]; pAngles[2] = g_BaseAngles[2];
        // The field's own value, not the trampoline's: the two are in different
        // conventions. See g_PassEntryFov.
        *pFov = g_HavePassEntryFov ? g_PassEntryFov : g_BaseFov;

        // The main pass, when its output is about to be erased anyway.
        if (0 == passIndex && kPass0Normal != g_Pass0Mode) {
            if (kPass0Tiny == g_Pass0Mode) *pFov = 2.0f;
            else pAngles[0] = -89.0f;   // straight up
        }

        RememberWritten(pOrigin, pAngles, *pFov);
        return;
    }

    // The frame the eye offset is measured in has to be the same frame the final angles
    // describe, or the eyes end up separated along the wrong axis.
    float angles[3];
    ComposeViewAngles(eye.dPitch, eye.dYaw, eye.dRoll, angles);
    g_LastHeadYaw = eye.dYaw;

    g_LastViewYaw = angles[1];
    g_LastViewPitch = angles[0];

    float forward[3], right[3], up[3];
    AfxVrMath::AngleVectors(angles, forward, right, up);

    float room[3];
    RoomOffsetWorld(room);

    for (int i = 0; i < 3; i++) {
        pOrigin[i] = g_BaseOrigin[i]
            + g_MoveOffset[i]
            + room[i]
            + eye.right   * right[i]
            + eye.forward * forward[i]
            + eye.up      * up[i];
    }

    pAngles[0] = angles[0];
    pAngles[1] = angles[1];
    pAngles[2] = angles[2];

    // "No fov of its own" means the engine's, as the engine left it for this pass.
    *pFov = (0.0f < eye.fov) ? eye.fov
                             : (g_HavePassEntryFov ? g_PassEntryFov : g_BaseFov);

    // The weapon model is drawn with its own frustum, from the float right after this one.
    // Left alone it keeps the game's ~68 degrees while the world renders at 108, so the
    // gun is projected about twice oversized, sits at the wrong stereo depth, and swims
    // against the world every time the head turns. Same value, same convention - which is
    // an assumption, and the reason this is a switch rather than unconditional.
    if (g_WeaponFov) {
        float * pWeaponFov = (float*)((unsigned char*)g_ViewStruct + AFXVR_OFS_WEAPONFOV);
        *pWeaponFov = *pFov;
    }


    g_Dirty = true;

    for (int i = 0; i < 3; i++) {
        g_AppliedOrigin[passIndex][i] = pOrigin[i];
        g_AppliedAngles[passIndex][i] = pAngles[i];
    }
    g_AppliedFov[passIndex] = *pFov;
    g_Applied[passIndex] = true;
    RememberWritten(pOrigin, pAngles, *pFov);

    // The pose is in place; now let the client rebuild what it derives from it, so the
    // HUD drawn during this pass is placed for this eye.
    RebuildViewMatrices();

    if (g_AfxVrFrameIndex < g_AfxVrLogUntilFrame) {
        advancedfx::Message(
            "AFXVR: frame=%i pass=%i eye org=(%f,%f,%f) ang=(%f,%f,%f) fov=%f\n",
            g_AfxVrFrameIndex, passIndex,
            pOrigin[0], pOrigin[1], pOrigin[2],
            pAngles[0], pAngles[1], pAngles[2], *pFov);
    }
}

void AfxVr_SetEye(int passIndex, bool enabled,
                  float right, float forward, float up,
                  float dPitch, float dYaw, float dRoll,
                  float fov) {
    if (passIndex < 1 || passIndex > 3) return;
    Eye & e = g_Eyes[passIndex];
    e.enabled = enabled;
    e.right = right; e.forward = forward; e.up = up;
    e.dPitch = dPitch; e.dYaw = dYaw; e.dRoll = dRoll;
    e.fov = fov;
}

void AfxVr_SetFreeLook(bool enabled) { g_FreeLook = enabled; }

void AfxVr_SetWeaponFov(bool enabled) { g_WeaponFov = enabled; }
bool AfxVr_GetWeaponFov() { return g_WeaponFov; }

// What the engine last put in the view struct, before anything of ours went into it.
//
// In a demo this is the camera the recording chose. In a game being played it is the
// player's own aim - the thing the mouse moves - and that is what the crosshair and the
// deadzone cone are measured against. Nothing else in this project needed to read it back
// out, which is why it was not exposed until aiming existed.
void AfxVr_GetBaseAngles(float out[3]) {
    out[0] = g_BaseAngles[0];
    out[1] = g_BaseAngles[1];
    out[2] = g_BaseAngles[2];
}

// The world yaw the viewer's own forward points along: the demo's or the player's yaw,
// plus every turn they have made. The deadzone cone is measured from this and not from
// the gaze - measured from the gaze, glancing over your shoulder would drag the world.
float AfxVr_BodyYawDegrees() {
    return AfxVrMath::NormalizeDegrees((g_FreeLook ? 0.0f : g_BaseAngles[1]) + g_YawOffset);
}

float AfxVr_YawOffsetDegrees() { return g_YawOffset; }

// The yaw the last frame was actually composed with - where the eyes are pointed. Walking
// is measured against this: the game walks along ITS yaw, and the difference between the
// two is exactly how far a push on the stick would take you the wrong way.
float AfxVr_ViewYawDegrees() { return g_LastViewYaw; }
float AfxVr_ViewPitchDegrees() { return g_LastViewPitch; }

float AfxVr_BaseYawDegrees() { return g_BaseAngles[1]; }

bool AfxVr_GetFreeLook() { return g_FreeLook; }

void AfxVr_Recenter() {
    // Make the direction the head is pointing now mean the direction the demo camera is
    // pointing now.
    g_YawOffset = g_BaseAngles[1] - g_LastHeadYaw;
}

void AfxVr_AddMove(float right, float forward, float up) {
    float delta[3];
    AfxVrMath::MoveInViewPlane(g_LastViewYaw, right, forward, up, delta);
    for (int i = 0; i < 3; i++) g_MoveOffset[i] += delta[i];
}

void AfxVr_AddYaw(float degrees) {
    // Turning pivots about the VIEWER, not about the point the demo put the camera at.
    // With the head a metre to one side of that point, changing the yaw would swing them a
    // metre sideways through the map on every snap - a teleport per press, proportional to
    // how far they have leaned. Compensate by however much the room offset moves.
    float before[3], after[3];
    RoomOffsetWorld(before);
    g_YawOffset = AfxVrMath::NormalizeDegrees(g_YawOffset + degrees);
    RoomOffsetWorld(after);
    for (int i = 0; i < 3; i++) g_MoveOffset[i] += before[i] - after[i];
}

void AfxVr_ResetMove() {
    g_MoveOffset[0] = g_MoveOffset[1] = g_MoveOffset[2] = 0.0f;
    if (!g_FreeLook) g_YawOffset = 0.0f; // back to exactly what the demo camera sees
}

CON_COMMAND(mirv_vr_freelook, "cs2-vr-spectator: follow a player's position but keep the head free.")
{
    if (2 <= args->ArgC()) {
        AfxVr_SetFreeLook(0 != atoi(args->ArgV(1)));
        if (AfxVr_GetFreeLook()) AfxVr_Recenter();
        advancedfx::Message("mirv_vr_freelook: %s\n", AfxVr_GetFreeLook() ? "on" : "off");
        return;
    }

    advancedfx::Message(
        "mirv_vr_freelook 0|1 - Off: the headset's rotation is added to the demo camera's,\n"
        "  which is right for a fixed observation point. On: only the position is taken\n"
        "  from the demo and the orientation comes from the headset alone, so following a\n"
        "  player does not drag the viewer's head around with their aim.\n"
        "Current value: %s\n",
        AfxVr_GetFreeLook() ? "1" : "0");
}

CON_COMMAND(mirv_vr_pass0, "cs2-vr-spectator: render almost nothing in the main pass, whose image is thrown away.")
{
    if (2 <= args->ArgC()) {
        if (!_stricmp(args->ArgV(1), "off")) g_Pass0Mode = kPass0Normal;
        else if (!_stricmp(args->ArgV(1), "tiny")) g_Pass0Mode = kPass0Tiny;
        else if (!_stricmp(args->ArgV(1), "up")) g_Pass0Mode = kPass0Up;
        else {
            advancedfx::Warning("mirv_vr_pass0 off|tiny|up\n");
            return;
        }
    }

    advancedfx::Message(
        "mirv_vr_pass0 off|tiny|up - what the main pass renders.\n"
        "\n"
        "With the HUD panel on, the main pass's world is wiped to transparent black before\n"
        "the UI is composited, so a whole scene traversal - roughly a third of the frame's\n"
        "rendering - produces an image that is erased. The pass cannot be skipped, but it\n"
        "can be given a frustum that contains almost nothing.\n"
        "\n"
        "  tiny  two degrees, still pointing where the head points.\n"
        "  up    the normal frustum, aimed at the sky.\n"
        "\n"
        "ONLY meaningful while the panel is wiping that world. With the panel off or opaque\n"
        "this is simply a broken monitor image and a broken panel.\n"
        "\n"
        "Current: %s\n",
        (kPass0Normal == g_Pass0Mode) ? "off" : (kPass0Tiny == g_Pass0Mode) ? "tiny" : "up");
}

CON_COMMAND(mirv_vr_fovraw, "cs2-vr-spectator: what is in the fov field when a pass begins, before we write to it.")
{
    int n = (2 <= args->ArgC()) ? atoi(args->ArgV(1)) : 1;
    if (n < 1) n = 1;
    if (n > 30) n = 30;
    g_RawFovProbe = n;
    advancedfx::Message("mirv_vr_fovraw: reporting the next %i main pass(es).\n", n);
}

CON_COMMAND(mirv_vr_roomscale, "cs2-vr-spectator: does a step in the room move you in the map?")
{
    if (2 <= args->ArgC()) AfxVr_SetRoomScale(0 != atoi(args->ArgV(1)));

    advancedfx::Message(
        "mirv_vr_roomscale 0|1 - whether leaning, crouching and stepping move the camera.\n"
        "\n"
        "On, the world stops being glued to your face: lean round a corner and the corner\n"
        "stays put, crouch and the floor comes up. It is also the honest thing to do, since\n"
        "the projection layer reports your real eye positions to the runtime either way -\n"
        "with this off it was describing a translation that had not been rendered.\n"
        "\n"
        "Measured from wherever you were when you last recentred.\n"
        "Current value: %s\n",
        AfxVr_GetRoomScale() ? "1" : "0");
}

CON_COMMAND(mirv_vr_weaponfov, "cs2-vr-spectator: project the weapon model with the world's frustum.")
{
    if (2 <= args->ArgC()) AfxVr_SetWeaponFov(0 != atoi(args->ArgV(1)));

    advancedfx::Message(
        "mirv_vr_weaponfov 0|1 - write the eye's field of view into the weapon model's too.\n"
        "\n"
        "OFF, and off because it was tried. The gun is drawn with its own frustum, from the\n"
        "float immediately after the view's - so at the game's ~68 degrees against a world\n"
        "rendered at 108 it is about twice oversized, at the wrong stereo depth, and it\n"
        "swims against the world when the head turns. That much is real.\n"
        "\n"
        "But that the float at +0x49c IS the weapon's fov comes from HLAE's own dead\n"
        "override code and has never been measured here. Turned on, the first game went\n"
        "black at seven frames a second the moment a team was picked.\n"
        "\n"
        "So: a switch, for measuring with, not a setting. One pass, one value, look.\n"
        "Current value: %i\n",
        AfxVr_GetWeaponFov() ? 1 : 0);
}

CON_COMMAND(mirv_vr_horizon, "cs2-vr-spectator: how much of the demo camera's own tilt the headset inherits.")
{
    if (2 <= args->ArgC()) {
        if (!_stricmp(args->ArgV(1), "level")) {
            g_HorizonLevel = true;
            advancedfx::Message("mirv_vr_horizon: level - only the demo camera's yaw.\n");
            return;
        }
        if (!_stricmp(args->ArgV(1), "full")) {
            g_HorizonLevel = false;
            advancedfx::Message("mirv_vr_horizon: full - the demo camera's pitch and roll too.\n");
            return;
        }
    }

    advancedfx::Message(
        "mirv_vr_horizon level|full - with free look OFF, how much of the demo camera's\n"
        "  orientation the headset's rotation sits on top of.\n"
        "\n"
        "level: the yaw only. The horizon stays where the room's floor is, and nobody\n"
        "  else's aim can pitch your world. It is also the only arrangement in which what\n"
        "  is rendered matches the pose the compositor is told about exactly, so a turn\n"
        "  reprojects without shearing.\n"
        "full: pitch and roll as well, composed as rotations rather than added as angles.\n"
        "  Correct, and unpleasant to wear.\n"
        "\n"
        "Current value: %s\n",
        g_HorizonLevel ? "level" : "full");
}

CON_COMMAND(mirv_vr_recenter, "cs2-vr-spectator: point free look where the demo camera is facing.")
{
    AfxVr_Recenter();
    advancedfx::Message("mirv_vr_recenter: forward is now the demo camera's forward.\n");
}

CON_COMMAND(mirv_vr_eye, "cs2-vr-spectator: set one eye's pose relative to the spectator camera.")
{
    int argc = args->ArgC();

    if (2 == argc && !_stricmp(args->ArgV(1), "off")) {
        for (int i = 1; i < 4; i++) g_Eyes[i].enabled = false;
        advancedfx::Message("mirv_vr_eye: all eyes off.\n");
        return;
    }

    if (9 == argc) {
        int pass = atoi(args->ArgV(1));
        AfxVr_SetEye(pass, true,
            (float)atof(args->ArgV(2)), (float)atof(args->ArgV(3)), (float)atof(args->ArgV(4)),
            (float)atof(args->ArgV(5)), (float)atof(args->ArgV(6)), (float)atof(args->ArgV(7)),
            (float)atof(args->ArgV(8)));
        advancedfx::Message("mirv_vr_eye: pass %i set.\n", pass);
        return;
    }

    advancedfx::Message(
        "mirv_vr_eye <pass> <right> <forward> <up> <dPitch> <dYaw> <dRoll> <fov> - set an eye.\n"
        "mirv_vr_eye off - disable all eyes.\n"
        "\n"
        "Pass 1 and 2 are the eyes; pass 0 is the monitor view and is never offset.\n"
        "Offsets are in the spectator camera's own frame, in units (1 unit = 1 inch).\n"
        "Angles are added to the camera's. A fov of 0 keeps the game's.\n"
        "\n"
        "Current:\n");
    for (int i = 1; i < 4; i++) {
        const Eye & e = g_Eyes[i];
        advancedfx::Message(
            "  pass %i: %s r=%f f=%f u=%f dP=%f dY=%f dR=%f fov=%f\n",
            i, e.enabled ? "on " : "off",
            e.right, e.forward, e.up, e.dPitch, e.dYaw, e.dRoll, e.fov);
    }
}

CON_COMMAND(mirv_vr_ipd, "cs2-vr-spectator: place the two eyes symmetrically, given a separation in units.")
{
    if (2 <= args->ArgC()) {
        float ipd = (float)atof(args->ArgV(1));
        float fov = 3 <= args->ArgC() ? (float)atof(args->ArgV(2)) : 0.0f;
        if (0.0f == ipd) {
            g_Eyes[1].enabled = false;
            g_Eyes[2].enabled = false;
            advancedfx::Message("mirv_vr_ipd: eyes off.\n");
            return;
        }
        AfxVr_SetEye(1, true, -0.5f * ipd, 0, 0, 0, 0, 0, fov);
        AfxVr_SetEye(2, true, +0.5f * ipd, 0, 0, 0, 0, 0, fov);
        advancedfx::Message("mirv_vr_ipd: %f units, fov %f.\n", ipd, fov);
        return;
    }

    advancedfx::Message(
        "mirv_vr_ipd <units> [fov] - symmetric eyes, a shorthand for two mirv_vr_eye calls.\n"
        "A 63 mm interpupillary distance is 2.5 units. 0 disables.\n");
}

CON_COMMAND(mirv_vr_selftest, "cs2-vr-spectator: report whether the hard-coded view offsets still look right.")
{
    CheckGameBuildOnce();

    advancedfx::Message(
        "mirv_vr_selftest\n"
        "  game build      %s\n"
        "  offsets built for %s%s\n"
        "  offsets         fov +0x%x, origin +0x%x, angles +0x%x (from CViewRender+0x10)\n",
        g_GameClientVersion[0] ? g_GameClientVersion : "unknown (steam.inf not read)",
        AFXVR_TESTED_CLIENT_VERSION,
        g_GameClientVersion[0] ? (g_GameBuildMatches ? "  - match" : "  - MISMATCH") : "",
        AFXVR_OFS_FOV, AFXVR_OFS_ORIGIN, AFXVR_OFS_ANGLES);

    if (nullptr == g_ViewStruct) {
        advancedfx::Message(
            "  view struct     not seen yet - start a demo, the check runs on the first frame.\n");
        return;
    }

    AfxVrMath::ViewCheck r = AfxVrMath::CheckView(g_BaseOrigin, g_BaseAngles, g_BaseFov);
    advancedfx::Message(
        "  last read back  origin=(%.1f, %.1f, %.1f) angles=(%.1f, %.1f, %.1f) fov=%.1f\n"
        "  verdict         %s\n",
        g_BaseOrigin[0], g_BaseOrigin[1], g_BaseOrigin[2],
        g_BaseAngles[0], g_BaseAngles[1], g_BaseAngles[2], g_BaseFov,
        r.ok ? "looks like a camera" : r.why);

    if (!r.ok) {
        advancedfx::Message(
            "\n"
            "  The eye poses are not being written while this is the case, which is the\n"
            "  safe answer but not a working one. Re-measuring the offsets is described in\n"
            "  docs/05-view-setup-point.md.\n");
    }
}

CON_COMMAND(mirv_vr_log, "cs2-vr-spectator: trace the pass loop and view setup for N frames.")
{
    int frames = 2 <= args->ArgC() ? atoi(args->ArgV(1)) : 1;
    g_AfxVrLogUntilFrame = g_AfxVrFrameIndex + frames;
    advancedfx::Message("mirv_vr_log: logging frames %i..%i\n", g_AfxVrFrameIndex, g_AfxVrLogUntilFrame - 1);
}

void AfxVr_SetMakeMatrix(AfxVr_MakeMatrix_t fn) {
    g_MakeMatrix = fn;
}

CON_COMMAND(mirv_vr_remakematrix, "cs2-vr-spectator: call the client's matrix builder per eye. Does NOT fix the HUD - see help.")
{
    if (2 <= args->ArgC()) {
        g_HudFix = 0 != atoi(args->ArgV(1));
        advancedfx::Message("mirv_vr_remakematrix: %s%s\n",
            g_HudFix ? "on" : "off",
            g_HudFix ? " - note this cancels the eye offset; see the help text" : "");
        if (g_HudFix && nullptr == g_MakeMatrix) {
            advancedfx::Warning("  ...but the matrix builder was never hooked, so this will do nothing.\n");
        }
        return;
    }

    advancedfx::Message(
        "mirv_vr_remakematrix 0|1 - call the client's matrix builder after writing an eye\n"
        "pose. Kept because it answers a question, not because it works.\n"
        "\n"
        "The problem it was written for: name tags and health numbers are placed with a\n"
        "world-to-screen matrix the client builds once a frame from the camera in the\n"
        "middle, while the HUD itself is drawn once per pass. So in an eye rendered from\n"
        "somewhere else, the tags float free of the people they belong to.\n"
        "\n"
        "The idea was to let the engine redo its own calculation with the eye's camera,\n"
        "rather than compute a matrix here and get the conventions wrong. Measured, in\n"
        "docs/experiments/15-hud-per-eye.md: it does not work. Calling the builder puts\n"
        "the base camera back, so the pass renders from the middle and the stereo is gone.\n"
        "The builder is not a consumer of the fields we write - it has its own source of\n"
        "truth and restores them.\n"
        "\n"
        "So issue #3 is deep rather than cheap, and this switch is the evidence. Leave it\n"
        "off.\n"
        "\n"
        "Current value: %s%s\n",
        g_HudFix ? "1" : "0",
        nullptr == g_MakeMatrix ? " (unavailable: the matrix builder was not hooked)" : "");
}

bool AfxVr_GetLastApplied(int passIndex, float outOrigin[3], float outAngles[3], float * outFov) {
    if (passIndex < 0 || passIndex > 3) return false;
    if (!g_Applied[passIndex]) return false;
    for (int i = 0; i < 3; i++) {
        outOrigin[i] = g_AppliedOrigin[passIndex][i];
        outAngles[i] = g_AppliedAngles[passIndex][i];
    }
    if (outFov) *outFov = g_AppliedFov[passIndex];
    return true;
}

namespace {
float g_Proj00 = 0.0f;
float g_Proj11 = 0.0f;
bool g_ProjKnown = false;
}

void AfxVr_SetProjectionDiagonal(float m00, float m11) {
    g_Proj00 = m00;
    g_Proj11 = m11;
    g_ProjKnown = (m00 > 1e-6f && m11 > 1e-6f);
}

bool AfxVr_GetRenderedFovDegrees(float * outHorizontal, float * outVertical) {
    if (!g_ProjKnown) return false;
    const double r2d = 180.0 / M_PI;
    if (outHorizontal) *outHorizontal = (float)(2.0 * atan(1.0 / g_Proj00) * r2d);
    if (outVertical)   *outVertical   = (float)(2.0 * atan(1.0 / g_Proj11) * r2d);
    return true;
}
