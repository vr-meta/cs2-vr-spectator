#pragma once

// cs2-vr-spectator: per-eye camera for the render passes.
//
// The engine resolves the camera once per frame, outside the render pass loop, but the
// CViewRender object it writes into is persistent and every pass reads it again. So each
// eye is produced by rewriting that object between passes. Measured in
// docs/experiments/04-per-pass-camera.md and 06-per-eye-projection.md.
//
// Pass 0 is the main pass - what the monitor shows - and is never offset. Passes 1 and 2
// are the eyes. The pass loop starts one more pass than it uses, so there is normally a
// pass 3 with no eye assigned.

// Maintained by the render pass loop in RenderSystemDX11Hooks.cpp.
extern int g_AfxVrFrameIndex;
extern int g_AfxVrPassIndex;

// Armed by mirv_vr_log; the probes print while g_AfxVrFrameIndex is below it.
extern int g_AfxVrLogUntilFrame;

// Called from the view setup trampoline, before it reads the view struct. Undoes the
// last pass's eye offset, which the trampoline would otherwise read back as the game's
// own camera and accumulate.
void AfxVr_BeforeViewSetupRead(void * pViewStruct);

// Called from the view setup trampoline once the camera for the frame has settled. This
// is the base pose the eyes are offsets from.
void AfxVr_AfterViewSetup(void * pViewStruct, float tx, float ty, float tz,
                          float rx, float ry, float rz, float fov);

// Called from the render pass loop, before the given pass renders.
void AfxVr_OnBeginRenderPass(int passIndex);

// Set the pose of one eye. Offsets are in the base camera's own frame - right, forward,
// up in units - and angle deltas are added to the base angles. A fov of 0 means "use the
// game's". This is the interface an OpenXR layer will drive from xrLocateViews.
void AfxVr_SetEye(int passIndex, bool enabled,
                  float right, float forward, float up,
                  float dPitch, float dYaw, float dRoll,
                  float fov);

// Free look: take only the *position* from whatever camera the demo is using, and the
// orientation entirely from the headset. Following a player otherwise means their aim
// yanks the viewer's head around, which is the single most reliable way to make someone
// ill in VR.
//
// With free look off, the headset's rotation is added to the demo camera's, which is the
// right behaviour for a fixed observation point.
void AfxVr_SetFreeLook(bool enabled);
bool AfxVr_GetFreeLook();

// Align the free-look forward direction with where the demo camera is currently facing,
// so "straight ahead" in the room means "straight ahead" in the map.
void AfxVr_Recenter();

// Move the viewer relative to wherever the demo camera is. Offsets are in the viewer's
// own frame - right, forward, up, in units - so "forward" means where they are looking.
// This is what the controller sticks drive.
void AfxVr_AddMove(float right, float forward, float up);

// Turn the viewer. Only meaningful with free look on; when the demo camera owns the
// orientation there is nothing to turn.
void AfxVr_AddYaw(float degrees);

// Put the viewer back on the demo camera.
void AfxVr_ResetMove();
