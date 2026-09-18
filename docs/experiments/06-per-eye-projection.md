# Experiment 06 — can the projection differ between eyes?

Date: 2026-09-18. Self-built hook, CS2 build 2000908, `pro_mirage.dem` paused, 1280x720.

Phase D step 1. A headset needs a different frustum per eye. Experiments 04 and 05 gave
per-eye *position*; this asks whether the projection can follow, and it is deliberately
the first thing done in phase D — if the answer were no, the OpenXR work would be built
on sand.

## The reasoning that picked the test

The view setup trampoline runs once per frame, *after* all passes — yet a per-pass camera
change does reach the image. So something inside `OnClientOutput` reads the view struct
and builds that pass's matrices. If the fov field is read there too, per-eye projection
costs nothing beyond the lever that already works.

Indicator: **+40° on the right eye only**. A fov that wide cannot be mistaken for
anything else.

## Result

| take | `mirv_vr_fovtest` | pixels differing between eyeL and eyeR |
| --- | --- | --- |
| control | 0 | 0.03 – 0.08 %, max deviation 2 |
| test | +40° | **93.65 %**, mean 41.1 |

And the images say it is a field of view and not damage: same frame, same round timer,
but the right eye shows far more of the map — palm trees, the whole of the A and B
approaches, a second player entering at the left edge — with the viewmodel correspondingly
smaller.

**Per-eye field of view works**, through the same write that carries the camera.

## What the trace says about the projection matrix

Logged `CViewRender::UnkMakeMatrix`, which is where HLAE reads the projection:

```
frame=10537 BeginMainRenderPass
frame=10537 BeginNextRenderPass -> pass=1
frame=10537 BeginNextRenderPass -> pass=2
frame=10537 BeginNextRenderPass -> pass=3
frame=10537 pass=3 MakeMatrix this=00007FFB59E90630 proj[0][0]=0.750000 proj[1][1]=1.333334
```

Once per frame, after the passes — the same shape as the view setup. So the matrix cached
at `CViewRender+0x218` is **not** what a pass renders with; it is a copy for the client's
own use (campath drawing, world-to-screen). The real per-pass projection is rebuilt
somewhere inside `OnClientOutput` from the view struct.

Also settled: `this` is `00007FFB59E90630`, exactly 16 bytes below the pointer the view
setup trampoline receives. They are one object — `CViewRender` — and note 05's field
offsets are relative to `CViewRender+0x10`.

`proj[1][1] / proj[0][0]` = 1.7778 = 16/9, the window aspect, as expected.

## What this means for the VR bridge

OpenXR takes the four frustum angles per eye in `XrCompositionLayerProjectionView::fov`,
and they must describe **what was actually rendered**, not what the runtime recommended.
A runtime's recommended frustum for a Quest 3 is asymmetric, but rendering a *symmetric*
frustum that encloses it and reporting that symmetric fov is correct — the cost is wasted
pixels at the edges, not a wrong image.

CS2 gives exactly one fov number per eye, so symmetric is all it can express. That is
therefore enough, and the expensive alternative — intercepting the projection in the
constant buffer below the client — is not needed.

Two things still have to be pinned down before submitting frames:

- **What CS2's fov number means** (horizontal at which aspect). Calibratable against the
  projection matrix, which can be read at `CViewRender+0x218` for a known fov.
- **Resolution.** Passes render at window size; a Quest 3 wants roughly 2064x2208 per
  eye. The window can be sized to that, awkward as it looks on a laptop panel, or a lower
  per-eye resolution can be submitted and left to the runtime to scale.

Still unverified: per-pass **angles**, at `+0x4b8` in the same struct. Same lever, so the
expectation is that it works, but it has not been measured.
