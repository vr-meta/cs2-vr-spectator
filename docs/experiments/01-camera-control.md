# Experiment 01 — camera control via mirv_input

**Build:** CS2 2000908 / 1.41.8.1 / rev 10981323.
**Run:** 2026-09-18, HLAE v2.192.2, demo `pro_mirage.dem` paused.
**Question:** with `cl_demo_view_offset_left` confirmed dead (experiment 00), is there a
working way to move the camera at all?

## Result: yes

`mirv_input` reads and writes the camera directly:

```
mirv_input position <x> <y> <z>   - Set new position, use * where you don't want changes.
  Current value: -452.449768 -2196.836182 -115.380615

mirv_input angles <yPitch> <xRoll> <zYaw>
  Current value: 0.459993 46.227703 0.000000

mirv_input fov [real] <fov>
  Current value: 90.000000
```

It requires `mirv_input camera` to enable the override first.

Measured on the paused demo, moving 40 units (about a metre) along world X:

Second run, with a return to baseline after *every* move, to rule out drift:

```
base -> x+40 -> base -> x-40 -> base
```

| comparison | mean diff | max channel | pixels changed |
| --- | --- | --- | --- |
| base → x +40 | 146.0 | 695 / 765 | 96.3% |
| base → x −40 | 165.7 | 713 / 765 | 96.5% |
| base¹ vs base² | 0 | **0** | 0% |
| base¹ vs base³ | 0 | **0** | 0% |
| base² vs base³ | 0 | **0** | 0% |

The three control rows matter more than the two measurements. All baseline captures are
bit-identical, so the camera is not merely moving, it is **positionable and repeatable** —
which is exactly what a VR eye offset needs, since the two eyes must differ by a precise
amount and by nothing else.

This also rules out the obvious confound. `mirv_input camera` mode lets the mouse fly the
camera, so a stray mouse movement could have produced the difference. But the mouse
changes *angles*, while the command sets only *position* — any mouse input between
captures would have left the baselines disagreeing with each other. Three identical
returns say nothing changed except what was commanded.

Visually the shift shows true parallax: a crate a few metres away moves a lot and
changes size, while buildings across the site barely move. That is a camera translating
through space, not an image being panned.

## What this replaces

Experiment 00 killed the engine's own eye offset. `mirv_input` takes its place:

| need | mechanism |
| --- | --- |
| render the scene twice from one simulation state | HLAE multi-pass (note 02) — code exists |
| move the eye between the two renders | `mirv_input position` — **confirmed working** |
| per-pass rather than per-frame application | **still unknown — the next test** |

## Still unknown, and it is the whole question

`mirv_input` was tested per *frame*: set a position, look at the resulting image. Stereo
needs the position to change between the two *passes* of a single frame. Nothing here
shows that is possible.

Two ways it might work, both untested:

1. `mirv_streams` per-stream `beforeCommands` run before each pass, and `mirv_input
   position` is a command. If it takes effect inside the pass loop, stereo follows
   almost immediately.
2. Otherwise a per-pass camera hook has to be added to HLAE's pass loop — the structure
   is understood (note 02) and the licence is MIT.

## Method notes

Two rounds of measurement were thrown away before this one, both for the same reason:
**something other than the thing under test was different between captures.**

- Round 1: the in-game console was open and covering the scene, so the F-keys went to
  the console instead of the game. Offsets appeared to do nothing.
- Round 2: in fullscreen, bringing the window forward for capture lost focus entirely —
  one screenshot captured the desktop terminal instead of the game, which showed up as a
  spurious "100% of pixels changed".

Both were caught by looking at the images rather than trusting the numbers. The fix that
made the measurement trustworthy was the control row: capture the baseline, move away,
move back, and require the return to be pixel-identical. A test that cannot reproduce
its own starting point is not measuring what it thinks it is.

Practical consequences for later runs: **windowed, console closed, game window focused,
and always include a return-to-baseline capture.**
