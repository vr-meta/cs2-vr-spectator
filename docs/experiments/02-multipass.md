# Experiment 02 — is HLAE's multi-pass rendering real?

**Build:** CS2 2000908 / 1.41.8.1. **Run:** 2026-09-18, HLAE v2.192.2, `pro_mirage.dem`.
**Question:** note 02 claimed from source reading that HLAE re-renders the whole scene
several times per frame. Does it, and can the passes be made to differ?

## Result: yes, the passes are genuinely independent

Two streams were defined and recorded simultaneously:

```
mirv_streams add normal eyeL
mirv_streams add normal eyeR
mirv_streams edit eyeR worldAction noDraw
mirv_streams record start / end
```

Output: `<take>/eyeL/00000.tga` and `<take>/eyeR/00000.tga`, 94 frames each, same frames.

| comparison | mean diff | max channel | pixels changed |
| --- | --- | --- | --- |
| eyeL vs eyeR (same frame) | 49.4 | 243 / 765 | 74.1% |

`eyeL` shows the full scene. `eyeR` shows players, weapons, particles and smoke floating
against an empty sky — the world geometry is simply absent. Two different renders of one
simulation state, written to disk side by side.

**This is the load-bearing finding of the project so far.** The expensive half of stereo —
making the engine traverse the scene twice from one frozen state — already exists,
works on the current build, and is MIT licensed.

## The remaining gap: per-pass camera

The passes differ, but only in what HLAE's own stream settings control. The camera is
not among them.

`beforeCommands` accepts **convars only**. Confirmed both ways:

- In the source, `CAfxStreams::ExecuteCommands` resolves each entry through `FindConVar`
  and assigns to the convar's value.
- In practice, `mirv_streams edit eyeR beforeCommands add r_drawviewmodel 0` registered
  and printed back, while `... add mirv_input position <x> <y> <z>` did not appear in
  the list at all.

And the one convar that would have moved the eye — `cl_demo_view_offset_left` — is dead
(experiment 00). So there is currently no convar worth putting there.

| need | status |
| --- | --- |
| render the scene twice from one simulation state | **works** (this experiment) |
| move the camera exactly and repeatably | **works** (experiment 01, `mirv_input`) |
| move it *per pass* rather than per frame | **missing — needs a code change** |

## What the code change looks like

From note 02, the pass loop is in `RenderServiceHooks.cpp`, and per-pass state is applied
in `CStream::EngineThread_BeginFrame` via `ExecuteCommands`. A per-pass camera means
either:

1. Extending the stream settings with a camera offset applied in the same place the
   convar commands are applied, or
2. Allowing concommands in `beforeCommands`, so `mirv_input position` can be used
   directly — probably simpler, and useful beyond this project.

Either is a contained change in understood, MIT-licensed code. This is the first point
in the project where writing code, rather than configuring existing tools, becomes
necessary.

## Two false negatives on the way here

Both looked exactly like "the mechanism does not work", and neither was.

**Wrong indicator.** The first attempt used `r_drawviewmodel` to tell the passes apart.
The convars registered correctly — the log showed each stream holding its value — but the
spectator view has no weapon model on screen, so there was nothing to change. Output:
two identical images.

**Wrong value, silently.** The second attempt used `worldAction hide`. Valid values are
`draw|noDraw|zOnly`. An invalid value does not raise an error: the command prints its
help text and leaves the setting untouched. Output: two identical images again.

A correct mechanism with a bad probe is indistinguishable from a broken mechanism. The
habit that eventually worked: **choose an indicator whose absence is unmissable** — the
entire map disappearing cannot be mistaken for anything else — and **read back what the
command actually did** rather than assuming it took effect.
