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
// is the base pose the eyes are offsets from - recorded before anything is added to it,
// so nothing accumulates frame over frame.
//
// It also writes the HEAD back through its arguments, and returns true when it did, so
// the trampoline's own write-back carries it into the view struct.
//
// That second job is not decoration. Everything the viewer adds - head orientation, the
// yaw offset, the stick's movement - used to be applied only between passes, and
// AfxVr_BeforeViewSetupRead puts the base camera back before the engine's next read. So
// every consumer that derives from the camera once per FRAME rather than once per pass
// still saw the demo camera and not the viewer: the audio listener above all, which is
// why sound did not turn with the head or move with the stick, and also the client's
// world-to-screen matrix, and whatever culling does. The image was the only thing that
// ever saw the head.
//
// The eyes stay per-pass. Only the head - the midpoint, with no interpupillary offset -
// belongs here.
bool AfxVr_AfterViewSetup(void * pViewStruct, float & tx, float & ty, float & tz,
                          float & rx, float & ry, float & rz, float & fov);

// The head pose, as opposed to either eye: the same orientation both eyes render with,
// and no interpupillary offset. Driven from xrLocateViews alongside AfxVr_SetEye.
// `room` is how far the head has moved in the room since the viewer was last recentred,
// in OpenXR's axes and metres. Until this existed only the head's ORIENTATION reached the
// game: leaning, crouching and stepping did nothing, the world was glued to the face, and
// the pose reported to the compositor described a translation that had never been
// rendered.
void AfxVr_SetHead(bool enabled, float dPitch, float dYaw, float dRoll, float fov,
                   float roomX, float roomY, float roomZ);

// Whether a step in the room is a step in the map. On by default.
void AfxVr_SetRoomScale(bool enabled);
bool AfxVr_GetRoomScale();

// How many times the view struct has read back as a camera. Counted whether or not VR is
// on, because the thing that wants to know - starting a session with nobody at the
// keyboard - has to decide before anything has been enabled.
unsigned int AfxVr_PlausibleViewCount();

// The same factor mirv_vr_ipd applies to the eye separation. A lean has to be scaled with
// it: world scale is one number, and eyes scaled while the head is not give the wrong
// parallax for the stereo the viewer is actually shown.
void AfxVr_SetRoomIpdScale(float scale);


// Called from the render pass loop, before the given pass renders.
void AfxVr_OnBeginRenderPass(int passIndex);

// What was last actually written into the view struct for a pass, as opposed to what was
// asked for. The difference between those two is where an evening goes: a setting that
// changes nothing on screen is either not arriving or not being applied, and only the
// values at the point of the write can tell the two apart.
bool AfxVr_GetLastApplied(int passIndex, float outOrigin[3], float outAngles[3], float * outFov);

// The engine's own projection matrix diagonal, from the hook in main.cpp. proj[0][0] is
// 1/tan(horizontal half-angle) and proj[1][1] is 1/tan(vertical half-angle), so this turns
// "what field of view did CS2 actually render" from a model into a measurement. It matters
// because the number handed to the engine is not the one it renders: fov in Source is
// defined at 4:3 and this window is portrait.
void AfxVr_SetProjectionDiagonal(float m00, float m11);
bool AfxVr_GetRenderedFovDegrees(float * outHorizontal, float * outVertical);

// The client builds its world-to-screen and projection matrices once a frame, from the
// base camera, in a function main.cpp already hooks. The HUD is drawn once per *pass* and
// reads those matrices - so name tags and health numbers land where they would have been
// on the monitor, not where the player is from this eye. That is issue #3.
//
// The obvious idea is to let the engine redo its own work with the camera we have just
// written, rather than compute a matrix here and get the conventions wrong. main.cpp hands
// the original function over for that.
//
// It does not work, and the negative result is worth keeping: calling the builder puts the
// base camera back, so the pass renders from the middle and the stereo disappears
// (docs/experiments/15-hud-per-eye.md). The builder is not a consumer of the fields we
// write; it has its own source of truth and restores them.
//
// mirv_vr_remakematrix is the switch, off by default, kept as the evidence that issue #3
// is deep rather than cheap.
typedef void (__fastcall * AfxVr_MakeMatrix_t)(void * pCViewRender);
void AfxVr_SetMakeMatrix(AfxVr_MakeMatrix_t fn);

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
