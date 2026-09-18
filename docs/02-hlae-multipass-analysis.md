# HLAE multi-pass rendering: a ready-made second view pass

Date: 2026-09-18. Source read: `advancedfx/AfxHookSource2` at `main`, MIT licensed,
last push 2026-09-12. Code read only — nothing run yet.

## The finding

`AfxHookSource2` already renders the CS2 scene **multiple times per frame from one
simulation state**, in shipping, maintained code. The mechanism exists to let
`mirv_streams` capture several versions of the same moment (depth, mattes, isolated
layers) for compositing in fragmovie work, but the machinery is general.

This is the exact capability milestone 3 needs, and the note in
`01-source2-integration-points.md` underestimated it as "substantial reverse
engineering". It is not; it is written, debugged and patch-tracked.

## How it works

`RenderServiceHooks.cpp` detours `engine2`'s `CRenderService::OnClientOutput` — the
call that renders a client frame:

```cpp
void __fastcall My_Engine2_RenderService_OnClientOutput(void * pUnk0, void * pUnk1) {
    RenderSystemDX11_EngineThread_BeginMainRenderPass();
    g_Engine2_RenderService_OnClientOutput(pUnk0, pUnk1);   // the normal frame

    while (RenderSystemDX11_EngineThread_HasNextRenderPass()) {
        g_Old_SceneSystem_WaitForRenderingToComplete(g_pSceneSystem);
        ClearThreadSceneLayerContexts();
        FrameUpdate(g_pSceneSystem, 1);                      // re-prepare the scene

        RenderSystemDX11_EngineThread_BeginNextRenderPass();
        g_Engine2_RenderService_OnClientOutput(pUnk0, pUnk1); // render it AGAIN
    }
}
```

Three things matter here:

1. **The whole client render is re-entered**, not replayed at the draw-call level. Each
   pass is a genuine scene traversal, so culling, shadows and visibility are resolved
   per pass. This is what a graphics-API wrapper fundamentally cannot do.
2. **`SceneSystem::FrameUpdate` is called between passes** via vtable index, with
   `WaitForRenderingToComplete` before it. The scene is re-prepared for rendering
   without the game simulation advancing.
3. **The main pass must run first** — the comment notes it generates the UI background
   texture, which later passes do not regenerate.

## Per-pass state is driven by convars

`CStreamSettings::CompareRenderPass` decides whether two streams can share a pass. Among
the discriminators:

```cpp
if (cmp = CompareCommands(BeforeCommands, o.BeforeCommands)) return cmp;
if (cmp = CompareCommands(AfterCommands,  o.AfterCommands))  return cmp;
```

Streams whose `BeforeCommands` differ are placed in **separate render passes**. And
`ExecuteCommands` applies those commands by writing the convar value directly and
invoking `CallChangeCallback` — so arbitrary convars can be set between passes.

### Why that is the whole game

Combine this with the finding in `01-source2-integration-points.md`:

> `cl_demo_view_offset_left` — "View offset during demo playback (+/- 1.25 is a good
> default for human average left/right eye offset)"

Two streams, one with `BeforeCommands` setting the offset to `-1.25` and one to `+1.25`,
would produce two full scene renders of the same frozen demo frame at two eye positions.
That is stereo, assembled entirely from parts that already exist, without writing a
renderer.

**This is a hypothesis about composition, not a demonstration.** It rests on the convar
still functioning, which is exactly what experiment 00 tests.

## What is already available

| Capability | Where | Status |
| --- | --- | --- |
| Re-render scene N times per frame | `RenderServiceHooks.cpp` | exists |
| Scene re-prepared between passes | `SceneSystem::FrameUpdate` via vtable | exists |
| Per-pass convar application | `CStreamSettings::BeforeCommands` | exists |
| External projection matrix input | `RenderSystemDX11_SupplyProjectionMatrix(VMatrix)` | exists |
| Camera override | `mirv_input`, `mirv_camio`, `mirv_campath` | exists, per frame |
| Suppressing `Present` on extra passes | `EngineThread_Suppress_Present()` | exists |

## What is missing for VR

- **Output target.** Extra passes are captured to disk through `CAfxCapture` (TGA and
  similar). VR needs the pass result as a GPU texture handed to the compositor, never
  touching storage. `CAfxCapture` is the insertion point.
- **Per-pass camera pose.** Camera override is per frame, not per pass. Convars are the
  only per-pass channel today, which is why the viability of `cl_demo_view_offset_left`
  matters so much. Otherwise a per-pass camera hook has to be added.
- **Pose input and frame timing.** Nothing here knows about a headset. Pose prediction
  and submission timing are entirely ours to build.
- **Frame pacing.** The multi-pass path was built for offline capture, where a frame
  taking hundreds of milliseconds is fine. VR needs it inside a 13.9 ms budget at 72 Hz.
  This is the largest unknown, and it is a performance question, not a feasibility one.

## Consequence for the plan

The candidate ranking in `01-source2-integration-points.md` should be read with this in
mind: approach 2 (engine-level second view pass) is far cheaper than it was described,
and approaches 3 and 4 correspondingly less attractive.

The best case is now a combination rather than a single mechanism: **HLAE multi-pass
supplying the second scene traversal, with the per-eye offset carried by the engine's
own demo convar.** Experiment 00 tests the second half of that; a follow-up experiment
should test the multi-pass half by running `mirv_streams` with two differing
`BeforeCommands` and confirming that two distinct renders of one paused frame come out.

## Licensing

MIT, so this is reusable with attribution. That is a materially better position than
portal2vr, which carries no license at all.
