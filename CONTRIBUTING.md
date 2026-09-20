# Working on cs2-vr-spectator

How to get from a clean Windows machine to a build you changed yourself running in a
headset, and what to know before sending it back. If you only want to *use* it, the README
and a release zip are all you need; this is for changing it.

**The boundary applies to development too.** This loads a DLL into CS2 started with
`-insecure`. Own demo files and offline play only — never matchmaking, never a
VAC-protected server, and never commit or attach anything of Valve's (binaries, maps,
models, demo files you do not have the right to share).

## What you need

| | |
| --- | --- |
| Windows 10/11 x64, Counter-Strike 2 from Steam | |
| A headset with an OpenXR runtime | Developed on a Meta Quest 3 over Link. Much of the work needs no headset — see *Two kinds of launch*. |
| Visual Studio 2022 **Build Tools**, C++ x64 workload, Windows 11 SDK | The full IDE works too. |
| **.NET Framework 4.6.2 Targeting Pack** | Not optional: HLAE's shader builder is a C# project, and without this the build fails with `MSB3644`. |
| CMake 3.24+, Ninja, Rust (stable, x64), Git | |
| GitHub CLI (`gh`) | Only to make releases. |

`scripts\check-toolchain.ps1` reports what is missing and `scripts\install-toolchain.ps1`
installs it. CMake and Ninja land in the *user* PATH — open a new shell afterwards.

If PowerShell refuses to run the scripts, this machine's execution policy is the default
one: `powershell -ExecutionPolicy Bypass -File scripts\<name>.ps1`.

## How the source is laid out, and why it builds somewhere else

This repository does not contain HLAE. The hook is HLAE's `AfxHookSource2` (advancedfx,
MIT) with two things added, and they are kept apart on purpose:

- `src\AfxHookSource2\` — every file this project wrote. They are **copied into** an
  advancedfx clone to build. `MirvVrMath.h` is the pure part: no engine, no Windows, no
  OpenXR, so it builds alone and is what the unit tests cover.
- `docs\patches\*.patch` — the only edits to advancedfx's own files. A 900-line source file
  is not reviewable as a diff, so ours are files and only the edits to theirs are patches.
- `tools\launcher\` — `cs2vr.exe`. Its decisions are in `LauncherLogic.h`, tested the same way.
- `tools\server\` — a control server on loopback: `/state`, `/log`, `/command`, for a browser
  page and for an agent. Rust, no dependencies, and it attaches to a running session rather
  than starting one. It reads `console.log` and writes the hook's pipe, so it is the one part
  that can be developed against a recorded log instead of a headset.
- `scripts\` — development launch, measurement and check scripts. `scripts\cs2\vr*.cfg` are
  the configs a release ships; `exp*.cfg` are records of experiments.
- `docs\experiments\` — one note per question asked of the engine, with the answer and how
  it was obtained. Read the relevant ones before changing what they settled.

## Building the hook

The full recipe, with the two build failures that do not say what is wrong, is in
[`docs/install.md`](docs/install.md). In short:

```powershell
# The OpenXR SDK package: headers to build against, and the loader that ships beside the
# hook. There is no default location - the configure step stops and asks for this.
Invoke-WebRequest https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-1.1.63/OpenXR.Loader.1.1.63.nupkg -OutFile openxr.zip
Expand-Archive openxr.zip -DestinationPath D:\Dev\cs2-vr-tools\openxr\pkg     # <openxr sdk root>

git clone https://github.com/advancedfx/advancedfx.git D:\Dev\cs2-vr-tools\advancedfx
cd D:\Dev\cs2-vr-tools\advancedfx
git checkout v2.192.2                      # the revision the patches are written against
Copy-Item <repo>\src\AfxHookSource2\* .\AfxHookSource2\ -Force
git apply <repo>\docs\patches\001-vswhere-products.patch
git apply <repo>\docs\patches\002-per-pass-camera.patch
cmake --preset x64-release -DAFXVR_OPENXR_DIR=<openxr sdk root>
$env:Path = "$PWD\build\x64-release\ShaderBuilder;$env:Path"
cmake --build --preset x64-release --target shaderbuilder
cmake --build --preset x64-release --target AfxHookSource2
```

**The advancedfx tree is CRLF.** Never run `sed -i`, a formatter, or anything else that
rewrites line endings over it: a two-line change becomes a 262-line diff and the patches
stop applying. When you change one of advancedfx's files, regenerate the patch from a tree
that already has the earlier patches applied, and run `scripts\check-patches.ps1`.

The built DLL goes into a copy of an HLAE release (`hlae-selfbuilt\x64\`), next to the DLLs
it imports. Do not rename it and do not move it out of `x64\`: it finds its shaders from
its own path, and finds itself by name
([experiment 21](docs/experiments/21-what-the-zip-has-to-contain.md)).

## Building the launcher and a release folder

```powershell
cmake -S tools\launcher -B build\launcher -A x64
cmake --build build\launcher --config Release

scripts\stage-release.ps1 -HookTree D:\Dev\cs2-vr-tools\hlae-selfbuilt `
                          -Launcher build\launcher\Release\cs2vr.exe -Version 0.0.0-dev
build\release\cs2-vr-spectator-0.0.0-dev-cs2-<build>\cs2vr.exe check
```

`cs2vr check` looks at the machine and starts nothing. It is the quickest way to find out
whether a staged folder is whole.

## Tests

```powershell
cmake -S tests -B build\tests -DCMAKE_BUILD_TYPE=Release
cmake --build build\tests --config Release
ctest --test-dir build\tests -C Release --output-on-failure
scripts\check-cfg.ps1          # every key bound once, every bind announced, every exec resolves
scripts\check-patches.ps1      # the patches apply, in order
cargo test --manifest-path tools\server\Cargo.toml
```

The `--config` and `-C` matter. CMake's default generator on Windows is multi-config, so
without them the build is Debug and `ctest` reports the tests as "Not Run" — which looks
like a broken test and is a missing flag. CI runs the same things on Linux plus the full
Windows build of the hook.

Anything that can be expressed as arithmetic or string handling goes into `MirvVrMath.h` or
`LauncherLogic.h` **with a test**. It is the only part of this project that can be verified
without a headset, and most of the bugs that cost an evening were in code that could have
been there.

**If a test parses something, its fixture is copied from a real file and the test says which
one.** The control server's first four faults were all one mistake: a grammar written from a
tidied example rather than from an artefact, which is how a parser came to match only the
lines that happen to have no timestamp. A fixture that reads the way the parser wants proves
nothing about the file it will meet.

## Two kinds of launch

They look identical from outside, which has already cost a session. Say which one you are
starting if anyone else is in the room.

- **Worn**: `scripts\start-vr.ps1` (or a staged `cs2vr.exe`). Meta's runtime for that
  process, a frame cap, full eye size, the session starts by itself. SteamVR must **not** be
  running beside it: on a Quest over Link it holds the headset and the game is never given
  a frame. Shut down in order — session, then CS2, then any runtime.
- **Desk**: `scripts\launch-cs2-experiment.ps1 -ExecCfg exp<NN>_<name>`. No headset needed:
  `mirv_vr_xr passes 2` renders the eye passes with no session and the monitor shows the
  last one, which is how stereo bugs get photographed. Never use `vr.cfg` for a desk
  launch — it can start a session.

There is **no console in a worn launch**: the window is taller than the display, Windows
clamps it, and the console's input line is off-screen. Use `scripts\send-command.ps1
"<console line>"` (it writes to the hook's pipe, `\\.\pipe\cs2vr`), a key bind, or a config.
`cfg\cs2vr\vr_layout.cfg` is re-read live with PgDn then PgUp. The hook's side of every
conversation is in `<CS2>\game\csgo\console.log`, on lines starting `AFXVR:`.

## Things that were each paid for

- **Nothing unmeasured is on by default.** A write into the game's memory that has not been
  measured ships behind a command, off. One that was not cost a black screen at seven
  frames a second in someone's headset.
- **Log what a decision was made from, not only the decision.** "Menu mode" is useless in a
  log; "no world (playing demo 0, level <empty>)" found three bugs in five minutes each.
- **A frame carries its own data.** Anything the render thread needs about a frame travels
  with that frame (the ticket), never through a global the engine thread has moved on from.
- **Only a worn eye can judge stereo, scale and comfort.** When reading cannot settle a
  number, put it on a controller as a dial, hand it over, and read the value off the log.
- **Say what was measured and what was not.** The experiments directory is the record of
  confident explanations that turned out wrong. Add to it rather than around it.

## Sending a change

Branch from `main`; keep a change to one idea. Commit subjects here are plain sentences
saying what is now true (`fix: the camera was frozen wherever the session started`), and
the body says how it was found and how it was checked — worn, at a desk, or by a test, and
which. If a change touches the view struct, the frame loop or the input path, say what you
wore it for and for how long. A new question about the engine gets a note in
`docs\experiments\` with the method, so the next person can repeat it.

After a CS2 update the field offsets in `MirvVr.cpp` may move. The hook refuses to write
when the values stop looking like a camera; re-measuring is described in
[`docs/05-view-setup-point.md`](docs/05-view-setup-point.md), and a release is tied to one
CS2 build for exactly this reason.

## The licence

[Apache License 2.0](LICENSE). Use it, change it, ship it, sell it; keep the
[`NOTICE`](NOTICE) file with whatever you distribute, say which files you changed, and
leave the copyright headers you found alone. **By sending a change you are offering it
under the same licence** — that is Apache 2.0 section 5, and there is no separate CLA to
sign.

Two things that follow and are easy to get wrong:

- **Do not paste in code you did not write** unless its licence allows it and you add it to
  [THIRD-PARTY.md](THIRD-PARTY.md) in the same change. A file with somebody else's
  copyright in it is not covered by our LICENSE and the whole point of that file is that
  the list is true.
- **A patch under `docs/patches/` changes advancedfx, which is MIT and not ours.** If it is
  worth having upstream, offer it upstream; nothing here stands in the way, and a fix that
  lands in HLAE is one this project stops having to carry.
