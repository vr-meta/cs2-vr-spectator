# Experiment 10 — the frame budget

Date: 2026-09-19. Quest 3 over Link, SteamVR/OpenXR 2.17.10, RTX 4070 Laptop,
`pro_mirage.dem`, CS2 at its default (maximum) graphics settings.

Phase E, prompted by the obvious question once frames were reaching the headset: can the
resolution go higher? It cannot, and this is why.

## How it is measured

Frames counted at `xrEndFrame` inside the hook, averaged over two seconds and logged —
`mirv_vr_xr fps 1`, on by default in `scripts/cs2/vr.cfg`. The game's own frame counter is
the wrong number: it also counts frames the headset never sees.

## Result

| configuration | frames/s | per frame |
| --- | --- | --- |
| 2528x2780 per eye, four scene traversals | 36 – 39 | ~27 ms |
| 2528x2780 per eye, three scene traversals | 38 – 48 | ~23 ms |
| **what the headset asks for at 72 Hz** | **72** | **13.9 ms** |

Roughly half the frame rate the headset wants. SteamVR fills the gap by reprojecting,
which is why the image looks acceptable but shows artefacts when the head moves.

**So raising the resolution is the wrong direction.** It is already at the runtime's full
recommendation, and every extra pixel makes the shortfall worse.

## The wasted pass, removed

The pass loop always began one more render pass than it used: with two eyes it rendered
the whole scene four times and discarded the fourth. This was noticed in experiment 04 and
recorded as a cost to pay attention to later; later is now.

The cause was where the pass count advanced. `EngineThread_EndNextRenderPass` incremented
it, so `HasNextRenderPass` was still true when the last eye had been begun but not yet
ended, and the loop started another. Advancing the count in
`EngineThread_BeginNextRenderPass` instead fixes it; the final pass is still ended
properly, by the scene system hook.

Worth **~15%**, not the 33% that dropping one traversal in four would suggest. So a
meaningful share of the frame time is not the scene traversal itself — worth knowing
before optimising the wrong thing.

## What is left to try, in order of expected return

1. **The game's graphics settings.** Untouched all project; CS2 is running at its defaults
   with shadows, effects and model detail at full. This is the obvious first lever and has
   not been tested at all.
2. **Resolution.** Dropping to ~70% linear halves the pixels. Sharpness for frame rate is
   a trade the viewer can judge; the code takes the resolution from the window, so it is a
   launch argument.
3. **Two traversals instead of three.** The main pass exists to feed the monitor and the
   UI background texture. Skipping or shrinking it while the headset is active would take
   the cost to the theoretical minimum for stereo, and is the only structural saving left.

## Note on the runtime

`xrCreateSession` began failing with `XR_ERROR_RUNTIME_FAILURE` after several sessions had
been created and destroyed in quick succession; restarting SteamVR cleared it each time.
The Quest also dropped out of Link twice during measurement, taking SteamVR and CS2 with
it. Neither is a fault in the bridge, but both cost time, so: measure in as few runs as
possible, and expect to restart the runtime between them.
