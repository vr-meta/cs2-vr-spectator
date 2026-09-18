# Experiment 00 — results

**Build:** CS2 ClientVersion 2000908, PatchVersion 1.41.8.1, SourceRevision 10981323,
dated 2026-09-09.
**Run:** 2026-09-18, RTX 4070 Laptop (driver 32.0.15.9144), D3D11 backend, 1280x720.
**Tooling:** HLAE v2.192.2, `AfxHookSource2.dll` built 2026-09-12, injected via `-customLoader -noGui -autoStart`. CS2 launched with `-insecure`, fullscreen.

The convar checks and the offset sweep are complete. The two-pass test (step 6) is not
yet run.

## Setup notes worth keeping

`mirv_cvar_unhide_all` reported:

```
==== Cmds total: 1215 (Cmds unhidden: 501) ====
==== Cvars total: 4023 (Cvars unhidden: 1929) ====
```

Confirming HLAE was genuinely attached, not merely loaded into the process.

**CS2 has no `con_logfile` and no `version` command.** Both are Source 1 and were used
in the first draft of the configs. Console capture is done with the `-condebug` launch
flag, writing `game/csgo/console.log`; the build id comes from `game/csgo/steam.inf`.

## Verdict

`cl_demo_view_offset_left` **exists in build 2000908 and does nothing**. It is a dead
stub: it registers, accepts and stores values, and has no effect on the rendered image
under any condition tested.

The central hypothesis of the project does not survive. The plan changes accordingly —
see "Consequence" below. Nothing about the multi-pass machinery in note 02 is affected;
only the mechanism for moving the eye is.

## Finding 1: the demo eye offset exists and is not even hidden

```
name                     value  default  flags   help text
cl_demo_view_offset_left 0               client  View offset during demo playback
                                                 (+/- 1.25 is a good default for human
                                                  average left/right eye offset)
```

Present in the live build, with Valve's interpupillary-distance description intact.
Its flag is plain `client` — the GameTracking dump lists it as `developmentonly
defensive`, but in this build it is not hidden at all.

### …and it has no effect

Measured on a professional GOTV demo (MOUZ vs Natus Vincere, mirage, from HLTV),
paused, POV of a player on Bombsite A — a frame with an ammo crate a few metres away
and buildings across the site, so parallax would have somewhere to show.

| offset | mean diff vs baseline | max channel diff | pixels changed |
| --- | --- | --- | --- |
| -1.25 | 0 | 0 / 765 | 0% |
| +1.25 | 0 | 1 / 765 | 0% |
| +10   | 0 | 0 / 765 | 0% |

Not "small". Identical. A 10-unit offset is roughly a quarter of a metre and would be
unmissable.

**Validity of this measurement** — each of these was checked, because the first attempt
was invalid (see below):

- **The demo really was playing.** Zero `Cannot process snapshot` errors in the log.
- **The demo really was paused.** Two captures 1.5 s apart were byte-identical
  (max channel diff 0), so the renderer was stable and noise-free.
- **The binds were installed in this session**, confirmed in the log at 20:27:23, after
  `mirv_cvar_unhide_all` reported 1929 convars unhidden.
- **The key input reached the game.** Reading the convar back after an earlier sweep
  returned `= 10`.
- Also tested: free roaming camera and default view, paused and playing, with and
  without `cl_demoviewoverride 1`. No lateral movement under any combination.

The convar registers, stores values and reads back correctly. Nothing consumes it.

### The first attempt was invalid, and why

The first sweep ran against a locally recorded bot demo and produced mean differences
of 0.001–0.009, which was read as render noise around a dead convar. That reading was
wrong — not in its conclusion, but in its evidence.

That demo did not replay. The console filled with

```
Cannot process snapshot tick 4581, it is a delta from tick 4580, which we do not have
```

hundreds of times, and in-game nothing moved: bots frozen, camera unresponsive. The
screenshots were of a still image, and **a still image cannot respond to a view offset**.
The small non-zero differences were artefacts of the broken playback, not of rendering.

The tell was in the log from the first minute and was dismissed as noise. It surfaced
only when the operator noticed the bots were not moving.

Lesson for later experiments: **verify the thing under test is alive before measuring
its response.** A frozen scene and a dead convar produce the same screenshots.

## Finding 2: no multiview stereo path in this build

```
> find stereo
snd_front_stereo_speaker_position
snd_rear_stereo_scale
snd_rear_stereo_speaker_position
snd_stereo_speaker_pan_exponent
snd_stereo_speaker_pan_radial_weight

> find multiview
no results
```

`r_stereo_multiview_instancing`, taken from a community convar list and described as
"Use multiview instancing for stereo rendering", **does not exist in build 2000908**.
Either it was removed, or that list came from a different build or a different Source 2
title.

### Control

An absent convar and a search looking in the wrong place produce identical output, so
the negative was checked before being recorded:

```
> find r_draw
r_draw3dskybox, r_draw_instances, r_drawparticles
  ("SceneSystem/Particles/Draw Particles"), r_drawworld, ... (21 results)
```

Renderer convars, including ones owned by the scene system, are visible from this
context. The multiview negative is therefore a real absence, not a lookup artefact.

## Consequence

Both engine-side shortcuts are gone: single-pass multiview does not exist, and the demo
eye offset does nothing. CS2 will not move the eye for us.

What survives is the half that matters more, and it was never dependent on either:
HLAE's multi-pass rendering (note 02) re-enters the whole client render several times
per frame from one simulation state. That is the hard part, and it is real code that
ships and is maintained.

What has to be replaced is only the cheap part — shifting the camera between passes:

- **`mirv_input`**, HLAE's documented camera override, is the obvious candidate. It is
  actively used for fragmovie work, so unlike the Valve convar it is known to function.
  Unverified: whether it can be changed *per pass* rather than per frame, which is the
  whole question.
- Failing that, a per-pass camera hook has to be added to HLAE's pass loop — more work,
  but in code that is MIT licensed and whose structure is already understood.

So the project is not blocked; it got more expensive. The estimate in note 01 that
approach 2 would need "substantial reverse engineering" is back to being accurate,
after note 02 briefly made it look cheap.

## Still to run

| Step | Status |
| --- | --- |
| 3 — offset sweep | done, negative |
| 4 — multiview path | dropped, convar absent from this build |
| 5 — `cl_demoviewoverride` | done, no effect on the offset |
| 6 — two passes in one frame via `mirv_streams` | **not yet run — now the critical test** |

Step 6 is what the project now rests on. It asks whether HLAE can render one paused
frame twice, and it no longer needs the convar to do it: if two passes can differ by
camera position at all, stereo is reachable.

## Incidental findings

Recorded because each cost time to discover.

- **CS2 has no `-netconport`.** The string is absent from `engine2.dll`, so there is no
  remote console; the experiment was driven with synthesised key input instead
  (`SendInput` with the scancode flag — plain `SendKeys` does not reach the game).
- **`playdemo` over an already-playing demo crashes the game** with
  `CDemoPlayer::StartPlayback: couldn't open demo file` followed by an access violation,
  even though the file is present and intact.
- **Dragging the demo timeline slider crashed the game** (access violation). Milestone 4
  requires seeking, so this needs revisiting — and specifically, whether it also happens
  without HLAE attached.
- A `record`ed POV demo has no other players to switch to, and once `spec_mode 6` is set
  the view cannot be returned to the default. Restarting the game is the way back.
