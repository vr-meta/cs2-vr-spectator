# Experiment 00 — probe the built-in stereo convars

**Question:** does CS2 still contain a working stereo path, reachable from demo playback?

**Cost:** one sitting. No build toolchain, no code, no injection beyond an MIT-licensed
Source 2 plugin.

**Why first:** if the answer is yes, milestone 3 (true stereo rendering) stops being a
reverse-engineering project and becomes a plumbing project. If the answer is no, that is
the feasibility gate failing early and cheaply, which is worth just as much.

Status: **not yet run** — waiting on the CS2 download.

## Preconditions

- CS2 installed; record the build id from `version` in the console.
- At least one local `.dem` file on a known map.
- Launch options contain `-insecure`. Never run this against matchmaking.
- The headset is not involved at this stage. This is a flat-screen experiment.

## Setup

1. Enable the developer console (Settings, Game, Enable Developer Console).
2. Install [cvar-unhide-s2](https://github.com/saul/cvar-unhide-s2) (MIT). The three
   convars under test are `developmentonly defensive` and are invisible without it.
   It installs as a Source 2 plugin into
   `game/csgo/addons/`, plus a `Game csgo/addons` search path in `game/csgo/gameinfo.gi`.
   It requires `-insecure`, which this project requires anyway.

   **Version risk:** the newest release is `v0.5.0` from 2025-08-01, over a year old.
   Source 2 plugins depend on engine internals and break on game updates, so it may
   simply fail to load. If it does, rebuild it from source before concluding anything
   about the convars — a plugin that did not load looks exactly like a convar that does
   not exist.

   HLAE (needed from step 6 on) ships as `v2.192.2`, released 2026-09-12, so it should
   be current. No building required for either tool unless the plugin fails.
3. Launch CS2 with `-insecure -novid -allow_third_party_software`.

## Step 1 — capture the ground-truth convar list

Everything downstream depends on knowing what actually exists in *this* build, rather
than in a community dump of some earlier one.

```
con_logfile cvarlist.log
cvarlist
con_logfile ""
```

The log lands in `game/csgo/`. Then check, in order:

```
r_stereo_multiview_instancing
cl_demo_view_offset_left
cl_demoviewoverride
cl_demo_steadycam_enable
```

Also grep the captured list for `stereo`, `multiview`, `_eye`, `vr_`, `cc_vr`, `hmd`.
The community dumps that motivated this experiment cover only `clientdll` and `gamedll`,
so the renderer convars are genuinely unknown territory — there may be more than the one
convar found so far.

**Record the full list in `docs/dumps/` regardless of the outcome.** It is the reference
for every later question and it is build-specific.

## Step 2 — hold a demo still

```
playdemo <file>
demo_pause
sv_cheats 1
```

Verify the frame is genuinely frozen: no particle motion, no smoke evolution, no
animation. Note whether the view still drifts (spectator camera logic may keep running
even while the demo is paused — that matters later, and `cl_demo_steadycam_enable` may
be relevant to it).

## Step 3 — the eye offset

With the demo paused, sweep:

```
cl_demo_view_offset_left 0
cl_demo_view_offset_left 1.25
cl_demo_view_offset_left -1.25
cl_demo_view_offset_left 10
```

Take a screenshot at each value from an identical starting state.

What to determine:

- **Does anything move at all?** If not, the convar is a dead stub and this line of
  inquiry ends here.
- **Is it a pure lateral translation, or does the projection change?** Compare parallax
  between near and far geometry across the shots. A position-only offset gives correct
  relative parallax but a symmetric frustum; true stereo would give an asymmetric one.
  A position-only offset is still usable — it is most of what is needed.
- **Does it apply in the demo path only,** or does it survive outside demo playback?
- **Does it update per frame while paused,** i.e. can the value be flipped between
  renders without the demo advancing? This is the crux for alternate-eye rendering.
- **Are the units inches?** 1.25 as "half a human eye separation" implies Source units,
  where 1 unit is roughly 1 inch — so ~6.35 cm IPD. Confirm against known map geometry,
  because world scale is a milestone 2 acceptance criterion.

## Step 4 — the multiview path

Only if the convar exists:

```
sv_cheats 1
r_stereo_multiview_instancing true
```

Expect one of: nothing observable, a visual artefact, a performance change, or a crash.
All four are informative. Capture `con_logfile` output and any crash dump.

If it produces anything at all, follow up on where the second view lands — a multiview
render target has two array slices, and finding whether slice 1 is populated is the
whole question. That likely needs a frame capture (RenderDoc against a D3D11 title) and
belongs to a separate experiment.

## Step 5 — view override

```
cl_demoviewoverride 1
```

Determine what it expects. It may want a companion convar or command supplying the
view, in which case find it in the captured convar list. This is the built-in
alternative to the HLAE `mirv_input` camera override, and if it works it is preferable:
no injection.

## Step 6 — two passes in one frame

Only worth doing if step 3 showed the offset working. Requires HLAE
([advancedfx](https://github.com/advancedfx/advancedfx), MIT) attached to CS2.

Per [`../02-hlae-multipass-analysis.md`](../02-hlae-multipass-analysis.md), streams with
differing `BeforeCommands` are rendered in separate passes of the same frame. So:

```
mirv_streams add afxDefault left
mirv_streams edit left settings ... (BeforeCommands: cl_demo_view_offset_left -1.25)
mirv_streams add afxDefault right
mirv_streams edit right settings ... (BeforeCommands: cl_demo_view_offset_left 1.25)
mirv_streams record start
```

Exact command syntax to be confirmed against `mirv_streams` built-in help — invoking a
command with no arguments prints it.

On a paused demo, capture one frame and compare the two outputs. What to determine:

- Do the two passes actually differ by the eye offset?
- Is the scene identical otherwise — same particle state, same smoke, same animation
  pose? Any difference means the simulation advanced between passes, which would be
  disqualifying.
- How long does a two-pass frame take? This is the first real data point on whether the
  multi-pass path can fit a VR frame budget rather than an offline capture one.

A positive result here means the stereo half of the project is reachable by composing
existing parts, and the remaining work is pose input plus getting the pass results to
the compositor as textures instead of files.

## Recording results

Write findings to `docs/experiments/00-results.md`, including:

- CS2 build id, date, GPU driver version.
- The captured `cvarlist.log`, committed under `docs/dumps/`.
- Screenshots for the offset sweep.
- A plain yes/no per open question, including the ones answered "no".

Negative results matter here as much as positive ones. The README feasibility gate asks
for documented evidence before scope changes, and this is where that evidence starts.

## What each outcome implies

| Outcome | Consequence |
| --- | --- |
| Offset works and can flip per frame while paused | Alternate-eye stereo on paused demos is reachable almost immediately. Milestone 3 largely bypassed for the static case. |
| Offset works but only per demo tick | Stereo still possible, but eyes must come from consecutive frames — needs the simulation held still between them. |
| Multiview path is live | Best case. Single-pass stereo already in the engine; work becomes pose input and target extraction. |
| Nothing works | Fall back to the HLAE `RenderService` / `SceneSystem` hooks and a genuine second view pass. Document the evidence as the feasibility gate requires. |
