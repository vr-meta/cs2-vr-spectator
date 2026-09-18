# Experiment 07 — the per-eye pose interface

Date: 2026-09-18. Self-built hook, CS2 build 2000908, `pro_mirage.dem` paused, 1280x720.

Phase D step 2. The per-pass code from experiments 04 and 06 was ad-hoc, spread across
`main.cpp` and `RenderSystemDX11Hooks.cpp`, and expressed only what each experiment
needed. It is now one module, `AfxHookSource2/MirvVr.{h,cpp}`, with one interface:

```
mirv_vr_eye <pass> <right> <forward> <up> <dPitch> <dYaw> <dRoll> <fov>
mirv_vr_ipd <units> [fov]      // shorthand for two symmetric eyes
mirv_vr_eye off
```

Offsets are in the spectator camera's **own frame** — right, forward, up — rather than
world axes, because that is the frame a headset pose arrives in. Angles are deltas on the
camera's. A fov of 0 keeps the game's. This is deliberately the shape `xrLocateViews`
fills in, so the OpenXR layer becomes a caller rather than a rewrite.

## What was measured

Three takes of one paused frame:

| take | setting | pixels differing between eyeL and eyeR |
| --- | --- | --- |
| control | `mirv_vr_eye off` | 0.00 % (a handful of pixels, max 21) |
| regression | `mirv_vr_ipd 2.5` | 79.92 %, mean 9.6 |
| angles | right eye yaw +20° | **97.03 %**, mean 44.8 |

The first two are the regression check — moving working code is exactly when something
breaks silently. Separation 0 still gives identical images and 2.5 units still gives the
stereo pair.

The third closes the **last unverified lever**. Position was measured in experiment 04,
fov in 06; the angle fields at `+0x4b8` were assumed to work and had not been tested.
They do:

```
frame=10216 pass=2 eye ang=(0.000000,20.000000,0.000000) fov=90.000000
frame=10216 pass=3 SetupView ang=(0.000000,0.000000,0.000000) fov=90.000000
```

The eye is turned, the base is untouched on the next read, and the rendered frame is a
clean rotated view of the same scene.

The control's "0.00 % but max 21" is the kill feed again — a few pixels of a
wall-clock-driven UI fade, as in experiment 05.

## Where this leaves the bridge

Everything the headset needs to *send into* the engine now works and is measured:
position, orientation, field of view, per eye, per render pass. What remains is the
plumbing in the other direction.

- OpenXR session on the game's D3D11 device. `g_pDevice` and `g_pSwapChain` are already
  globals in `RenderSystemDX11Hooks.cpp`.
- `xrWaitFrame` / `xrBeginFrame` at the top of the pass loop, `xrLocateViews` to fill the
  two eyes, `xrEndFrame` after them.
- Frame submission. The render command queue already delivers
  `(ID3D11DeviceContext*, ID3D11Texture2D*)` at `BeforePresent` for each pass — the same
  hook the file capture uses — so submitting an eye is a `CopyResource` into a swapchain
  image rather than a copy into a staging texture.
- Per-eye resolution is the window size, so the first version runs the game windowed at
  the eye resolution. 1080x1080 or so is enough to test with, and the display is
  2560x1600, so it fits.

Open question deferred to when frames are actually submitted: the extra render passes
only exist while `mirv_streams record` is running, because they come from
`m_RecordingExtraPasses`. VR needs those passes without writing 300 MB per second to
disk, so either a stream gains a "submit to VR instead of a file" output, or the pass
loop gains a VR source of its own.
