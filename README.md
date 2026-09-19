# CS2 VR Spectator

Watch Counter-Strike 2 match replays from inside the map using a Meta Quest 3 connected to a Windows PC.

## Status

**CS2 renders into a Meta Quest 3.** Stereo, head tracked, at the runtime's full recommended 2528x2780 per eye, from a demo playing inside the real game, with the controllers flying the viewer around the map. The feasibility question this project existed to answer is answered. There is no installable build yet.

How it works: CS2's own stereo hooks are dead ends - the demo eye-offset convar has no effect, the multiview path is absent. Stereo comes instead from HLAE's multi-pass rendering, which re-renders one frame several times, plus a change of ours that gives each pass its own camera. The engine resolves the camera once per frame, outside the pass loop - but the object holding it is persistent and re-read by every pass, so rewriting it between passes separates the eyes. Position, orientation and field of view all go through that one lever.

The pair is verified: at a real 63 mm interpupillary distance it differs only by viewpoint, and with the separation set to zero on a playing demo with live smoke the two eyes stay identical - so the simulation does not advance between passes, which is the error that would be unbearable in a headset. [Look at the pair](https://claude.ai/artifact/3iESVvwPKZjiEDLX9eAC8x).

Open work is tracked as [GitHub issues](https://github.com/vr-meta/cs2-vr-spectator/issues). Two things stand between this and something pleasant to use:

**Frame rate.** 38-48 frames/s where the headset wants 72, so SteamVR reprojects to fill the gap. Where it goes is now measured rather than guessed ([experiment 13](docs/experiments/13-where-the-frame-goes.md)): three scene traversals are about 11 ms of a 21-26 ms frame, so **more than half of a VR frame is not the game rendering at all** - it is the submission path and the runtime. The two levers everyone reaches for are spent: the graphics settings were already at minimum, the game having quietly auto-configured itself there, and resolution is worth 8% rather than the half it looks like. Trying Meta's runtime instead of SteamVR is now one launch argument.

**Everything two-dimensional.** The demo's timeline, scoreboard and menu assume one camera and a screen. Both halves are now worn and default: the eyes are captured before the UI is composited, and the HUD goes to OpenXR quad layers in space - the score up high, the radar to one side, the timeline low like a dashboard - cut out of one sheet whose world has been wiped to transparent black ([experiment 16](docs/experiments/16-the-hud-on-a-panel.md)). Player name tags are a harder case and stay off: they are world-anchored, so no flat panel can carry them, and the cheap fix was tried and [does not work](docs/experiments/15-hud-per-eye.md).

- [`src/`](src/) - the C++ this project wrote: a camera pose per render pass, and the OpenXR session that consumes it.
- [`docs/environment.md`](docs/environment.md) - the reference machine, headset runtimes, and toolchain state.
- [`docs/01-source2-integration-points.md`](docs/01-source2-integration-points.md) - candidate integration points, licensing, and open questions. Notable finding: CS2 ships unused stereo convars in its demo playback path.
- [`docs/02-hlae-multipass-analysis.md`](docs/02-hlae-multipass-analysis.md) - HLAE already re-renders the CS2 scene several times per frame from one simulation state, which is the core of what stereo needs.
- [`docs/03-vr-bridge-sketch.md`](docs/03-vr-bridge-sketch.md) - where the per-eye textures would come from, and the problems that sketch has to survive.
- [`docs/experiments/00-stereo-cvar-probe.md`](docs/experiments/00-stereo-cvar-probe.md) - the first experiment.
- [`docs/experiments/00-results.md`](docs/experiments/00-results.md) - CS2 own stereo hooks are dead: the demo eye-offset convar has no effect, and the multiview path is absent.
- [`docs/experiments/01-camera-control.md`](docs/experiments/01-camera-control.md) - `mirv_input` moves the camera exactly and repeatably, replacing the dead convar.
- [`docs/experiments/02-multipass.md`](docs/experiments/02-multipass.md) - confirmed: HLAE renders one frame twice with independent settings per pass. The expensive half of stereo already exists.
- [`docs/experiments/03-per-pass-camera.md`](docs/experiments/03-per-pass-camera.md) - the camera cannot be changed per pass from config: the view is resolved before pass commands run. Needs a change inside the render path.
- [`docs/experiments/08-openxr-instance.md`](docs/experiments/08-openxr-instance.md) - **OpenXR comes up inside CS2.** The runtime is identified, the Quest is found through Link, and it asks for 2528x2780 per eye. Also: never kill the runtime with an instance live, and cap the game or it fights the compositor.
- [`docs/experiments/07-eye-pose.md`](docs/experiments/07-eye-pose.md) - the per-pass code becomes one module with the interface `xrLocateViews` will drive, and the last unverified lever - **view angles** - is measured. Everything the headset needs to send into the engine now works.
- [`docs/experiments/06-per-eye-projection.md`](docs/experiments/06-per-eye-projection.md) - **per-eye field of view works too**, through the same write that carries the camera. That settles the frame submission: a symmetric frustum per eye, reported honestly to OpenXR, is correct.
- [`docs/experiments/05-stereo-pair.md`](docs/experiments/05-stereo-pair.md) - **the pair is correct.** At 63 mm separation it differs only by viewpoint; with separation 0 on a playing demo the eyes stay identical while consecutive frames differ by up to 98%, so nothing advances between passes.
- [`docs/experiments/04-per-pass-camera.md`](docs/experiments/04-per-pass-camera.md) - **stereo works.** The view setup runs once per frame, outside the pass loop, but the `CViewSetup` it fills is persistent and re-read per pass. Rewriting it there gives each eye its own camera: control take 0.00% differing, test take 88.89% with correct parallax.

- [`docs/workflow.md`](docs/workflow.md) - how experiments are run here: division of labour, launch, capture, and the rules that earned their place.
- [`docs/05-view-setup-point.md`](docs/05-view-setup-point.md) - the exact function where a per-pass camera must be applied, and why the config route failed.
- [`docs/experiments/09-frames-in-the-headset.md`](docs/experiments/09-frames-in-the-headset.md) - **frames reach the headset.** The session, the swapchains, and the two problems only a headset reveals: the world swimming when the head turns, and a followed player's aim dragging your head with it.
- [`docs/experiments/10-frame-budget.md`](docs/experiments/10-frame-budget.md) - what it costs: 38-48 frames/s against the 72 the headset wants, and a wasted render pass removed.
- [`docs/experiments/11-seeking.md`](docs/experiments/11-seeking.md) - **seeking works and the timeline never crashed anything.** Twenty-three seeks including full-width slider drags. Direction is not what costs: a backward seek replays from the preceding keyframe, so ten seconds back can cost more than sixty forward. And Panorama takes synthetic mouse input, which we had assumed it did not.
- [`docs/experiments/12-openxr-runtime.md`](docs/experiments/12-openxr-runtime.md) - choosing the OpenXR runtime for CS2 alone, through `XR_RUNTIME_JSON`, instead of the machine-wide registry change. No elevation, nothing left behind.
- [`docs/experiments/13-where-the-frame-goes.md`](docs/experiments/13-where-the-frame-goes.md) - **more than half a VR frame is not rendering.** Three traversals are 11 ms of 21-26. The graphics settings were already at minimum and nobody knew; resolution is worth 8%. Also the trap that nearly ruined the measurement: `fps_max 120` lives in a file CS2 loads after the command line.
- [`docs/experiments/14-when-the-ui-is-drawn.md`](docs/experiments/14-when-the-ui-is-drawn.md) - the UI is composited **once per pass**, so it really is baked into both eyes, and moving the capture to HLAE's before-UI hook takes it out.
- [`docs/experiments/15-hud-per-eye.md`](docs/experiments/15-hud-per-eye.md) - the name tags, reproduced on a monitor at last, and the cheap fix disproved: asking the engine to rebuild its matrices puts the base camera back and cancels the stereo.
- [`docs/experiments/16-the-hud-on-a-panel.md`](docs/experiments/16-the-hud-on-a-panel.md) - **the score is back.** The world is wiped out of the main pass between the scene and the UI, so the quad carries the HUD on nothing; measured first, because a write-masked alpha channel would have made the panel invisible and looked like the feature not working. Then cut into one quad per HUD group, because a single sheet puts the score at the horizon and the timeline on the floor.
- [`docs/experiments/17-judder-on-head-turns.md`](docs/experiments/17-judder-on-head-turns.md) - **the judder was never the frame rate.** Reprojection is exact if the reported pose is the one the image was drawn from; it was read from a global the engine thread had often already moved on from, by a different amount each frame. Each pass now carries its own. Worn verdict: none at all, at 30 frames a second.
- [`docs/06-vr-experience-plan.md`](docs/06-vr-experience-plan.md) - the plan for making it usable: the menu, the timeline, the controls, and why they all need to leave the back buffer.
- [`docs/04-plan.md`](docs/04-plan.md) - the original plan, phases A to E. All of it is done; 06 is what comes next.
- [`docs/patches/README.md`](docs/patches/README.md) - the changes made to HLAE, kept so they survive a re-clone: the build fix, and the per-pass camera itself.

- [`docs/install.md`](docs/install.md) - how to get from a clean machine to CS2 rendering into a headset: build the hook, lay out the files, launch, the controls, and the shutdown order that avoids an unkillable process.

- [`tests/`](tests/) - the parts that are a function of their arguments and nothing else: the geometry, the stick shaping, the plausibility check on the view struct, the steam.inf parsing. 593 checks, no engine, no headset, seconds in CI.
- [`tools/xr-probe/`](tools/xr-probe/) - which OpenXR runtime a process would actually get. Needs no headset.

Run [`scripts/check-toolchain.ps1`](scripts/check-toolchain.ps1) to see what the machine is missing; [`scripts/install-toolchain.ps1`](scripts/install-toolchain.ps1) installs it. [`scripts/check-patches.ps1`](scripts/check-patches.ps1) answers "do the patches still apply", against the pinned tag or against upstream `main`.

## Goal

Build a Windows VR spectator mod that integrates with CS2 itself. CS2 should continue to load maps, play demos, animate players, simulate effects, and render the world. The mod should connect the spectator camera and rendering pipeline to a VR headset.

The intended experience is to stand beside a bombsite, look around naturally, lean to inspect the action, and move between spectator positions while a recorded match plays. This requires actual stereoscopic rendering and head tracking, rather than displaying the desktop on a virtual screen.

## Existing reference: Portal 2 VR

The organization's [Portal 2 VR integration](https://github.com/vr-meta/portal2vr) is the primary reference for this project.

Its README describes a DXVK-based `d3d9.dll` that combines Direct3D 9-to-Vulkan translation with a VR mod, submits stereo frames to SteamVR, and supports head tracking and motion controllers. It also provides Windows installation, launch, build, and packaging workflows.

Study that project for:

- Headset pose handling, coordinate conversion, world scale, and recentering.
- Per-eye camera setup and stereo frame submission.
- VR lifecycle, configuration, diagnostics, and recovery.
- Windows installation, removal, and release packaging.

This is not a drop-in port. Portal 2 uses Source 1 and the reference mod builds for x86 around Direct3D 9. CS2 uses Source 2 and a different, 64-bit rendering stack. Engine interfaces, camera integration, render targets, synchronization, and loading mechanisms must be investigated independently.

Review the licenses and provenance of individual components before copying code; an accessible source repository is not by itself permission to reuse every component.

## Initial scope

- Windows PC running a legitimate Steam installation of CS2.
- Meta Quest 3 connected through a PC VR connection.
- Local `.dem` playback on one initial test map.
- A spectator camera with independent headset rotation and positional tracking.
- Correct left-eye and right-eye images from the same simulation state.
- Recenter, fixed observation positions, and basic spectator navigation.
- Pause, resume, and seeking using CS2's demo playback capabilities.
- Minimal diagnostics for compatibility, rendering, and frame timing.

The first prototype does not include competitive gameplay, VR weapons or hands, standalone Quest execution, or live tournament feeds. A tabletop map view and player-follow modes can be explored after the core renderer works.

## Proposed architecture

```text
CS2 local demo playback
        |
        v
Spectator camera and Source 2 render integration
        ^                         |
        |                         v
Headset pose                 Left/right eye textures
        |                         |
        +------ VR runtime -------+
                    |
                    v
             Meta Quest 3
```

### Engine adapter

Integrate with the spectator camera and scene-rendering lifecycle. Compose a spectator anchor with the tracked head pose and eye offsets. Render both eyes without advancing the match between them.

Investigate [HLAE / AdvancedFX](https://github.com/advancedfx/advancedfx) as a reference for CS2 camera and engine integration. Do not assume it already exposes the stereo rendering path needed here.

### VR bridge

Receive predicted headset poses, obtain per-eye projections, manage graphics resources, and submit frames to the VR compositor.

Evaluate OpenXR as the initial runtime API. Compare it with the OpenVR/SteamVR integration used by Portal 2 VR before committing to a backend. SteamVR runtime support and the choice of API are separate decisions.

### Spectator controls

Keep head movement independent of player aim and automatic observer camera rotations. Start with a fixed camera anchor and recentering, then add selectable positions and optional navigation with snap turning.

### Launcher and diagnostics

Eventually provide an explicit demo-viewing launcher, reversible installation, readable logs, and checks for supported game builds. Detect unsupported integration points and stop with an actionable error rather than continuing with invalid assumptions.

## Milestones and acceptance criteria

### 1. Inspect the reference and identify CS2 integration points

- Trace the Portal 2 VR pose-to-camera and render-to-compositor paths.
- Document reusable concepts and engine-specific dependencies.
- Inspect current CS2 camera/render integration options.
- Select one graphics backend and a VR API for the experiment.
- Record the exact CS2 build, headset runtime, GPU, and graphics settings.

**Done when:** a short technical note identifies the candidate camera and rendering integration points, remaining unknowns, and the first experiment.

### 2. Head-tracked camera on a paused demo

- Open a local demo and pause on a known scene.
- Apply headset rotation and translation to a spectator camera.
- Validate axes, handedness, world scale, and recentering.

**Done when:** turning and leaning move the camera consistently while the demo remains paused. This milestone alone is not full VR.

### 3. True stereo rendering

- Render separate views with the runtime-provided eye poses and projections.
- Keep both eyes on the same demo state.
- Submit both images to the headset.
- Check depth, clipping, visibility, shadows, particles, smoke, and temporal effects.

**Done when:** a paused scene has stable binocular depth and correct head response, with no eye mismatch or simulation advancement between views.

**Feasibility gate:** if the current engine cannot render two correct views without unacceptable instability or cost, document the evidence before expanding scope. Do not silently replace stereo with a flat virtual screen.

### 4. Continuous demo playback

- Resume the match while keeping headset tracking responsive.
- Handle pause, seek, map reload, and VR session loss.
- Measure CPU/GPU frame times, dropped frames, and reprojection.
- Target stable native rendering at a selected Quest refresh rate, starting with 72 Hz; record hardware, resolution, and settings with results.

**Done when:** a complete round plays with synchronized eyes, usable head tracking, and documented performance and visual limitations.

### 5. Usable spectator prototype

- Add observation-point selection, recentering, and basic playback controls.
- Provide a readable VR control surface or overlay.
- Package a reversible Windows setup and removal workflow.
- Publish reproducible setup steps and a demonstration recording.

**Done when:** another user with the documented hardware and software can install the prototype, watch a local demo in VR, and remove it.

## Main technical risks

| Risk | What must be verified |
| --- | --- |
| Source 2 stereo integration | Two independent views can be rendered from one simulation state. |
| Temporal rendering | History buffers, post-processing, smoke, and other effects remain correct for each eye. |
| Visibility and camera handling | Culling and clipping use the correct eye views and support positional tracking. |
| Performance | Rendering twice leaves enough CPU and GPU time for comfortable headset updates. |
| Runtime synchronization | Pose prediction, texture ownership, and frame submission remain consistent. |
| Game updates | Unsupported builds are detected; integration assumptions can be revalidated. |
| Comfort | Camera motion stays under spectator control, with stable world scale and recentering. |

## Operating boundaries

Development and initial use are restricted to local demo playback in a dedicated CS2 launch with `-insecure`. The project must not bypass anti-cheat, hide injected modules, or attach to competitive matchmaking sessions.

This restriction is a project boundary, not a guarantee against account sanctions. `-allow_third_party_software` is not a substitute for isolating the prototype from protected online play. See [Valve's Trusted Mode documentation](https://help.steampowered.com/en/faqs/view/09A0-4879-4353-EF95).

Do not redistribute CS2 binaries, maps, or other Valve assets. The user supplies their installed game and demo files.

## Future work

Once local replay viewing works:

- Player-follow cameras with independent head orientation.
- Tabletop viewing with adjustable world scale.
- Bookmarked moments and spectator viewpoints.
- Live spectating, subject to authorized feed access, delay, and a separately validated operating model.

## References

- [vr-meta/portal2vr](https://github.com/vr-meta/portal2vr) - existing integration in this organization.
- [Gistix/portal2vr](https://github.com/Gistix/portal2vr) - upstream Portal 2 VR project.
- [AdvancedFX](https://github.com/advancedfx/advancedfx) - engine and spectator-camera integration reference.
- [Khronos OpenXR Guide](https://github.com/KhronosGroup/OpenXR-Guide) - runtime integration and frame submission.

This is an independent experimental project, not an official Valve or Meta product.
