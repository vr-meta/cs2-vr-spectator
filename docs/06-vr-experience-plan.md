# Plan for the VR spectator experience

Date: 2026-09-19, written once frames were reaching the headset and the obvious next
questions all turned out to be the same question: **what is the viewer actually holding,
and what can they reach from inside the headset?**

Phases A–D of [`04-plan.md`](04-plan.md) answered "can CS2 render into a Quest". They can.
This plan is about making it usable. It supersedes nothing there; it is what comes after.

> **Later, 2026-09-19.** Most of what follows has now been built or measured, and two of
> the assumptions in it turned out to be wrong. Read
> [`experiments/11`](experiments/11-seeking.md) through [`15`](experiments/15-hud-per-eye.md)
> and the GitHub issues before acting on anything below. In brief: seeking is safe and the
> timeline never crashed anything; Panorama accepts synthetic mouse input, which is the
> pointer problem solved at the far end; the UI really is composited once per pass, and both
> `mirv_vr_xr ui out` and `mirv_vr_panel` now exist; the name tags cannot be fixed by asking
> the engine to recompute; and the frame budget is more than half submission rather than
> rendering, so the graphics settings this plan expected to help were already at minimum.

## What works today

- Stereo at the runtime's full 2528x2780 per eye, head tracked, from a playing demo.
- Two ways to sit in the scene: in the player seeing what they see, or riding along with
  the head free (`mirv_vr_freelook`).
- Controller sticks fly the viewer; buttons toggle free look, recentre, and pause.
- 38–48 frames/s where the headset wants 72 — [`experiments/10-frame-budget.md`](experiments/10-frame-budget.md).

## What is wrong, and why

Three complaints from the first real session in the headset, and they share one root.

**Player name tags drift off the players.** They are placed using the projection computed
once for the main pass; each eye renders from a different camera, so the tags land in the
wrong place in both. Currently worked around by turning the HUD off.

**The demo's menu — timeline, seeking, speed — is unreachable.** It is a flat Panorama
overlay drawn into the back buffer at screen depth. In stereo it is doubled, wrongly
placed, and there is no pointer to click it with.

**Switching players did not work.** `spec_next` and `spec_prev` exist as commands but do
nothing during demo playback; the on-screen hints say MOUSE1 and SPACE because the demo
listens for game *actions*. Fixed by sending `+attack` / `-attack` a frame apart.

The root is that **everything two-dimensional in CS2 assumes one camera and a screen**.
Anything that is going to be readable in a headset has to leave the back buffer.

## The shape of the answer

OpenXR composites more than one layer. A projection layer carries the world; a **quad
layer** carries a flat image placed in space — exactly what a menu wants to be. HLAE
already separates the UI from the scene: its render command queue has a `BeforeUi` hook
that fires with the scene finished and the UI not yet drawn.

So:

- **Scene into the projection layer**, captured at `BeforeUi` rather than `BeforePresent`,
  so no UI is baked into the eyes.
- **UI into a quad layer**, captured after it is drawn, and placed at a comfortable
  distance in front of the viewer — head-locked or world-locked, whichever reads better.

That gives a readable timeline and scoreboard without fighting the engine, and it is the
same mechanism every VR application uses for menus.

Name tags are a separate problem and a harder one: they are world-anchored but drawn in
screen space, so no flat panel can fix them. Either they are rendered per eye with that
eye's projection — which means finding where the client places them — or they are left
off. Off, for now.

## Order of work

Ranked by what the viewer notices, with the cheap and certain before the deep and
uncertain.

### 1. Controls the viewer can learn (small, immediate)

The mapping has accreted one button at a time and needs one pass to become coherent. It
also needs to be discoverable from inside the headset, which today it is not: the only
record of it is a comment in a cfg file.

- Settle the scheme: sticks move and turn; triggers change player; grip changes camera
  mode; face buttons for free look, recentre, pause.
- **Seeking on the sticks**, which is the thing most obviously missing: a click or a hold
  to jump the demo back and forward. `demo_gototick` is the command; note that dragging
  the timeline crashed the game early on ([`workflow.md`](workflow.md)), so seeking needs
  testing on its own before it is bound to anything.
- A way to see the mapping without taking the headset off — which needs item 2.

### 2. The UI as a quad layer (the real fix)

- Capture the scene at `BeforeUi` so the eyes are clean.
- Capture the composed UI separately.
- Submit it as `XR_TYPE_COMPOSITION_LAYER_QUAD`, sized and placed for reading.
- Decide head-locked versus world-locked. Head-locked is easier and usually worse; a panel
  that stays where you left it is more comfortable to look away from.

This also gives somewhere to put our own text — the control mapping, the frame rate, which
mode is active.

### 3. Frame rate (known problem, measured)

Nothing here is new; see experiment 10. In order: the game's graphics settings, which have
never been touched and are still at their defaults; then resolution; then dropping the
main pass while the headset is active, which is the only structural saving left.

Worth doing before item 2, if the quad layer turns out to cost anything meaningful.

### 4. Seeking and playback, properly

The demo timeline is the reason to have a menu at all. Once there is a quad layer to draw
on and controls to drive it:

- Jump by a fixed interval, by round, and to an absolute tick.
- Confirm whether seeking still crashes the game, and whether that depends on HLAE.
- Playback speed from the controller rather than an F-key.

## Open questions worth answering before building

- **Does `demo_gototick` work reliably?** The timeline drag crashed the game on day one.
  If seeking is fragile, the menu design changes.
- **What does a quad layer cost?** If it forces an extra copy of the UI every frame, it
  has to be weighed against item 3.
- **Where does the client place name tags?** If that is findable, per-eye tags become
  possible and the HUD stops being all-or-nothing.
