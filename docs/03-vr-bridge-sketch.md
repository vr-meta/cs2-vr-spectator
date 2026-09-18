# VR bridge: where the eye textures come from

Date: 2026-09-18. Derived from reading `AfxHookSource2/RenderSystemDX11Hooks.cpp`
(5368 lines, MIT). Code read only. This is a design sketch to be falsified by
experiment, not a specification.

## The insertion point

HLAE captures each render pass through a command queue that runs around `Present`:

```cpp
auto & queue = pRenderPassCommands.BeforePresent;
queue.Push([capture](ID3D11DeviceContext * pDeviceContext, ID3D11Texture2D * pTexture) {
    capture->OnBeforeGpuPresent(pDeviceContext, pTexture, 1.0f, 0.0f);
});
```

The `pTexture` handed to that lambda is the **swap chain back buffer**
(`g_pSwapChain->GetBuffer(0, ...)`). Every extra pass renders into the back buffer and
is read out from there; `EngineThread_Suppress_Present()` pushes a lambda setting
`g_Present_Suppress = true` so intermediate passes never reach the monitor.

What `OnBeforeGpuPresent` then does is exactly wrong for VR: it performs a GPU-to-CPU
`CopyResource` into a staging texture and hands it to a worker thread that writes files.
A round trip to system memory, for something that needs to stay on the GPU.

**So the bridge is a substitution, not an addition:** push a lambda that copies the back
buffer into the OpenXR (or OpenVR) swapchain image for the current eye, and let the
existing suppression keep intermediate passes off the screen.

## Sketch of a stereo frame

```
xrWaitFrame / xrBeginFrame
    acquire predicted head pose
        |
        v
CRenderService::OnClientOutput hook  (HLAE, exists)
    |
    +-- main pass ------------> back buffer --> monitor (Present allowed)
    |                            also generates the UI background texture,
    |                            which later passes do not regenerate
    |
    +-- pass "left"  ---------> back buffer --> CopyResource --> XR swapchain image L
    |     BeforeCommands set the left eye offset            (Present suppressed)
    |     SceneSystem::FrameUpdate re-prepares the scene
    |
    +-- pass "right" ---------> back buffer --> CopyResource --> XR swapchain image R
          BeforeCommands set the right eye offset           (Present suppressed)
        |
        v
xrEndFrame with both layer views
```

Everything left of the copies already exists in HLAE. The copies, the pose input and the
frame timing are the work.

## What has to be built

| Piece | Notes |
| --- | --- |
| XR session lifecycle | Create session, swapchains, reference space. Handle loss and recentering. |
| Pose input | `xrLocateViews` for per-eye pose and projection, fed into the camera override. |
| Per-eye camera | `mirv_input` overrides the camera per frame, not per pass. If the demo view offset convar is dead, a per-pass camera hook is needed instead. |
| GPU copy | Replace the staging copy with a copy into the XR swapchain image. Formats and sRGB handling must match. |
| Frame pacing | The multi-pass path was built for offline capture where a slow frame is harmless. |

## Known problems

**Back buffer resolution.** Every pass renders at the game window's resolution, but VR
wants per-eye resolution — roughly 2064x2208 per eye for a Quest 3 at full rate. Options,
none yet tested: run the game window at eye resolution and scale the monitor view; render
to an off-screen target instead of the back buffer; or accept a resolution compromise for
the first prototype. This is the most likely place for the sketch to break.

**Three renders per frame, not two.** The main pass is mandatory — HLAE's own comment
says it generates the UI background texture that later passes reuse. So a stereo frame
costs the main pass plus two eyes. Whether the main pass can be made cheap, or skipped
once the UI texture exists, is an open question with direct performance consequences.

**Frame budget.** 72 Hz leaves 13.9 ms for all three passes plus the copies and
submission. There is no evidence yet that this is achievable; it is the central risk of
the whole approach and cannot be assessed from source alone.

**Present suppression and the compositor.** The monitor still gets the main pass, so the
desktop view keeps working. But the interaction between suppressed Presents and XR frame
submission timing is unexamined.

## Why this beats the alternatives

A graphics-API wrapper in the portal2vr style cannot do this at all: by the time draw
calls arrive, culling and shadows are already resolved for one camera. Here each pass is
a real scene traversal through the engine's own path, which is what makes the second view
correct rather than merely plausible.

## Order of validation

1. Experiment 00 — do the stereo convars work, and do two passes of one paused frame
   differ only by eye offset?
2. Measure a two-pass frame. If it cannot approach the budget, everything above is moot
   and the feasibility gate needs the numbers written down.
3. Only then build the XR session and the copies.

Do not build the VR bridge before step 1 answers. The whole sketch rests on a convar
that may be a dead stub.
