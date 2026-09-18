# Experiment 04 — per-pass camera, in code

Date: 2026-09-18. Self-built `AfxHookSource2.dll`, CS2 build 2000908, `pro_mirage.dem`
paused, windowed 1280x720.

This is phase B of [`../04-plan.md`](../04-plan.md). It answers the open question left by
[`../05-view-setup-point.md`](../05-view-setup-point.md) and then solves it.

## Question 1 — how often does the view setup trampoline run?

Instrumented the hook: a frame counter and a pass counter maintained by the render pass
loop, plus log lines at every `OnClientOutput` enter/leave, every pass boundary, and
every call of `CS2_Client_CSetupView_Trampoline_IsPlayingDemo`. Armed for three frames
with `mirv_vr_log 3` while recording two streams.

The trace, one frame, verbatim except for timestamps:

```
frame=42400 BeginMainRenderPass
frame=42400 pass=0 OnClientOutput enter
frame=42400 pass=0 OnClientOutput leave
frame=42400 BeginNextRenderPass -> pass=1
frame=42400 pass=1 OnClientOutput enter
frame=42400 pass=1 OnClientOutput leave
frame=42400 BeginNextRenderPass -> pass=2
frame=42400 pass=2 OnClientOutput enter
frame=42400 pass=2 OnClientOutput leave
frame=42400 BeginNextRenderPass -> pass=3
frame=42400 pass=3 OnClientOutput enter
frame=42400 pass=3 OnClientOutput leave
frame=42400 pass=3 SetupView this=... org=(-1656.00,-1976.00,-203.46) ...
```

**Answer: once per frame, and outside the pass loop entirely.** The view setup is not
part of `OnClientOutput`; it runs after all four passes have rendered, as part of the
client's frame update, and what it produces is consumed by the *next* frame's passes.

So the trampoline cannot be the place where eyes diverge. It is called once and all
passes share its result — the expensive answer that note 05 flagged as the risk.

Two things fell out of the same trace:

- **Two streams cost four scene traversals, not three.** The pass loop always begins one
  more pass than it needs; `RenderServiceHooks.cpp` admits this in a comment
  ("We are wasteful here ..."). It costs a whole extra render of the scene per frame and
  is worth removing before phase E measures anything.
- The 3 log lines for 3 armed frames are themselves the measurement. Had the trampoline
  run per pass, there would have been 12.

## Question 2 — is the view re-read between passes?

The trampoline receives a `CViewSetup` pointer. If that object is persistent rather than
a stack temporary, and if each pass reads it again, then writing an eye offset into it
from the pass loop would move the camera per pass.

Logged the pointer: `00007FFB59E90640`, identical on every frame and inside a module's
data, not on a stack. So: persistent.

Then wrote into it from `EngineThread_BeginNextRenderPass`, before each pass renders —
base origin plus an offset along the view's right vector, `-ipd/2` for pass 1 and
`+ipd/2` for pass 2:

```
frame=36778 pass=0 wrote org=(-336.29,-2175.06,-110.76)
frame=36778 pass=1 wrote org=(-336.29,-2165.06,-110.76)
frame=36778 pass=2 wrote org=(-336.29,-2185.06,-110.76)
frame=36778 pass=3 wrote org=(-336.29,-2175.06,-110.76)
```

## Result

Two takes of the same paused frame, compared byte-wise (`scripts/compare-tga.ps1`), on
the final build:

| take | `mirv_vr_eyes` | pixels differing between eyeL and eyeR |
| --- | --- | --- |
| control | 0 | **0.00 %** — bit-identical, all three frames |
| test | 20 units | **88.89 %**, mean abs diff 23.1 |

The control is the important half: with the mechanism present but the separation zero,
the two passes produce the same image, to the byte. Whatever the test shows is therefore
the offset and not the machinery.

Looking at the pair confirms it is a translation and not damage: same frame, same round
timer, same HUD, and the parallax is correct — the near buildings shift far more than the
tower and the skyline behind them, and they shift *left* in the right eye, which is the
direction a camera moved right produces. The HUD does not shift at all, being screen
space.

(The first build measured 0.2–0.4 % on the control and 98.97 % on the test, on a
different viewpoint. The residual control difference was the drift described below,
leaking a stale offset into one pass; it is 0.00 % once that is fixed.)

**Per-pass camera works.** Phase B is answered: not in the view setup trampoline, but in
the pass loop, by rewriting the persistent `CViewSetup` before each pass.

## The sharp edge

The trampoline reads the current contents of `CViewSetup` as "the game's camera". Our
pass writes land in the same fields, so an offset left behind by the last pass would be
read back as the game camera on the next frame and accumulate.

It did not happen here only by luck: the wasted trailing pass has no eye assigned, so it
writes the base back. With a single stream the trailing pass *is* an eye and the drift
would be real.

Fixed by restoring the base at the top of the trampoline before reading, guarded by a
dirty flag and a pointer match, rather than relying on the pass layout. The evidence that
it worked: the logged base origin is `(-99.726929, 5.380821, 52.643745)` on every armed
frame of both takes — identical to the last decimal with the offset on and off — and the
control take went from 0.2–0.4 % to 0.00 %.

## What this does not answer

- Passes render at window resolution, not per-eye resolution (note 03).
- Only position is offset. Head tracking needs angles too, and per-eye projection
  matrices, which are built in `New_CViewRender_UnkMakeMatrix` — also once per frame.
- The HUD is drawn identically in both eyes at screen depth. Fine for a file; wrong in a
  headset.
