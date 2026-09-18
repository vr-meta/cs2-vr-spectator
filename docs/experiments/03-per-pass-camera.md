# Experiment 03 — can the camera differ between passes?

**Build:** CS2 2000908. **Run:** 2026-09-18, self-built `AfxHookSource2.dll`,
`pro_mirage.dem` paused.
**Question:** phase B of the plan. Two streams, identical except for camera position —
do they render from different viewpoints?

## Result: no. The camera is fixed by the time a pass runs.

```
mirv_input camera
mirv_streams edit eyeL beforeCommands add mirv_input position -492.449768 -2196.836182 -115.380615
mirv_streams edit eyeR beforeCommands add mirv_input position -412.449768 -2196.836182 -115.380615
```

80 units apart — about two metres. Output:

| frame | mean diff | max channel |
| --- | --- | --- |
| 00000 | 0 | 0 |
| 00005 | 0 | 0 |
| 00020 | 0 | 0 |
| 00060 | 0 | 0 |
| 00100 | 0 | 0 |
| 00148 | 0 | 0 |

Byte-identical, across the whole recording. Later frames were checked in case the change
applied a frame late; it does not apply at all.

### The commands did run

This is not a case of the setting failing to register or the command being rejected:

- Both entries printed back correctly from `beforeCommands print`, negative coordinates
  intact.
- **No `AFXWARNING: "..." to be set / executed is not a command or cvar`** in the log.
  `ExecuteCommands` emits that whenever a name resolves to neither, so its absence means
  `mirv_input` was found and dispatched.
- `mirv_input position` demonstrably moves the camera when used per frame
  (experiment 01), so the command itself works.

The command executes and changes nothing, which means **the view for the frame is already
resolved before the per-pass commands run.** `mirv_input` feeds a camera override that is
read earlier in the pipeline than the point where a pass begins.

This is exactly the risk recorded in `04-plan.md` for phase B.

## Correcting an earlier claim

Note 02 and the experiment 00 results stated that `beforeCommands` accepts **convars
only**, and that `mirv_input` could not be placed there. **Both parts were wrong.**

`CAfxStreams::ExecuteCommands` tries `FindConVar` first, and falls back to `FindCommand`
plus `DispatchConCommand` when the name is not a convar. Concommands are supported.
The claim came from reading the first half of the function and stopping at the convar
switch statement.

The supporting "evidence" was equally faulty: `mirv_input` was reported as absent from
the printed command list, but it had been there all along — the log search used the
pattern `mirv_input position`, while the log records `"mirv_input" "position"` with
quotes, so the line never matched.

Two independent mistakes agreeing with each other produced a confident wrong conclusion.
The correction does not change the outcome — per-pass camera still does not work — but it
changes *why*, and therefore what the fix is.

## What this means for phase B

The fix is no longer "allow concommands in `beforeCommands`" (they are already allowed).
It has to reach the point where a pass sets up its view:

- Find where the per-pass render obtains its camera, downstream of the `mirv_input`
  override that experiment 01 exercises.
- Apply a per-stream offset there, so each pass renders with its own eye position.

That is inside the render path rather than the command layer, which makes it the more
invasive of the two options originally sketched — and the cheaper one is now ruled out.

## Method note

The check that made this trustworthy was looking for the *absence* of a specific warning.
"The command did nothing" and "the command was never executed" produce identical images;
`AFXWARNING` distinguishes them, and it is absent. Without that, this experiment would
have been another ambiguous null result like the three before it.
