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
2. Launch CS2 through HLAE so `AfxHookSource2` is injected. `-insecure` stays in the
   launch options: it is this project's operating boundary regardless of the plugin.
3. In the console, before anything else:

   ```
   mirv_cvar_unhide_all
   mirv_cvar_unlock_sv_cheats
   ```

   Everything downstream depends on these succeeding. If `mirv_cvar_unhide_all` is not
   recognised, HLAE is not attached, and every convar result afterwards is meaningless —
   a missing hook looks exactly like a missing convar.
3. Launch CS2 with `-insecure -novid -allow_third_party_software`.

## Tools

HLAE `v2.192.2` in `D:\Dev\cs2-vr-tools\hlae\` — `x64\AfxHookSource2.dll` built
2026-09-12. Outside the repository: third-party binary, not project content.

**HLAE unhides the convars itself.** It provides:

```
mirv_cvar_unhide_all
mirv_cvar_unlock_sv_cheats
```

That removes the need for the `cvar-unhide-s2` plugin entirely, along with the
`gameinfo.gi` edit it required. HLAE injects into the running game and touches no game
files, so Steam has nothing to revert.

### Do not install cvar-unhide-s2 (tried 2026-09-18, crashed the game)

Recorded so it is not retried. The plugin ships its payload as `addons\bin\win64\
server.dll`, and the `Game csgo/addons` search path it asks for puts that directory
ahead of the stock one on `GAMEBIN`. Confirmed from the crash dump's search-path spew:

```
41:  GAMEBIN  ...\game\csgo\addons\bin\win64\
42:  GAMEBIN  ...\game\csgo\addons\bin\
```

The engine then loads the plugin's `server.dll` in place of the stock module. That
binary dates to 2025-08-01, thirteen months behind build 2000908, and CS2 crashed
during startup — `engine2`, `rendersystemdx11`, `scenesystem` and `vphysics2` were
loaded, no `server.dll` was, so it failed at exactly that point.

Steam then deleted the modified `gameinfo.gi` on its own and flipped the app state out
of "fully installed". Modifying that file is not merely fragile across updates, as
assumed earlier: Steam actively reverts it.

## Prepared config files

Two configs in [`../../scripts/cs2/`](../../scripts/cs2/) cover the console work.
Copy both into `<CS2>\game\csgo\cfg\`:

| File | Purpose |
| --- | --- |
| `exp00_dump.cfg` | `exec exp00_dump` — captures `version` and the full `cvarlist` to a log |
| `exp00_probe.cfg` | `exec exp00_probe` — checks the four convars exist, binds the offset sweep to F5-F9 |

`exp00_probe.cfg` changes nothing by itself, so the baseline screenshot can be taken
before anything is altered.

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
