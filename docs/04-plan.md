# Plan after the reconnaissance phase

Date: 2026-09-18, written after experiments 00–02. Supersedes the sequencing implied by
the README milestones, which assumed the engine might do the stereo work for us.

## What the experiments changed

The README planned milestone 2 (head-tracked camera) before milestone 3 (stereo), on the
assumption that stereo was the risky part. That turned out backwards in an unexpected
way: **stereo rendering is the part that already works**, and the missing piece is a
small, specific connection between two working mechanisms.

| piece | status |
| --- | --- |
| render one frozen frame twice, independently | works — experiment 02 |
| move the camera exactly and repeatably | works — experiment 01 |
| move the camera *per pass* | missing, needs code |
| get pass output as a GPU texture instead of a file | not started |
| headset poses in, frames out | not started |
| fit it all in a frame budget | unknown, the main risk |

Everything before the first gap was configuration of existing tools. Everything after it
is development.

## Phase A — build HLAE from source

Not interesting in itself, but nothing downstream is possible without it, and it is the
first real test of the toolchain.

`BUILDING.md` lists a heavy dependency set for a full release — Node.js, Python with pip,
GNU gettext, Rust with a 32-bit target, and Visual Studio Community with the .NET
workload. **Most of that is for the GUI, injector and Win32 hooks, none of which this
project touches.**

Only `AfxHookSource2.dll` (x64) matters here, and the build system supports that directly:

```batch
cmake --preset x64-release
cmake --build --preset x64-release
```

with `-DAFX_MULTIBUILD_STAGING_X64=source2` to limit staging to the Source 2 hook family.
That drops Node.js and gettext from the requirements.

**Correction:** the .NET pieces are *not* droppable, contrary to the first version of this
plan. The x64 hook depends on `ShaderBuilder`, a C# project used to compile shaders, so
the build fails without the .NET Framework 4.6.2 Targeting Pack:

```
error MSB3644: The reference assemblies for .NETFramework,Version=v4.6.2 were not found.
```

`BUILDING.md` lists that component; it was dismissed as GUI-only and it is not.

Still needed:

- **Rust toolchain** (`cargo`, `rustc`) — not optional even for the hook alone:
  `AfxHookSource2` contains `AfxHookSource2Rs.cpp`, `CVarRs.cpp` and `ConsoleRs.cpp`,
  which bind to Rust components built through Corrosion. Neither is installed.
- **MSBuild** — `CMakeLists.txt` locates it via `vswhere -requires
  Microsoft.Component.MSBuild`, and that query currently returns nothing. The preset also
  uses the `Visual Studio 17 2022` generator rather than Ninja, so MSBuild is required
  regardless. The Build Tools install needs that component added.

The existing HLAE release stays installed as the reference: when the self-built DLL
misbehaves, the question "is this my build or my change?" should be answerable by
swapping one file.

**DONE 2026-09-18.** The self-built `AfxHookSource2.dll` loads into CS2 and reproduces
experiment 02: two streams, `eyeR` with `worldAction noDraw`, 80.4% of pixels differing
(release build: 74.1% on a different frame). Build recipe and the two environment
obstacles are in [`patches/README.md`](patches/README.md).

**Risk:** the build pulls protobuf and ABSL. If it fights back, the fallback is to patch
against the released binary rather than rebuild, which is worse but not fatal.

## Phase B — per-pass camera

**DONE 2026-09-18.** See [`experiments/04-per-pass-camera.md`](experiments/04-per-pass-camera.md).

The trampoline turned out to run **once per frame, outside the pass loop** — the
expensive answer. But the object it receives, `CViewSetup`, is persistent and *is*
re-read by every pass, so the eyes diverge when the pass loop rewrites it before each
pass. Control take 0.00 % differing, test take 88.89 % with correct parallax.

Two things the same trace turned up, both carried forward:

- Two streams cost **four** scene traversals, not three: the pass loop always starts one
  more pass than it needs. Worth removing before phase E measures anything.
- The trampoline reads `CViewSetup` back as "the game camera", so a leftover offset
  would accumulate. Fixed by restoring the base before reading.

The original analysis, kept because it explains the shape of the solution:

**UPDATED after experiment 03.** Option 1 below is void: concommands are already
accepted in `beforeCommands`, and putting `mirv_input position` there changes nothing,
because the view is resolved before per-pass commands run. Only the deeper change
remains:

1. ~~Allow concommands in `beforeCommands`~~ — already supported, and ineffective.
2. **Apply a per-stream camera offset inside the render path.** The exact location is
   identified in [`05-view-setup-point.md`](05-view-setup-point.md):
   `CS2_Client_CSetupView_Trampoline_IsPlayingDemo` in `AfxHookSource2/main.cpp:637`,
   where every HLAE camera override is applied. Now the only option.

~~**First question of phase B, and it decides the cost:** does that trampoline run once
per frame or once per render pass?~~ Answered: once per frame. The fix was not to make it
run per pass but to write the view it produced, per pass, from the pass loop.

**Done when:** one paused frame produces two images that differ *only* by a camera
translation, with correct parallax — near geometry shifting more than far. **Met.**

## Phase C — stereo pair on disk

**DONE 2026-09-18.** See [`experiments/05-stereo-pair.md`](experiments/05-stereo-pair.md).

At a real interpupillary distance (2.5 units = 63 mm) the pair differs only by viewpoint,
and the parallax matches the arithmetic. The decisive test was the temporal one: with
separation 0 on a *playing* demo with live smoke, the eyes stay identical to within
dithering (max 3 of 255) while consecutive frames differ by up to 98% and max 255. The
simulation does not advance between passes.

The one frame that deviated further put all of it in the kill feed — a UI fade driven by
wall-clock time — with the volumetric smoke identical to the byte.

Original intent, all of it met:

Before any VR hardware is involved, prove the images are correct.

- Left and right eye of the same frozen frame, at a real interpupillary distance.
- Check the content most likely to break: smoke, particles, tracers, shadows, and any
  temporal effects. Note 03 flags these; experiment 02 already produced a frame with
  smoke, so the test material exists.
- Verify no simulation advance between eyes — the frames must be identical apart from
  viewpoint.

**Done when:** a stereo pair can be viewed (cross-eyed or in any stereo viewer) with
correct depth and no eye mismatch. This is the README's milestone 3 feasibility gate,
answered on disk rather than in a headset.

**Why before VR:** every VR problem is harder to diagnose inside a headset. If the pair
is wrong here, it will be wrong there, and debugging it here is far cheaper.

## Phase D — VR bridge

**Step 1 done 2026-09-18:** per-eye field of view works, through the same write that
carries the camera — see [`experiments/06-per-eye-projection.md`](experiments/06-per-eye-projection.md).
That was taken first on purpose: a headset needs a different frustum per eye, and if the
projection had been stuck per frame, everything below would have been built on sand.

It also settles the shape of the frame submission. OpenXR's projection layer takes the
frustum angles that were *actually rendered*, so a symmetric frustum enclosing the
runtime's asymmetric recommendation is correct — wasted edge pixels, not a wrong image.
One fov number per eye is therefore enough, and intercepting the projection in the
constant buffer is not needed.

Remaining, in order:

1. **Per-pass angles** (`+0x4b8`, same struct). Same lever, unmeasured.
2. **Calibrate the fov number** against the projection matrix at `CViewRender+0x218`, so
   the angles reported to OpenXR match what was rendered.
3. **Resolution.** Passes render at window size; a Quest 3 wants ~2064x2208 per eye.
4. **OpenXR session** in the hook: instance, system, session on the game's D3D11 device,
   swapchains, reference space, `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`, and
   `xrLocateViews` at frame start feeding the per-pass camera.
5. **Frame submission**: replace the staging copy in `CAfxCapture::OnBeforeGpuPresent`
   with a `CopyResource` into the swapchain image. That function already receives both
   the device context and the finished texture.

Per note 03, the insertion point is where HLAE copies the back buffer
into a staging texture for file writing; VR needs a GPU-to-GPU copy into an XR swapchain
image instead.

- OpenXR session, swapchains, reference space, loss and recentering.
- `xrLocateViews` for per-eye pose and projection, driving the per-pass camera from
  phase B.
- Replace the staging copy with a swapchain copy.

Open decisions deferred to here, deliberately: OpenXR vs OpenVR, and whether to switch
the active OpenXR runtime from SteamVR to Meta for a Link-connected Quest.

**Known problem:** passes render at window resolution, not per-eye resolution. Note 03
records this as the most likely place for the sketch to break.

## Phase E — performance

The main risk of the whole approach, and it cannot be assessed before phase D.

A stereo frame costs the main pass plus two eye passes — three scene traversals. The
multi-pass path was built for offline capture, where a slow frame is harmless. 72 Hz
leaves 13.9 ms.

**Done when:** frame timings are measured and recorded against this machine and build,
whatever they say. If the budget cannot be met, that is the feasibility gate failing with
evidence, which the README explicitly asks for rather than a silent fallback to a flat
virtual screen.

## Carried-over issues

Small but recorded so they are not rediscovered:

- **Dragging the demo timeline crashes the game** (access violation, build 2000908).
  Milestone 4 requires seeking. Unknown whether it also happens without HLAE — worth
  five minutes to check before it becomes a blocker.
- `playdemo` over an already-playing demo crashes; restart the game instead.
- A locally recorded bot demo did not replay at all. Use GOTV demos.
- CS2 ships `rendersystemvulkan.dll`. Unexplored, and irrelevant while the HLAE hooks
  are D3D11-only, but it would change the VR bridge substantially if it were viable.

## Working rules adopted after the false negatives

Three measurements were discarded during experiments 00–02, each because something other
than the thing under test differed between captures. The habits that fixed it:

- **Verify the subject is alive before measuring its response.** A frozen demo and a dead
  convar produce identical screenshots.
- **Choose indicators whose absence is unmissable.** The whole map disappearing cannot be
  confused with anything else; a weapon model that is not on screen can.
- **Read back what a command actually did.** An invalid value prints help and silently
  leaves the setting unchanged.
- **Always capture a return to baseline** and require it to be pixel-identical. A test
  that cannot reproduce its own starting point is not measuring what it thinks it is.
