# Experiment 09 — frames in the headset

Date: 2026-09-19. Self-built hook, CS2 build 2000908, `pro_mirage.dem`, Quest 3 over Link,
SteamVR/OpenXR 2.17.10.

**CS2 renders into a Meta Quest 3.** Stereo, head tracked, from a demo playing inside the
real game. This is the feasibility gate the README set, cleared.

## What was built

- An OpenXR session on the game's own D3D11 device (`g_pDevice`), a `LOCAL` reference
  space, and one swapchain per eye sized to the back buffer.
- The frame loop — `xrWaitFrame` / `xrBeginFrame` / `xrLocateViews` / `xrEndFrame` — on
  the **render thread**, where HLAE hands over the finished texture for a pass. Splitting
  begin and end across threads would have been a race; the price is that a frame renders
  with the poses located during the previous one, one frame of latency.
- Submission as a plain `CopyResource` into the acquired swapchain image.
- **VR as its own source of render passes.** Until now the extra passes only existed
  while `mirv_streams record` was writing ~300 MB/s of TGA to disk. `CAfxStreams` now
  produces two passes for VR independently of recording.
- Poses from `xrLocateViews` feed the levers measured in experiments 04, 06 and 07:
  eye offset in the camera's own frame, angle delta, field of view.

## Three deadlocks of my own making, in order

Each of these has the same shape — gating something on a state that only that something
can produce — and each cost a build.

1. **Events.** The session's state machine is driven by `xrPollEvent`, which I called only
   from the submission path, which only ran once the session was running. The session
   could never start. Fixed by pumping events unconditionally every frame from the engine
   thread.
2. **The frame loop.** I ran it only once the session reached `SYNCHRONIZED`, but a
   runtime only advances a session past `READY` when the application starts submitting
   frames. The session sat at `READY` forever, and the headset showed SteamVR's void with
   the game's audio. Fixed by running the loop as soon as the session is running, and
   ending frames with zero layers when there is nothing to show.
3. **The frustum aspect.** Not a deadlock but the same kind of latent error: the vertical
   half-angle was reported equal to the horizontal one, which is correct only for a square
   image. The first test happened to be 1080x1080, so it was right by accident. Now
   derived from the actual image size.

## The format, which was the real obstacle

```
AFXVR: the runtime does not accept the game's back buffer format (27)
```

DXGI format 27 is `R8G8B8A8_TYPELESS`: CS2's back buffer is typeless, and no runtime
offers a typeless swapchain. That looked like it would force a shader blit.

It does not. `CopyResource` accepts a typeless source and a fully typed destination of the
same family, so the swapchain is created as `R8G8B8A8_UNORM_SRGB` (29) — sRGB rather than
plain UNORM because the game's pixels are gamma encoded and the compositor has to be told.

```
AFXVR: swapchains 1600x1600, back buffer format 27 -> swapchain 29, 3 images per eye.
AFXVR: submitting frames to the headset.
AFXVR: session state 3 -> 4 -> 5
```

## Resolution — full, and no code needed

Per-eye resolution is the window size, because submission copies the back buffer, and I
assumed the window could not exceed the display: on a 2560x1600 panel that would cap it at
1600x1600.

Wrong. Asking for `-w 2528 -h 2780` gives exactly that back buffer, with the window simply
extending past the screen edge:

```
AFXVR: swapchains 2528x2780, back buffer format 27 -> swapchain 29, 3 images per eye.
```

That is the runtime's full recommended resolution — 4x the pixels of the 1600x1600 step and
6x the first working frame — reached by changing two numbers rather than by rendering to an
off-screen target. Worth remembering before building something elaborate: try the cheap
thing first.

`-noborder` still helps, since a title bar would offset the client area.

## Two things that only show up in a headset

Both were reported within a minute of the first working frame, and neither is visible on a
monitor.

**The world swam when the head turned.** The projection layer reported the poses located
during *this* frame, but the image was rendered with the poses located during the previous
one. The runtime reprojects an image from the pose it was drawn from to the pose the eye
will be in at display time — given the wrong origin, it corrects in the wrong direction.
Fixed by reporting the poses actually rendered with, which is what the layer field means.
The one frame of latency remains; the swimming does not.

**Following a player is intolerable without free look.** The demo camera sits in a player
and rotates with their aim, so the viewer's head is dragged around by someone else. Added
`mirv_vr_freelook`: with it on, only the *position* comes from the demo and the
orientation is the headset's alone. `mirv_vr_recenter` aligns the room's forward with the
map's.

That gives the two modes worth having: sit in the player and see what they see, or ride
along with them and look where you like.

## Operating it

Everything is on F-keys, loaded at startup by `+exec vr`, because with a VR runtime up the
game window stops taking input reliably and console text cannot be synthesised at all
(see `../workflow.md`). `scripts/send-key.ps1` drives them from outside.

| key | |
| --- | --- |
| F9 / F5 | headset on / off |
| F6 / F7 | free camera on / off — off returns to the demo's own camera |
| F8 / F10 | free look on / off |
| F1 | recenter free look on the demo camera's forward |
| F4 | pause the demo |
| F2 / F3 | slow motion / normal speed |
| F11 / F12 | mouse slower / faster |

Mouse sensitivity is set to 0.05 and keyboard to 0.3 at startup, far below the defaults.
On a monitor a fast camera is merely fast; in a headset the same motion is read by the
inner ear as the world lurching, and it makes people ill in seconds.

## Still open

- **Performance is unmeasured.** Four scene traversals per stereo frame, and the pass loop
  still starts one pass more than it uses. This is phase E and remains the project's main
  risk.
- One frame of pose latency, by construction.
- The HUD is drawn into both eyes at screen depth.
- Per-eye resolution is capped by the window, as above.
