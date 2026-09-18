# Milestone 1 note: candidate integration points in CS2

Date: 2026-09-18. Status: desk research only. Nothing below has been run against CS2 —
the game was still downloading when this was written. Every claim marked **unverified**
is a hypothesis to test, not a finding.

## Summary

No CS2 VR project exists, publicly or otherwise. Searching turned up VR mods for
Counter-Strike: Source (Source 1) and for Half-Life 2 via Alyx's engine, but nothing
that renders CS2 itself in stereo.

The useful result of the search is different and better: **CS2 appears to ship with
unused stereo and VR plumbing already inside it**, and there is a mature, actively
maintained hooking library that attaches at exactly the engine layer this project
needs. That reorders the work considerably — the first task is to test what is already
there before writing a stereo renderer.

## Why portal2vr does not port

The reference project works because of one specific piece of luck: Source 1 renders
through Direct3D 9, and `d3d9.dll` can be replaced wholesale. A DXVK fork standing in
that position sees every draw call and every transform, which is where both the stereo
views and the head tracking come from.

CS2 removes that lever:

- Source 2 and x64, against Source 1 and x86. The reference mod is built around a 32-bit
  D3D9 interface that does not exist here.
- CS2 ships **three** render backends: `rendersystemdx11.dll`, `rendersystemvulkan.dll`
  and `rendersystemempty.dll`. Note that DXVK translates D3D9 *into* Vulkan — CS2 having
  its own native Vulkan backend does not give the wrapper anything to stand in front of.
- More fundamentally, **no graphics-API wrapper can produce correct stereo**. A wrapper
  can offset a view matrix, but it cannot make the engine traverse the scene a second
  time. Culling, shadow cascades, particle simulation and temporal history are all
  already resolved against one camera by the time draw calls reach the API. A second
  "eye" derived at that layer is a distorted copy, not a second viewpoint.

So the central question of this project is not which VR API to use. It is: **where can
CS2 be asked to produce a second view from one simulation state?**

## What CS2 already exposes

### Stereo hooks in demo playback

From the convar dump of the current build
([SteamDatabase/GameTracking-CS2](https://github.com/SteamDatabase/GameTracking-CS2),
`DumpSource2/convars.txt`):

```
cl_demo_view_offset_left 0   (developmentonly clientdll defensive)
    View offset during demo playback
    (+/- 1.25 is a good default for human average left/right eye offset)

cl_demoviewoverride 0        (developmentonly clientdll defensive)
    Override view during demo playback

cl_demo_steadycam_enable 0   (developmentonly clientdll defensive)
    Stabilize camera orientation/position during demo playback
    1 == remove roll, 2 == steadycam
```

The first is an interpupillary offset applied during demo playback. Valve left a
mechanism in CS2 for shifting the view by half a human eye separation, specifically in
the demo path — plausibly for capturing stereo content from recordings, which is this
project's exact scenario.

**Unverified:** whether it still does anything; whether it offsets camera position only
or also produces a proper asymmetric stereo projection; whether it can change per frame.

### A stereo render path in the renderer

From a convar list captured in-game
([ArminC-CS2-Cvars](https://github.com/ArmynC/ArminC-CS2-Cvars)):

```
r_stereo_multiview_instancing  (cheat)  Default: false
    Use multiview instancing for stereo rendering.
```

Multiview instancing is the standard VR technique of rendering both eyes in a single
scene traversal. If this path is live, the expensive half of milestone 3 already exists
inside the engine.

This convar does not appear in the GameTracking dump, but that dump only covers
`clientdll` and `gamedll`, while `r_*` convars live in the render and engine modules —
so its absence there is not evidence against it. It has never been discussed anywhere
publicly; nobody has tried it.

**Unverified:** whether it exists in the current build at all, and what happens when set.

### Alyx VR code was not stripped

Nine `cc_vr_*` convars survive in CS2 — the Half-Life: Alyx VR subtitle system,
including world-space panel placement (`cc_vr_forward_offset`, `cc_vr_epsilon`) and
depth testing against the scene (`cc_vr_depth_test`). CS2 is built from a Source 2
tree that still contains VR components.

This is weak evidence on its own — leftover convars do not imply a working code path —
but it is consistent with the two findings above.

## Engine module layout

Module metadata in the same dump names the modules worth knowing:

`engine2`, `scenesystem`, `worldrenderer`, `rendersystemdx11`, `materialsystem2`,
`meshsystem`, `particles`, `client`, `server`.

The manifests also leak Valve's own source paths, e.g. `src/engine2/renderingworld.cpp`
and `src/worldrenderer/grasstilesceneobject.cpp`, which is useful for orienting inside
disassembly later.

**Correction, 2026-09-18.** An earlier version of this note claimed CS2 had no Vulkan
backend, inferred from the module metadata list in the GameTracking dump, which contains
only `rendersystemdx11.kv3`. The actual installation disproves it:

```
rendersystemdx11.dll     4.5 MB
rendersystemvulkan.dll   6.1 MB
rendersystemempty.dll    1.7 MB
```

So the graphics backend is **not** decided for us, and this reopens a question worth
weighing before committing:

- **D3D11.** What HLAE hooks — `RenderSystemDX11Hooks.cpp` is D3D11-specific, so the
  multi-pass machinery in note 02 is only available here. Requires D3D11-to-XR interop.
- **Vulkan.** Would allow OpenXR to be driven with Vulkan directly, no interop, and is
  the same API family the portal2vr reference submits through. But none of the HLAE
  interception applies, so the second view pass would have to be built from nothing.

Unverified: whether the Vulkan backend is actually reachable in CS2 (a launch option,
a convar, or dead weight shipped from the shared Source 2 tree), and whether it is
stable enough to be worth the loss of HLAE.

Given that the multi-pass path is the strongest asset found so far, D3D11 remains the
working assumption — but as a choice made for a reason, not an absence of options.

## HLAE as integration substrate

[advancedfx](https://github.com/advancedfx/advancedfx) (`AfxHookSource2`) is **MIT
licensed** and actively maintained — last push 2026-09-12, release 0.40.0 in July 2026
tracking current CS2 patches. It hooks precisely the layer this project needs:

| File | Relevance |
| --- | --- |
| `RenderServiceHooks.h/.cpp` | `Hook_Engine_RenderService()`, `Hook_SceneSystem_WaitForRenderingToComplete()` |
| `RenderSystemDX11Hooks.cpp` | 227 KB of D3D11 render-system interception |
| `SceneSystem.cpp` | scene system access |
| `CamIO.cpp`, `CampathDrawer.cpp` | camera import/export and paths |

Its `mirv_input` command is documented as "Override game camera and control", with
`mirv_camio` and `mirv_fov` alongside. Milestone 2 — head pose driving the spectator
camera on a paused demo — is largely covered by work that already exists.

HLAE is a DLL injected into CS2. That is compatible with this project's stated operating
boundary of `-insecure` local demo playback only, and incompatible with anything else.

## Licensing

- **advancedfx: MIT.** Safe to study and reuse with attribution.
- **cvar-unhide-s2: MIT.**
- **portal2vr: no license file, in either `vr-meta/portal2vr` or upstream `Gistix/portal2vr`.**
  Default copyright therefore applies — accessible source is not a grant of reuse. The
  repository also vendors `L4D2VR` and a DXVK fork, each with its own provenance. Treat
  it as a document to learn from, not a source to copy from, until this is resolved.

## Candidate approaches, in order of preference

1. **Use the engine's own stereo path.** If `r_stereo_multiview_instancing` or the demo
   view offset is live, the work becomes feeding poses in and taking two targets out.
   Cheapest by a wide margin. Unknown whether it is possible at all.
2. **Engine-level second view pass.** Drive a second scene traversal through the
   `RenderService` / `SceneSystem` hooks HLAE already establishes. Correct results.
   **Revised after reading the HLAE source — see [`02-hlae-multipass-analysis.md`](02-hlae-multipass-analysis.md):
   this is much cheaper than first assumed. HLAE already re-renders the scene N times
   per frame from one simulation state, and applies per-pass convars while doing it.
   Combined with option 1 it may be the whole answer.**
3. **Alternate-eye rendering.** Alternate the eye per frame at the presentation hook.
   On a *paused* demo this is exact and nearly free, which fits the primary use case;
   during playback it halves the rate and introduces judder.
4. **Depth reprojection.** One view plus depth. Cheap, but fails on smoke, particles and
   transparency — precisely the content that makes CS2 worth watching. Last resort, and
   the README feasibility gate demands the evidence be documented before settling here.

## Open questions

- Does `cl_demo_view_offset_left` do anything in the current build, and is it a position
  offset or a full stereo projection?
- Does `r_stereo_multiview_instancing` exist in the current build?
- Can the demo be held on a fixed frame while the view changes, with no simulation
  advance between eyes?
- Does the HLAE camera override survive the engine's own spectator camera updates?
- Is Quest Link encoding on the RTX 4070 or the iGPU?
- OpenXR or OpenVR — deferred until the above are answered; the choice depends on which
  integration point wins.

## Next step

Experiment 00 (see `experiments/00-stereo-cvar-probe.md`) answers the first two
questions in one sitting, as soon as CS2 finishes installing. It requires no build
toolchain and no code.

In parallel, and independent of CS2: install the toolchain, and read
`RenderSystemDX11Hooks.cpp` to understand what the HLAE interception actually provides.
