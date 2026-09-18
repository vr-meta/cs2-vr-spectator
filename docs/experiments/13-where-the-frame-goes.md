# 13 — Where the frame actually goes

**Question.** [Issue #1](https://github.com/vr-meta/cs2-vr-spectator/issues/1) records
38–48 frames a second against the Quest 3's 72, and lists three levers in order of
expected return:

1. The game's graphics settings — "untouched for the whole project — CS2 is running at its
   defaults with shadows, effects and model detail at full. Never tested, and the obvious
   first thing."
2. Resolution — "dropping to ~70% linear halves the pixels."
3. Two scene traversals instead of three.

**Answer.** The first lever was already pulled, by the game, without anyone noticing. The
second is worth almost nothing. And the third turns out to be a much smaller share of the
frame than the list implies — **less than half of a VR frame is the game rendering at
all.**

## The instrument

`mirv_vr_frametime <seconds> [label]`, new in the hook. It samples the wall time around
each engine frame, keeps the samples, and reports mean, median, p95, p99 and the worst
frame. Percentiles rather than an average, because an average hides exactly what makes a
headset uncomfortable: a run averaging 60 that drops one frame in twenty feels worse than
a steady 50.

It measures on the engine thread with no session and no headset, which is what makes all
of the below possible on a desk.

## The trap that nearly ruined the whole measurement

The first three runs — 640x480, 1770x1946 and 2528x2780, a factor of twenty-three in
pixels — came back at 8.50, 8.51 and 8.53 ms. Identical to two decimal places.

That is not a workload, it is a cap. `fps_max 120` lives in

```
userdata\<id>\730\local\cfg\cs2_machine_convars.vcfg
```

and CS2 loads it **after** the command line, so `+fps_max 0` at launch does nothing. The
number being measured was the cap, not the game.

Worth knowing beyond this experiment: any frame-rate measurement in this project taken
without setting `fps_max` from a config that runs at startup is suspect if it lands near
120. `scripts/cs2/exp13_perf.cfg` sets `fps_max 0` and `fps_max_tools 0` at exec time and
puts them on F6.

## One scene traversal, uncapped

All at minimum graphics settings, playing the same demo, twenty-second samples:

| resolution | pixels | mean | median | p99 |
| --- | --- | --- | --- | --- |
| 2528 × 2780 | 7.03 M | **3.59 ms** | 3.36 | 5.21 |
| 1770 × 1946 | 3.44 M | 3.29 ms | 3.11 | 4.84 |
| 640 × 480 | 0.31 M | 3.17 ms | 2.92 | 4.86 |

Twenty-three times fewer pixels buys **0.42 ms**. Fit a line through those and the
pixel-independent part is about 3.15 ms — roughly **88% of a traversal at full VR
resolution is not the pixels**. It is scene traversal, animation, the client's per-frame
work: CPU, not fill.

So lever 2 is worth about 8% of a traversal, not the half the issue expected. Dropping to
70% linear would save 0.3 ms of a 23 ms frame, and cost the sharpness that made the last
resolution increase worth doing.

## The graphics settings were already at minimum

This is the surprise. `cs2_video.txt` on this machine:

```
"Autoconfig"                          "2"
"setting.shaderquality"               "0"
"setting.r_texturefilteringquality"   "0"
"setting.msaa_samples"                "0"
"setting.r_csgo_cmaa_enable"          "0"
"setting.videocfg_shadow_quality"     "0"
"setting.videocfg_dynamic_shadows"    "0"
"setting.videocfg_texture_detail"     "0"
"setting.videocfg_particle_detail"    "0"
"setting.videocfg_ao_detail"          "0"
```

Every quality setting at zero, and `Autoconfig 2` — the game configured itself that way.
Nobody chose it and nobody noticed. The issue's "shadows, effects and model detail at
full" was an assumption, and it was wrong.

The lever is real, it has simply already been used. Raising everything — shader quality 1,
anisotropic 3, MSAA 4×, CMAA on, shadows 2, dynamic shadows on, texture and particle
detail 2, ambient occlusion on — and measuring again at 2528 × 2780:

| settings | mean | median | p99 |
| --- | --- | --- | --- |
| minimum (as found) | 3.59 ms | 3.36 | 5.21 |
| high | 5.58 ms | 5.92 | 7.38 |

So the settings are worth **1.99 ms a traversal, 36%** — a real lever, pointing the wrong
way. There is nothing left to turn down.

*(The settings were restored afterwards. They are the user's.)*

## What this says about the 23 ms

Three traversals at minimum settings and full resolution:

```
3 × 3.59 ms = 10.8 ms
```

The headset measures 38–48 frames a second: **21 to 26 ms a frame**. So of a VR frame,
about eleven milliseconds are the game drawing the world three times, and **ten to fifteen
are something else entirely.**

That "something else" is the submission path and the runtime: two `CopyResource` calls of
7-megapixel textures, `xrWaitFrame` pacing, the compositor, and SteamVR's share. It is
now the larger half of the budget, and nothing in issue #1's list addresses it.

It also explains the one number in that issue that never fitted. Removing the wasted
fourth pass bought ~15%, where dropping one traversal in four implies 33%. With
traversals being under half the frame, ~15% is exactly what dropping one of them should
give. The model is consistent; the earlier expectation was not.

## What to do instead

In order of expected return, revised:

1. **Try Meta's runtime.** SteamVR is a translation layer this setup has no use for, and
   the non-traversal half of the frame is exactly where a runtime shows up.
   [Experiment 12](12-openxr-runtime.md) made this a command-line switch — `-MetaRuntime`
   — rather than a machine-wide registry change, so it costs one launch to find out.
2. **Measure the submission path.** `mirv_vr_frametime` times the whole frame; the copy
   and the wait inside it are not yet separated. Until they are, "the runtime" is a guess
   with a good alibi rather than a measurement.
3. **Drop the main pass.** Worth about 3.6 ms of 23, roughly 15%. Real, but it costs the
   monitor view and whatever the UI needs, and it is no longer the biggest thing on the
   list.
4. **Resolution.** Worth about 0.3 ms. Effectively nothing. Leave it where it is.
5. **Graphics settings.** Already at minimum. Nothing to do.

The one thing that would beat all of these is not rendering the world twice — single-pass
stereo instancing. That door is shut and was shut before this project started:
`r_stereo_multiview_instancing` **does not exist in build 2000908**, established by
searching the running engine's own convar tables rather than a community list
([00-results, finding 2](00-results.md)). Two traversals is the floor.

## Reproducing

```powershell
scripts\launch-cs2-experiment.ps1 -SelfBuilt -Demo pro_mirage.dem `
    -Width 2528 -Height 2780 -ExecCfg exp13_perf
# F6 uncaps, F2 samples for twenty seconds.
scripts\send-key.ps1 -Key F6
scripts\send-key.ps1 -Key F2
```

Samples land in `game\csgo\console.log`. Twenty seconds gives ~5600 frames at full
resolution, which is enough for p99 to mean something.
