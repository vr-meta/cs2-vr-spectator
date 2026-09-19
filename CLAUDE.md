# cs2-vr-spectator

Watch Counter-Strike 2 demos from inside a VR headset. While a VR session is running, CS2
renders each frame three times through HLAE's multi-pass loop (once otherwise): the main
pass, numbered 0, and two extra passes, numbered 1 and 2, which get an eye camera written
into the engine's view struct and are submitted to OpenXR as a stereo projection layer. Developed and worn on
a Meta Quest 3 over Link, Oculus PC runtime.

**The boundary, before anything else:** this injects a DLL into CS2 started with
`-insecure`. Own demo files only. Never matchmaking, never a VAC-protected server, never
redistribute Valve's binaries, maps or assets. Nothing here may make starting without
`-insecure` possible.

## Layout

- `src/AfxHookSource2/` — everything this project wrote. It is **copied into** a clone of
  advancedfx (`D:\Dev\cs2-vr-tools\advancedfx`) to build. `MirvVr*` is the per-pass camera,
  `MirvVrXr*` is OpenXR, `MirvVrMath.h` is pure logic with no engine, Windows or OpenXR.
- `docs/patches/` — the only edits to advancedfx's own files (`main.cpp`, render hooks).
  The advancedfx tree is CRLF: never run `sed -i` or anything that rewrites line endings
  over it, and regenerate a patch from a tree that has the earlier patches applied.
- `tests/` — builds `MirvVrMath.h` alone. `scripts/` — launch, measurement and check
  scripts. `docs/experiments/` — one note per question asked of the engine, with the answer.
- `docs/07-release-plan.md` — where this is going: a launcher exe and GitHub releases.

## Commands

```
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
pwsh scripts/check-cfg.ps1        # every key bound once, every bind announced
pwsh scripts/check-patches.ps1    # patches apply in sequence
```

The `--config` / `-C` are not decoration. On this Windows machine CMake defaults to the
Visual Studio generator, which is multi-config: `CMAKE_BUILD_TYPE` is ignored, a bare
`cmake --build` produces Debug, and a bare `ctest` then reports the test as "Not Run" and
0% passed — which reads like a broken test and is a missing flag. CI runs on Linux, where
the short form works, so copying the workflow's line is how this goes wrong. If PowerShell
refuses the scripts, run them as `powershell -ExecutionPolicy Bypass -File <script>`.

Building the hook itself: `docs/install.md` and `docs/patches/README.md`. CI
(`.github/workflows/build-hook.yml`) runs all of the above plus the Windows build.

## Rules that were each paid for

- **A worn session starts only with `scripts\start-vr.ps1`.** Meta runtime for that process,
  frame cap, explicit 2528x2780. Desk measurements use `launch-cs2-experiment.ps1` with an
  `exp*.cfg`, never `vr.cfg` (it can start a session), and the operator is told in chat
  before a desk launch — from outside the two look identical.
- **SteamVR must not be running next to a Meta-runtime session.** It holds the headset and
  our session is never scheduled. Shut down in order: session, CS2, then any runtime.
- **There is no console in a worn launch.** The window is taller than the display, Windows
  clamps it, and the console's input line is off-screen. Anything the operator adjusts goes
  on a key, a controller gesture or a config. `vr_layout.cfg` is re-read live with PgDn
  then PgUp (`scripts/send-key.ps1` can press them).
- **`vr_keys.cfg` and `vr_diag.cfg` have a bind checker** with reserved keys. Run
  `check-cfg.ps1` after touching any cfg; do not hand-edit around it.
- **Pure logic goes in `MirvVrMath.h` with a test.** It is the only part that can be
  verified without a headset.
- **The user is the operator and the instrument.** Only a worn eye can judge stereo, scale
  and comfort. When reading cannot settle something, build a dial, put it on the
  controller, and read the value off `console.log` afterwards.
- **Report what was measured, and say when something was not.** Several confident
  explanations in this project's history were wrong; the experiments directory is the
  record of finding out.

## Facts about the engine and runtime (do not re-derive)

- View struct = `CViewRender+0x10`: fov `+0x498`, origin `+0x4a0`, angles `+0x4b8`, for CS2
  build 2000908. The hook checks `steam.inf` and refuses to write when values stop looking
  like a camera.
- The fov field means two things: at the once-per-frame SetUpView trampoline it is the
  4:3-convention number; **at pass time it is the true horizontal fov of the image**.
  Measured worn, to about one degree.
- The Oculus PC runtime reports `fovMutable = false` and ignores a submitted fov. We render
  symmetric and submit the runtime's own frustum with `imageRect` cropped to it.
- The head (orientation, stick offset) is written once per frame at the trampoline for
  audio, culling and the client's matrices; only the ±IPD/2 eye offset is per pass. The
  base camera is restored before the engine's read **only if the engine has not written a
  fresh one** — restoring unconditionally froze the viewer in place.
- Each frame carries the pose it was rendered from (a ticket captured on the engine
  thread); globals shared with the render thread race.
- The demo's spectator controls react to **key events**, not to console `+attack`. The
  hook sends real key presses.
- TrueView (`cl_demo_predict`) fights the three passes and freezes the watched player:
  it stays `0`.
- The UI is composited once per pass. Eyes capture before it; the main pass, with the world
  cleared, carries the HUD to quad layers that follow the head's position and keep a
  room-fixed direction.
- Name tags are laid out once per frame by Panorama; calling the engine's matrix builder
  mid-pass restores the base camera (experiment 15).

## Style

Match the surrounding code: comments explain *why* and what went wrong before, not what
the line does. Commit subjects are plain sentences (`fix: the camera was frozen wherever
the session started`). Docs and code are English.
