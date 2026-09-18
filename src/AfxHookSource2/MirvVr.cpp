#include "stdafx.h"

#include "MirvVr.h"

#include "WrpConsole.h"

#include "../shared/AfxConsole.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdlib.h>

// Field offsets into the view struct the trampoline receives, which is CViewRender+0x10
// for CS2 build 2000908. These move on game updates - see docs/05-view-setup-point.md.
#define AFXVR_OFS_FOV     0x498
#define AFXVR_OFS_ORIGIN  0x4a0
#define AFXVR_OFS_ANGLES  0x4b8

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

// Source's AngleVectors, for angles in degrees as (pitch, yaw, roll).
void AngleVectors(const float angles[3], float forward[3], float right[3], float up[3]) {
    const double d = M_PI / 180.0;
    double sp = sin(angles[0] * d), cp = cos(angles[0] * d);
    double sy = sin(angles[1] * d), cy = cos(angles[1] * d);
    double sr = sin(angles[2] * d), cr = cos(angles[2] * d);

    forward[0] = (float)(cp * cy);
    forward[1] = (float)(cp * sy);
    forward[2] = (float)(-sp);

    right[0] = (float)(-sr * sp * cy + cr * sy);
    right[1] = (float)(-sr * sp * sy - cr * cy);
    right[2] = (float)(-sr * cp);

    up[0] = (float)(cr * sp * cy + sr * sy);
    up[1] = (float)(cr * sp * sy - sr * cy);
    up[2] = (float)(cr * cp);
}

bool AnyEyeEnabled() {
    for (int i = 1; i < 4; i++) if (g_Eyes[i].enabled) return true;
    return false;
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

    if (g_AfxVrFrameIndex < g_AfxVrLogUntilFrame) {
        advancedfx::Message(
            "AFXVR: frame=%i pass=%i SetupView this=%p org=(%f,%f,%f) ang=(%f,%f,%f) fov=%f\n",
            g_AfxVrFrameIndex, g_AfxVrPassIndex, pViewStruct, tx, ty, tz, rx, ry, rz, fov);
    }
}

void AfxVr_OnBeginRenderPass(int passIndex) {
    if (nullptr == g_ViewStruct || !AnyEyeEnabled()) return;
    if (passIndex < 0 || passIndex > 3) return;

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
    AngleVectors(angles, forward, right, up);

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
    // Move in the plane the viewer is facing. Deliberately horizontal: tilting the head
    // down and pushing forward should not drive you into the floor.
    const double d = M_PI / 180.0;
    double sy = sin(g_LastViewYaw * d), cy = cos(g_LastViewYaw * d);

    // Source: forward is (cos yaw, sin yaw, 0), right is (sin yaw, -cos yaw, 0).
    g_MoveOffset[0] += (float)(forward * cy + right * sy);
    g_MoveOffset[1] += (float)(forward * sy - right * cy);
    g_MoveOffset[2] += up;
}

void AfxVr_AddYaw(float degrees) {
    g_YawOffset += degrees;
    while (g_YawOffset > 180.0f) g_YawOffset -= 360.0f;
    while (g_YawOffset < -180.0f) g_YawOffset += 360.0f;
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

CON_COMMAND(mirv_vr_log, "cs2-vr-spectator: trace the pass loop and view setup for N frames.")
{
    int frames = 2 <= args->ArgC() ? atoi(args->ArgV(1)) : 1;
    g_AfxVrLogUntilFrame = g_AfxVrFrameIndex + frames;
    advancedfx::Message("mirv_vr_log: logging frames %i..%i\n", g_AfxVrFrameIndex, g_AfxVrLogUntilFrame - 1);
}
