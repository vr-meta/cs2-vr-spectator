# CS2 VR Spectator

Watch Counter-Strike 2 match replays from inside the map using a Meta Quest 3 connected to a Windows PC.

## Status

There is no working CS2 VR integration or installable build yet. Native stereo rendering inside the current CS2 engine is the main feasibility question to resolve.

Milestone 1 is in progress. Desk research is written up; nothing has been run against CS2 yet.

- [`docs/environment.md`](docs/environment.md) - the reference machine, headset runtimes, and toolchain state.
- [`docs/01-source2-integration-points.md`](docs/01-source2-integration-points.md) - candidate integration points, licensing, and open questions. Notable finding: CS2 ships unused stereo convars in its demo playback path.
- [`docs/02-hlae-multipass-analysis.md`](docs/02-hlae-multipass-analysis.md) - HLAE already re-renders the CS2 scene several times per frame from one simulation state, which is the core of what stereo needs.
- [`docs/03-vr-bridge-sketch.md`](docs/03-vr-bridge-sketch.md) - where the per-eye textures would come from, and the problems that sketch has to survive.
- [`docs/experiments/00-stereo-cvar-probe.md`](docs/experiments/00-stereo-cvar-probe.md) - the first experiment, ready to run once CS2 finishes installing.

Run [`scripts/check-toolchain.ps1`](scripts/check-toolchain.ps1) to see what the machine is missing; [`scripts/install-toolchain.ps1`](scripts/install-toolchain.ps1) installs it.

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
