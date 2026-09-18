# 14 — When is the UI drawn, and is it inside the eyes?

**Question.** [Issue #2](https://github.com/vr-meta/cs2-vr-spectator/issues/2) states that
the demo's playback UI "is a flat Panorama overlay drawn into the back buffer at screen
depth. Submission copies that back buffer into each eye, so in stereo the UI is doubled."
[Issue #3](https://github.com/vr-meta/cs2-vr-spectator/issues/3) is about the
world-anchored HUD landing in the wrong place.

Both rest on the same thing nobody had checked: **is the UI composited once a frame, after
the passes, or once per pass?** If once a frame, the eyes cannot contain it and the premise
of #2 is wrong. If once per pass, they do, and moving the capture earlier is available as a
fix.

**Answer: once per pass.** The premise holds, and the fix works.

## Watching it without a headset

The eye passes only render while an OpenXR session is running, which normally means a Quest
on someone's head before anything can be observed. `mirv_vr_xr passes 2` now forces them
with no session and no runtime — the passes render, the poses are written, and nothing is
submitted anywhere, because every submission call needs a session.

That, plus probes on the two capture points, makes the whole pass loop readable from a
desk.

```powershell
scripts\launch-cs2-experiment.ps1 -SelfBuilt -Demo pro_mirage.dem -ExecCfg exp14_uiorder
scripts\send-key.ps1 -Key F2   # force two extra passes
scripts\send-key.ps1 -Key F4   # mirv_vr_log 3
```

## What a frame looks like

```
frame=12810 BeginMainRenderPass
frame=12810 pass=0 OnClientOutput enter
frame=12810 pass=0 OnClientOutput leave
frame=12810 BeginNextRenderPass -> pass=1
frame=12810 pass=1 eye org=(-99.73, 6.63, 52.64) ang=(0,0,0) fov=90
frame=12810 pass=1 OnClientOutput enter
frame=12810 pass=1 OnClientOutput leave
frame=12810 pass=1 BeforeUi       (render thread)
frame=12810 pass=1 BeforePresent  (render thread)
frame=12810 BeginNextRenderPass -> pass=2
frame=12810 pass=2 eye org=(-99.73, 4.13, 52.64) ang=(0,0,0) fov=90
frame=12810 pass=2 OnClientOutput enter
frame=12810 pass=2 BeforeUi       (render thread)
frame=12810 pass=2 BeforePresent  (render thread)
frame=12810 pass=2 BeforeUi       (render thread)
frame=12810 pass=2 BeforePresent  (render thread)
```

Three passes, three `BeforeUi` and three `BeforePresent`, each pair in order. (The pass
number on the render-thread lines is whatever the engine thread has reached by then, not
the pass being presented — the two threads are a pass apart. The count is what matters.)

So the UI is composited into **every** pass, immediately before that pass is handed over.
Capturing at `BeforePresent`, which is where this project has always captured, takes the
image with the HUD and the demo menu already in it. In stereo that is a flat overlay at
screen depth, drawn twice, at the wrong distance and with no depth of its own — exactly
what issue #2 describes.

The two eye positions in that log differ by 2.5 units on Y, which is the 63 mm
interpupillary distance, so the passes are the eyes and not something else.

## The fix, and it works

`mirv_vr_xr ui out` pushes the eye capture onto the `BeforeUi` queue instead of
`BeforePresent`. `ui in` puts it back. It takes effect on the next frame; the session does
not need restarting.

Verified by making the capture announce itself:

```
=== default (ui in) ===
AFXVR: eye 0 captured at BeforePresent
AFXVR: eye 1 captured at BeforePresent

=== after mirv_vr_xr ui out ===
AFXVR: eye 0 captured at BeforeUi
AFXVR: eye 1 captured at BeforeUi
```

Both eyes, every frame, no crash.

**A note on the probe that was nearly believed.** The first version of it reported whether
the `BeforePresent` queue was empty, on the theory that with the capture moved it would be.
It never was — HLAE queues its own work there. That would have read as "the switch does not
work" and it would have been wrong. The capture now reports from inside itself, where there
is nothing to misread.

## Why it is not the default

Because it removes the demo menu from the headset entirely, and there is nothing yet to put
it back. A menu in the wrong place is more use than no menu at all.

Turning it on is the first half of issue #2. The second half is a
`XR_TYPE_COMPOSITION_LAYER_QUAD` carrying the UI as a panel in space, and the experiment
that unblocked it is [11](11-seeking.md): **Panorama accepts synthetic mouse input**, so a
controller ray mapped through the quad into window coordinates can click the timeline. That
was the part with no known answer.

## What it does not fix

Issue #3. Name tags and health numbers are world-anchored: they are positioned with a
projection computed once per frame for the main pass, so they are wrong in an eye no matter
which capture point is used, and no flat panel can carry them. They have to be drawn per
eye with that eye's projection, or not at all. `cl_drawhud 0` remains the only answer for
now.
