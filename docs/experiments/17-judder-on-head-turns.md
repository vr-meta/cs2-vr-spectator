# 17 — Judder on head turns, and why it was not the frame rate

**The report.** "Камера прям сильно дрожит при поворотах" — the picture jerks when the
head turns. Separately, and earlier, "чуть растягивается мир при повороте" — it pulls a
little.

**The obvious answer is wrong.** At 30–40 frames a second the runtime's reprojection
re-aims every displayed frame to the head's current pose, and that correction is *exact* —
provided the pose in `XrCompositionLayerProjectionView` really is the pose the image was
rendered from. A head turn should not judder at 30 fps any more than at 90. If it does,
the number being reported is wrong.

## What was wrong with it

`MirvVrXr_RenderThread_SubmitEye` read `g_RenderedViews`, a global, at submission time.
The engine thread writes that global when it decides what the next frame will be rendered
with. Source 2's render thread runs behind the engine thread — and the amount varies from
frame to frame.

Measured, once the instrumentation existed:

```
AFXVR:   render thread 0.45 frames behind (worst 1)
AFXVR:   render thread 0.02 frames behind (worst 1)
AFXVR:   render thread 0.00 frames behind (worst 0)
AFXVR:   render thread 0.12 frames behind (worst 1)
```

A fraction means *some* frames in that two-second window were submitted after the engine
thread had already moved on, and some were not. One frame out at a hundred degrees a
second is 2.8° of reported pose that never happened; the runtime dutifully reprojects by
the difference. Wrong by a *constant* amount is drag — the world lags the head and catches
up. Wrong by a **different amount every frame** is judder, and no frame rate fixes it,
because frame rate is not what is broken.

Worse, the two halves of the pose were published in separate lock scopes — positions after
the locate, orientations after the eye loop — so a reader arriving between them got half of
one frame and half of another.

## The fix

The frame carries its own pose. Each pass is queued on the engine thread, in patch 002, at
exactly the moment the engine thread knows what that pass is being rendered with; the
submit lambda captures a ticket by value there. The ticket names a complete entry in an
eight-deep ring, written under one lock in one go. The render thread reads only that.

The same race ran through the timing. In low-latency mode the engine thread overwrites
`g_FrameState` with the next frame's `xrWaitFrame` result while the render thread is still
between `xrBeginFrame` and `xrEndFrame`, so the display time a frame was *ended* with could
belong to the following one. The render thread now takes its own copy at the top of a
frame, and in low-latency mode takes the predicted time from the ticket.

Note what the fix does **not** do: the skew is still there, and the instrumentation still
reports it. What changed is that the skew no longer matters, because what is reported is
what was rendered however late it is submitted.

## Worn

> "Подергивания вообще нет, выглядит прям хорошо."

28–33 frames a second at 2528×2780 per eye, session FOCUSED, lag still varying between
0.00 and 0.45 frames — and no judder.

## Two things that had to be right for this to be visible

**The field of view must not be written once a frame.** The head pose is now written at the
view-setup trampoline so that the audio listener and the client's matrices see the viewer
(see the commit "the head belongs to the frame, not only to the passes"). The first version
of that write carried the eye's field of view along with the origin and angles. It made the
picture measurably worse from the moment it shipped — which is a finding in itself: the
engine derives something from that field once a frame that reaches the eye image. The
trampoline now writes origin and angles only.

**SteamVR must not be running.** A session opened while it is up comes up, reports itself
running, and then gets `xrWaitFrame` averaging 131 ms with one wait of 606 ms, 3.7 frames a
second, and finally a CS2 render thread parked inside the wait and a game that stops
responding. On a Quest over Link, SteamVR is not an alternative to the Oculus runtime — it
is a client of it, and while it is up it is the application the compositor schedules.

That cost an afternoon, because from the outside it is indistinguishable from whatever
shipped last being broken. It was attributed in turn to the runtime selection, the frame
cap, the window size, the quad layers and the frame ticket, and it was none of them. Now:
`scripts/start-vr.ps1` refuses to launch while SteamVR is running, the hook looks for
`vrserver.exe` when a session is created and says what it means, and the frame rate line
carries the session state whenever it is not `FOCUSED`.

## Still open

The world looks slightly small and far, and the fov question behind it: the crop hands the
runtime a sub-rectangle computed from the angle we believe the eye passes rendered, and
nobody has measured what they actually render. `mirv_vr_fovraw` prints what the engine left
in the fov field by the time a pass begins, which settles the convention; `vr_diag.cfg`'s
left and right arrows compare `mirv_vr_fov 120` against `150`, which a correct pipeline is
invariant to.
