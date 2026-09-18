# Experiment 00 — results

**Build:** CS2 ClientVersion 2000908, PatchVersion 1.41.8.1, SourceRevision 10981323,
dated 2026-09-09.
**Run:** 2026-09-18, RTX 4070 Laptop (driver 32.0.15.9144), D3D11 backend, windowed 1280x720.
**Tooling:** HLAE v2.192.2, `AfxHookSource2.dll` built 2026-09-12, injected via
`-customLoader -noGui -autoStart`. CS2 launched with `-insecure`.

Steps 1 and the convar half of the experiment are complete. The offset sweep and the
two-pass test are not yet run — they need a demo.

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

This was the central hypothesis of the project and it survives. **Existence is not
function** — whether it moves the camera, and how, is the next test.

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

The cheapest possible path — flip a convar and let the engine render both eyes in one
pass — is gone. What remains is the combination described in
[`../02-hlae-multipass-analysis.md`](../02-hlae-multipass-analysis.md): HLAE re-renders
the scene per pass, and `cl_demo_view_offset_left` shifts the eye between passes. That
costs two full scene traversals per stereo frame instead of one, which moves the frame
budget from a secondary concern to the primary risk.

This does not change the plan, only its cost. The remaining question is unchanged and
still unanswered: does the offset convar actually do anything?

## Still to run

| Step | Blocker |
| --- | --- |
| 3 — offset sweep on a paused demo | needs a demo |
| 4 — multiview path | dropped, convar absent |
| 5 — `cl_demoviewoverride` behaviour | needs a demo |
| 6 — two passes in one frame via `mirv_streams` | needs a demo |

A bot demo on de_dust2 is being recorded for this, deliberately including smoke — the
effect most likely to break under two-pass rendering.
