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

bool g_FreeLook = false;
float g_YawOffset = 0.0f;   // added to the head's yaw so the room lines up with the map
float g_LastHeadYaw = 0.0f; // whatever the headset last reported, for Recenter

// Where the viewer has moved to, relative to the demo camera. World space, units.
float g_MoveOffset[3] = { 0.0f, 0.0f, 0.0f };
// The yaw the last frame was actually composed with, so a stick move goes where the
// viewer is looking rather than where the map's X axis points.
float g_LastViewYaw = 0.0f;

bool AnyEyeEnabled() {
    for (int i = 1; i < 4; i++) if (g_Eyes[i].enabled) return true;
    return false;
}

// See the note in MirvVr.h. The view struct we hold is CViewRender+0x10; the matrix
// builder wants CViewRender itself.
AfxVr_MakeMatrix_t g_MakeMatrix = nullptr;
bool g_HudFix = false;
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

} // namespace

void AfxVr_BeforeViewSetupRead(void * pViewStruct) {
    if (!g_Dirty || pViewStruct != g_ViewStruct) return;

    float * pOrigin = (float*)((unsigned char*)pViewStruct + AFXVR_OFS_ORIGIN);
    float * pAngles = (float*)((unsigned char*)pViewStruct + AFXVR_OFS_ANGLES);
    float * pFov    = (float*)((unsigned char*)pViewStruct + AFXVR_OFS_FOV);

    pOrigin[0] = g_BaseOrigin[0]; pOrigin[1] = g_BaseOrigin[1]; pOrigin[2] = g_BaseOrigin[2];
    pAngles[0] = g_BaseAngles[0]; pAngles[1] = g_BaseAngles[1]; pAngles[2] = g_BaseAngles[2];
    *pFov = g_BaseFov;

    g_Dirty = false;
}

void AfxVr_AfterViewSetup(void * pViewStruct, float tx, float ty, float tz,
                          float rx, float ry, float rz, float fov) {
    g_ViewStruct = pViewStruct;
    g_BaseOrigin[0] = tx; g_BaseOrigin[1] = ty; g_BaseOrigin[2] = tz;
    g_BaseAngles[0] = rx; g_BaseAngles[1] = ry; g_BaseAngles[2] = rz;
    g_BaseFov = fov;

    // Cheap, and the first frame is the right moment: the game is fully up by the time a
    // view is being set up, and the answer is wanted before anything is written.
    CheckGameBuildOnce();

    if (g_AfxVrFrameIndex < g_AfxVrLogUntilFrame) {
        advancedfx::Message(
            "AFXVR: frame=%i pass=%i SetupView this=%p org=(%f,%f,%f) ang=(%f,%f,%f) fov=%f\n",
            g_AfxVrFrameIndex, g_AfxVrPassIndex, pViewStruct, tx, ty, tz, rx, ry, rz, fov);
    }
}

void AfxVr_OnBeginRenderPass(int passIndex) {
    if (nullptr == g_ViewStruct || !AnyEyeEnabled()) return;
    if (passIndex < 0 || passIndex > 3) return;

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
        *pFov = g_BaseFov;
        return;
    }

    // The frame the eye offset is measured in has to be the same frame the final angles
    // describe, or the eyes end up separated along the wrong axis.
    float angles[3];
    if (g_FreeLook) {
        // Position from the demo, orientation from the headset alone.
        angles[0] = eye.dPitch;
        angles[1] = eye.dYaw + g_YawOffset;
        angles[2] = eye.dRoll;
        g_LastHeadYaw = eye.dYaw;
    } else {
        // The yaw offset applies here too, or turning with the stick would silently do
        // nothing whenever the demo camera owns the orientation.
        angles[0] = g_BaseAngles[0] + eye.dPitch;
        angles[1] = g_BaseAngles[1] + eye.dYaw + g_YawOffset;
        angles[2] = g_BaseAngles[2] + eye.dRoll;
        g_LastHeadYaw = eye.dYaw;
    }

    g_LastViewYaw = angles[1];

    float forward[3], right[3], up[3];
    AfxVrMath::AngleVectors(angles, forward, right, up);

    for (int i = 0; i < 3; i++) {
        pOrigin[i] = g_BaseOrigin[i]
            + g_MoveOffset[i]
            + eye.right   * right[i]
            + eye.forward * forward[i]
            + eye.up      * up[i];
    }

    pAngles[0] = angles[0];
    pAngles[1] = angles[1];
    pAngles[2] = angles[2];

    *pFov = (0.0f < eye.fov) ? eye.fov : g_BaseFov;

    g_Dirty = true;

    for (int i = 0; i < 3; i++) {
        g_AppliedOrigin[passIndex][i] = pOrigin[i];
        g_AppliedAngles[passIndex][i] = pAngles[i];
    }
    g_AppliedFov[passIndex] = *pFov;
    g_Applied[passIndex] = true;

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
    g_YawOffset = AfxVrMath::NormalizeDegrees(g_YawOffset + degrees);
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
