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

// Render thread, from the pass's BeforePresent command. eyeIndex is 0 or 1.
void MirvVrXr_RenderThread_SubmitEye(int eyeIndex, ID3D11DeviceContext * pContext, ID3D11Texture2D * pTexture);
