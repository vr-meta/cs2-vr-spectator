# 11 — Seeking in a demo: does it work, and does it still crash?

**Question.** [Issue #4](https://github.com/vr-meta/cs2-vr-spectator/issues/4) held three,
and they had to be answered before a seek control went on a controller button:

1. Does `demo_gototick` work reliably during playback, with the hook attached?
2. Does seeking backwards behave differently from forwards?
3. Does it survive being done repeatedly, the way someone scrubbing would?

Behind them was a fourth, from day one of this project: **dragging the timeline slider
crashed the game** with an access violation, and nothing since had established whether
that was the hook's doing or the game's.

**Answer, short.** Seeking works, in both directions, paused or playing, and survived
twenty-three seeks in five minutes without complaint. The direction is not what costs —
the distance back to the preceding keyframe is. And the timeline slider did not crash
anything: it took synthetic mouse input, seeked, and kept going.

## How it was run

No headset. This is the one part of the project that can be tested without one, which is
why it was worth doing on its own.

```powershell
scripts\launch-cs2-experiment.ps1 -SelfBuilt -Demo pro_mirage.dem -Width 1280 -Height 720 `
    -FpsMax 120 -ExecCfg exp11_seek
```

`scripts/cs2/exp11_seek.cfg` puts every case on a function key and
`scripts/send-key.ps1` presses them from outside. Everything goes through
`mirv_vr_seek now <seconds>`, which is the same code a controller trigger runs — testing
`demo_gototick` directly would have answered a question nobody asked.

`scripts/drag-in-cs2.ps1` is new, for the slider: it synthesises a press, a path and a
release in the game's client coordinates.

The demo is a 42-minute GOTV recording of a professional match on Mirage, 64 ticks a
second (`0.0156 s per tick`, reported by `mirv_vr_seek where`).

## 1. It works

Eighteen seeks through `mirv_vr_seek`, forwards and backwards, 10 seconds to 60 and one
of ten minutes. Every one landed where it was asked to:

```
AFXVR: seek +10.0s, tick 3494 -> 4134
AFXVR: seek -10.0s, tick 5422 -> 4782
AFXVR: seek +60.0s, tick 6346 -> 10186
AFXVR: seek -60.0s, tick 10414 -> 6574
```

Seeking before the start is clamped to tick 1 by the hook, and the engine takes it:

```
AFXVR: seek -100000.0s, tick 7034 -> 1
[Demo] Demo Skipping: skipping to demo tick 1 (game tick 52832) from full packet 0
[Demo] Demo Skipping finished at tick 1
```

Seeking **while paused** works in both directions and lands exactly on the requested tick
— `7048` out, `6408` back, no drift. The menu can therefore pause and scrub, which is what
anyone studying a round will want to do.

## 2. Direction is not the thing that costs

This is the part worth knowing, and it is not what the issue assumed.

The engine logs how it gets there, and there are **three** shapes, not two:

```
# forwards, from where we already are -- no rewind at all
[Demo] Demo Skipping: skipping forward to demo tick 4591 from current 3951

# backwards, or forwards across a keyframe: rewind to a full packet, then replay
[Demo] Demo Skipping: skipping to demo tick 3922 from full packet 3840 (56671)
[Demo] Demo Skipping: skipping to demo tick 3404 from full packet 0 (52831)
```

Full packets in this demo are every **3840 ticks — exactly 60 seconds**. Seeking backwards
means rewinding to the full packet at or before the target and replaying forward from
there. So the cost is not the size of the jump and not its direction; it is **how far the
target sits past the preceding keyframe**:

| seek | replayed from | ticks replayed | felt |
| --- | --- | --- | --- |
| −10 s to tick 3922 | full packet 3840 | 82 | instant |
| −10 s to tick 3404 | full packet 0 | 3404 | about a second |
| +60 s to tick 7272 | full packet 3840 | 3432 | about a second |
| +10 s to tick 4591 | current position | 640 | instant |

A ten-second step backwards can therefore cost more than a sixty-second step forwards,
which is entirely counter-intuitive from the outside and completely explicable from the
inside. The worst case is bounded: never more than 3840 ticks, one minute of replay.

And it does not freeze the process. The engine chunks the work:

```
[Demo] Demo Skipping paused after 2500 messages, tick 2499 start 0 goal 3404
[Demo] Demo Skipping flushing last 905 messages, tick 3404 start 2499 goal 3404
```

Two frames, not one long stall — which matters a great deal in a headset, where a frozen
main loop stops frames reaching the compositor.

## 3. It survives being used

Six seeks issued inside one frame (the hook coalesces those into one, deliberately), then
twelve separate seeks a quarter-second apart alternating forwards and backwards, then five
full-width drags of the timeline slider. Twenty-three demo skips in total.

No crash, no access violation, nothing in `console.log` beyond the ordinary. The game was
still rendering at the end, and a screenshot confirms it — players, HUD, timeline, all
where they should be.

## 4. The slider does not crash the game

The day-one crash **could not be reproduced**. That is a weaker statement than "it does not
happen", and it should stay weaker: the original was seen once, months of work ago, on an
earlier game build and an earlier state of this project.

What was established instead:

- **Panorama accepts synthetic mouse input.** A click at 74% along the track jumped the
  demo from 2:25 to 31:35 of 42:06. This is worth recording next to the opposite finding
  in `workflow.md` — synthetic *typed text* never reaches Panorama, and it was reasonable
  to assume the mouse would be the same. It is not.
- **The slider seeks on release, not continuously.** Five full-width drags, each forty
  mouse moves, produced two demo skips between them. So dragging a scrub bar is not the
  thousand-seeks-a-second stress it looks like, and the fear that shaped this issue was
  larger than the thing.
- **Round stepping works and lands on round boundaries.** The demo UI has `|◀ Round: 0 ▶|`
  buttons beside the timeline, and clicking one jumped cleanly to the next round.

## What this changes

**The controls.** Seeking is now on the triggers — left back, right forward, ten seconds a
press, `mirv_vr_seek` to change it. There is no reason to hold it back.

**The menu, when #2 builds it.** CS2's own demo UI offers ±15 s, round stepping, speed and
a scrub bar — in that order of prominence. It is a reasonable design, arrived at by people
who watch a lot of demos, and the VR menu should start by copying it rather than inventing
something. Round stepping in particular is the control that does what a viewer actually
wants ("show me the next round"), and fixed jumps are kinder than a scrub bar to a hand
holding a controller in the air.

**One thing not answered.** Whether there is a console command behind the round buttons.
If there is, round stepping belongs on the controllers directly and not only in a menu.
Finding it means dumping the cvar list and looking, which is cheap and was out of scope
here.

## Evidence

`scripts/cs2/exp11_seek.cfg`, and screenshots
`exp11_after_scrubbing.png`, `exp11_after_slider_drag.png`, `exp11_after_track_click.png`,
`exp11_round_step.png`.
