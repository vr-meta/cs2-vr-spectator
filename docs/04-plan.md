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

The one missing connection. Two candidate approaches, in order of preference:

1. **Allow concommands in `beforeCommands`.** Currently `CAfxStreams::ExecuteCommands`
   resolves every entry through `FindConVar` and assigns to a convar. Extending it to
   dispatch real commands would make `mirv_input position` usable per pass immediately —
   and is useful beyond this project, so it may be worth offering upstream.
2. **Add a camera offset to stream settings.** A per-stream position/rotation delta
   applied where the convar commands are applied today. More invasive, but does not
   depend on command dispatch being safe to call inside the render path.

**Done when:** one paused frame produces two images that differ *only* by a camera
translation, with correct parallax — near geometry shifting more than far.

**Risk:** the render pass may run on a thread or at a point where changing the camera is
not respected, because the view matrix for the frame is already resolved. If so, the
change moves deeper — into where the pass sets up its view — and gets more expensive.
This is the single most important unknown remaining.

## Phase C — stereo pair on disk

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

Only after phase C. Per note 03, the insertion point is where HLAE copies the back buffer
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
