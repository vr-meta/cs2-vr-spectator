#pragma once

#include <d3d11.h>

// cs2-vr-spectator: the OpenXR side of the bridge.
//
// The engine-facing half lives in MirvVr.h: per-eye pose in, per-pass rendering out.
// This half talks to the headset runtime, drives those poses from xrLocateViews, and
// submits each rendered eye to a swapchain.
//
// The loader is opened by explicit path rather than linked, because this DLL is injected
// into a process that knows nothing about OpenXR and must not depend on the loader being
// on its search path.
//
// Threading: everything except the pose handover happens on the render thread, where
// HLAE hands over the finished texture for a pass. Splitting xrBeginFrame and xrEndFrame
// across two threads is asking for trouble, so they stay together. The consequence is
// that the poses a frame renders with are the ones located during the previous frame -
// one frame of latency, which is the price of a simple first version.

// Instance and system only. Reports what the runtime is; submits nothing.
bool MirvVrXr_Start();

// Session, reference space and the frame loop. Requires MirvVrXr_Start first.
bool MirvVrXr_SessionStart();
void MirvVrXr_SessionStop();

void MirvVrXr_Stop();

bool MirvVrXr_IsRunning();

// True while the session wants eyes rendered. The render pass loop asks this to decide
// whether to produce the two extra passes.
bool MirvVrXr_WantsPasses();

// Engine thread, at the top of every frame. Pumps the runtime's event queue - which is
// what drives the session from created to running - and feeds the last located views
// into MirvVr's eyes. It has to be unconditional: gating it on "is the session running"
// would mean the session could never start.
void MirvVrXr_EngineThread_Frame();

// Engine thread, when a pass is queued: which frame's pose, timing and mode that pass is
// being rendered with. The number is opaque; hand it back to the eye submission and to
// the main pass's panel clear and submission. All three must carry the queued ticket,
// because reading the current mode at either callback can describe the next frame.
//
// It exists because the render thread runs behind the engine thread by an amount that
// varies from frame to frame. Everything the projection layer has to report - the pose
// each eye was rendered from, the predicted display time, whether the runtime wanted the
// frame drawn at all - used to be read from globals at submission time, by which point
// the engine thread had often already overwritten them for the NEXT frame. Sometimes.
//
// A pose that is wrong by one frame at a hundred degrees a second is nearly three degrees
// out, and the runtime reprojects by the difference it was told about. Wrong by a
// different amount each frame, that is not swim; that is judder, and no amount of frame
// rate fixes it.
unsigned long long MirvVrXr_EngineThread_FrameTicket();

// Render thread, from the pass's BeforeUi or BeforePresent command. eyeIndex is 0 or 1.
// `ticket` is what MirvVrXr_EngineThread_FrameTicket returned when this pass was queued.
void MirvVrXr_RenderThread_SubmitEye(int eyeIndex, ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                     unsigned long long ticket);

// Render thread, from the MAIN pass's BeforePresent command. The main pass is the one
// image that still has the HUD and the demo menu composited into it once the eyes have
// stopped taking them, so it is what the quad layer carries. Used when the HUD panel is
// switched on with mirv_vr_panel or when this ticket requests the whole-window sheet.
// `ticket` is captured when this MAIN pass was queued, just as it is for the eyes.
void MirvVrXr_RenderThread_SubmitPanel(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                      unsigned long long ticket);

// True while the panel wants the main pass handed over. Asked by the pass loop so the
// callback is not queued for nothing. Not gated on a session: what it queues gates itself,
// and the alpha probe has to be runnable from a desk.
bool MirvVrXr_WantsPanel();

// Render thread, from the MAIN pass's BeforeUi command - after the world, before the UI.
// Wipes the finished world out of the back buffer so the only thing left for the panel to
// carry is the HUD itself, on transparent black.
//
// Without this the quad is the whole main pass: world and HUD together, opaque, and 1.6 m
// wide at 1.8 m is about 48 degrees of the view blocked by a second copy of a world the
// eyes are already showing. With it, the panel is a HUD and nothing else.
//
// The cost is that the monitor shows the HUD on black while the panel is on. The monitor
// is not the deliverable.
// Use the same queued ticket as SubmitPanel: clearing according to a later mode can
// erase a menu background or leave an opaque world behind a transparent sheet.
void MirvVrXr_RenderThread_ClearForPanel(ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture,
                                       unsigned long long ticket);

// True while that clear is wanted. Separate from MirvVrXr_WantsPanel because it needs no
// session: the whole point is to be able to look at what the panel would carry from a
// desk, with mirv_vr_xr passes.
bool MirvVrXr_WantsPanelClear();

// Which of the two capture points the eyes are taken from.
//
// BeforeUi fires once per render pass, immediately before that pass's UI is composited -
// measured, in docs/experiments/14-when-the-ui-is-drawn.md, not assumed. Capturing there
// gives eyes with no HUD and no demo menu baked in, which is what stereo wants: a flat
// overlay at screen depth is drawn at the wrong distance in a headset and doubled besides.
//
// BeforePresent is where this started, and keeps the UI. Still the default until the quad
// layer that replaces it exists, because a viewer with a wrong menu is better off than one
// with no menu at all.
bool MirvVrXr_CaptureBeforeUi();
