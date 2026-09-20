# Documentation

Everything that was found out, in the order it makes sense to read it. The root
[README](../README.md) is for using this; [CONTRIBUTING](../CONTRIBUTING.md) is for building
and changing it; this page is the map of how it works and how each thing was established.

There are also two skills for coding agents, in `.claude/skills/` — one to
[install it](../.claude/skills/install-cs2-vr/SKILL.md) and one to
[run and adjust it](../.claude/skills/run-cs2-vr/SKILL.md). They are procedures rather than
documentation, but they carry the operating rules in a form something else can follow, and
the second one is the practical answer to "there is no console in a worn session".

The experiments are the core. Each is one question put to the engine or the runtime, the
method, and the answer - including the confident explanations that turned out wrong. Read
the relevant ones before changing what they settled.

## How it works, in one paragraph

CS2's own stereo hooks are dead ends: the demo eye-offset convar has no effect and the
multiview path is absent. Stereo comes instead from HLAE's multi-pass rendering, which
re-renders one frame several times, plus a change of ours that gives each pass its own
camera. The engine resolves the camera once per frame, outside the pass loop - but the
object holding it is persistent and re-read by every pass, so rewriting it between passes
separates the eyes. Position, orientation and field of view all go through that one lever.
The two eye passes are copied into OpenXR swapchains and submitted as one projection layer;
the HUD and CS2's own menu travel separately, as quad layers.

## Index

- [`src/`](../src/) - the C++ this project wrote: a camera pose per render pass, and the OpenXR session that consumes it.
- [`docs/environment.md`](environment.md) - the reference machine, headset runtimes, and toolchain state.
- [`docs/01-source2-integration-points.md`](01-source2-integration-points.md) - candidate integration points, licensing, and open questions. Notable finding: CS2 ships unused stereo convars in its demo playback path.
- [`docs/02-hlae-multipass-analysis.md`](02-hlae-multipass-analysis.md) - HLAE already re-renders the CS2 scene several times per frame from one simulation state, which is the core of what stereo needs.
- [`docs/03-vr-bridge-sketch.md`](03-vr-bridge-sketch.md) - where the per-eye textures would come from, and the problems that sketch has to survive.
- [`docs/experiments/00-stereo-cvar-probe.md`](experiments/00-stereo-cvar-probe.md) - the first experiment.
- [`docs/experiments/00-results.md`](experiments/00-results.md) - CS2 own stereo hooks are dead: the demo eye-offset convar has no effect, and the multiview path is absent.
- [`docs/experiments/01-camera-control.md`](experiments/01-camera-control.md) - `mirv_input` moves the camera exactly and repeatably, replacing the dead convar.
- [`docs/experiments/02-multipass.md`](experiments/02-multipass.md) - confirmed: HLAE renders one frame twice with independent settings per pass. The expensive half of stereo already exists.
- [`docs/experiments/03-per-pass-camera.md`](experiments/03-per-pass-camera.md) - the camera cannot be changed per pass from config: the view is resolved before pass commands run. Needs a change inside the render path.
- [`docs/experiments/08-openxr-instance.md`](experiments/08-openxr-instance.md) - **OpenXR comes up inside CS2.** The runtime is identified, the Quest is found through Link, and it asks for 2528x2780 per eye. Also: never kill the runtime with an instance live, and cap the game or it fights the compositor.
- [`docs/experiments/07-eye-pose.md`](experiments/07-eye-pose.md) - the per-pass code becomes one module with the interface `xrLocateViews` will drive, and the last unverified lever - **view angles** - is measured. Everything the headset needs to send into the engine now works.
- [`docs/experiments/06-per-eye-projection.md`](experiments/06-per-eye-projection.md) - **per-eye field of view works too**, through the same write that carries the camera. That settles the frame submission: a symmetric frustum per eye, reported honestly to OpenXR, is correct.
- [`docs/experiments/05-stereo-pair.md`](experiments/05-stereo-pair.md) - **the pair is correct.** At 63 mm separation it differs only by viewpoint; with separation 0 on a playing demo the eyes stay identical while consecutive frames differ by up to 98%, so nothing advances between passes.
- [`docs/experiments/04-per-pass-camera.md`](experiments/04-per-pass-camera.md) - **stereo works.** The view setup runs once per frame, outside the pass loop, but the `CViewSetup` it fills is persistent and re-read per pass. Rewriting it there gives each eye its own camera: control take 0.00% differing, test take 88.89% with correct parallax.

- [`docs/workflow.md`](workflow.md) - how experiments are run here: division of labour, launch, capture, and the rules that earned their place.
- [`docs/05-view-setup-point.md`](05-view-setup-point.md) - the exact function where a per-pass camera must be applied, and why the config route failed.
- [`docs/experiments/09-frames-in-the-headset.md`](experiments/09-frames-in-the-headset.md) - **frames reach the headset.** The session, the swapchains, and the two problems only a headset reveals: the world swimming when the head turns, and a followed player's aim dragging your head with it.
- [`docs/experiments/10-frame-budget.md`](experiments/10-frame-budget.md) - what it costs: 38-48 frames/s against the 72 the headset wants, and a wasted render pass removed.
- [`docs/experiments/11-seeking.md`](experiments/11-seeking.md) - **seeking works and the timeline never crashed anything.** Twenty-three seeks including full-width slider drags. Direction is not what costs: a backward seek replays from the preceding keyframe, so ten seconds back can cost more than sixty forward. And Panorama takes synthetic mouse input, which we had assumed it did not.
- [`docs/experiments/12-openxr-runtime.md`](experiments/12-openxr-runtime.md) - choosing the OpenXR runtime for CS2 alone, through `XR_RUNTIME_JSON`, instead of the machine-wide registry change. No elevation, nothing left behind.
- [`docs/experiments/13-where-the-frame-goes.md`](experiments/13-where-the-frame-goes.md) - **more than half a VR frame is not rendering.** Three traversals are 11 ms of 21-26. The graphics settings were already at minimum and nobody knew; resolution is worth 8%. Also the trap that nearly ruined the measurement: `fps_max 120` lives in a file CS2 loads after the command line.
- [`docs/experiments/14-when-the-ui-is-drawn.md`](experiments/14-when-the-ui-is-drawn.md) - the UI is composited **once per pass**, so it really is baked into both eyes, and moving the capture to HLAE's before-UI hook takes it out.
- [`docs/experiments/15-hud-per-eye.md`](experiments/15-hud-per-eye.md) - the name tags, reproduced on a monitor at last, and the cheap fix disproved: asking the engine to rebuild its matrices puts the base camera back and cancels the stereo.
- [`docs/experiments/16-the-hud-on-a-panel.md`](experiments/16-the-hud-on-a-panel.md) - **the score is back.** The world is wiped out of the main pass between the scene and the UI, so the quad carries the HUD on nothing; measured first, because a write-masked alpha channel would have made the panel invisible and looked like the feature not working. Then cut into one quad per HUD group, because a single sheet puts the score at the horizon and the timeline on the floor.
- [`docs/experiments/17-judder-on-head-turns.md`](experiments/17-judder-on-head-turns.md) - **the judder was never the frame rate.** Reprojection is exact if the reported pose is the one the image was drawn from; it was read from a global the engine thread had often already moved on from, by a different amount each frame. Each pass now carries its own. Worn verdict: none at all, at 30 frames a second.
- [`docs/experiments/18-what-the-fov-field-means.md`](experiments/18-what-the-fov-field-means.md) - **the same field means two different things at two different moments**, and three readings of the source could not tell which applied where. Settled in thirty seconds by putting the dial on the triggers and handing it to someone wearing the headset: they converged on 0.853 where the prediction was 0.848, inside one step.
- [`docs/experiments/19-four-faults-a-desk-could-not-see.md`](experiments/19-four-faults-a-desk-could-not-see.md) - the camera frozen wherever the session started, a controller pressing nothing, whoever you watched frozen in place, and the HUD panels leaning. Each passed every check available without a headset; each was found within minutes of wearing one. Pause is not a neutral condition, and neither is facing forwards.
- [`docs/experiments/21-what-the-zip-has-to-contain.md`](experiments/21-what-the-zip-has-to-contain.md) - which ten DLLs and 400 KB of resources a release actually needs, read off a running game and off the source rather than guessed. It also found the rule that fixes the layout: HLAE's folder is derived from the hook DLL's own path minus one directory, so putting the DLL one level too high leaves every shader silently unfound.
- [`docs/07-release-plan.md`](07-release-plan.md) - what has to be true before a stranger can install this: a launcher instead of PowerShell, the hook made relocatable, a tagged release, and the README rewritten only once those three lines are honest.
- [`docs/06-vr-experience-plan.md`](06-vr-experience-plan.md) - the plan for making it usable: the menu, the timeline, the controls, and why they all need to leave the back buffer.
- [`docs/04-plan.md`](04-plan.md) - the original plan, phases A to E. All of it is done; 06 is what comes next.
- [`docs/patches/README.md`](patches/README.md) - the changes made to HLAE, kept so they survive a re-clone: the build fix, and the per-pass camera itself.

- [`docs/install.md`](install.md) - how to get from a clean machine to CS2 rendering into a headset: build the hook, lay out the files, launch, the controls, and the shutdown order that avoids an unkillable process.

- [`tests/`](../tests/) - the parts that are a function of their arguments and nothing else: the geometry, the stick shaping, the plausibility check on the view struct, the steam.inf parsing. 593 checks, no engine, no headset, seconds in CI.
- [`tools/xr-probe/`](../tools/xr-probe/) - which OpenXR runtime a process would actually get. Needs no headset.

Run [`scripts/check-toolchain.ps1`](../scripts/check-toolchain.ps1) to see what the machine is missing; [`scripts/install-toolchain.ps1`](../scripts/install-toolchain.ps1) installs it. [`scripts/check-patches.ps1`](../scripts/check-patches.ps1) answers "do the patches still apply", against the pinned tag or against upstream `main`.
