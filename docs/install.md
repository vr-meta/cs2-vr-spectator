# Installing and running

How to get from a clean machine to CS2 rendering into a headset. Written from the machine
this was built on; paths are examples, not requirements.

**Read the boundary first.** This runs CS2 with `-insecure` and injects a DLL. That is
fine for watching your own demo files and nothing else. Do not attach it to matchmaking,
do not join VAC-protected servers with it, and do not redistribute Valve's binaries, maps
or assets. HLAE is a hack by design; the `-insecure` flag is what keeps this honest.

## What you need

| | |
| --- | --- |
| Windows | 10 or 11, x64 |
| CS2 | a legitimate Steam install |
| Headset | tested with a Meta Quest 3 over Link; any OpenXR runtime should work |
| GPU | tested on an RTX 4070 Laptop — see [experiments/10-frame-budget.md](experiments/10-frame-budget.md) before assuming yours is enough |

Build tools, if you are compiling the hook rather than taking a released build:

- Visual Studio 2022 **Build Tools** with the C++ x64 workload and the Windows 11 SDK
- The **.NET Framework 4.6.2 Targeting Pack** — not optional, see below
- CMake 3.24+ and Ninja
- Rust (`rustup`, stable, x64)

`scripts/check-toolchain.ps1` reports what is missing; `scripts/install-toolchain.ps1`
installs it.

## 1. HLAE

Two copies, deliberately:

```
D:\Dev\cs2-vr-tools\hlae\            released HLAE 2.192.2, untouched, the reference
D:\Dev\cs2-vr-tools\hlae-selfbuilt\  the same tree with our AfxHookSource2.dll in x64\
```

Keeping both means "is this my build or my change?" is answered by swapping one file.
Download HLAE from <https://www.advancedfx.org/> and unpack it twice.

## 2. The hook

Clone advancedfx (MIT), copy in this project's own sources, and apply the patches that
edit advancedfx's files:

```powershell
git clone https://github.com/advancedfx/advancedfx.git D:\Dev\cs2-vr-tools\advancedfx
cd D:\Dev\cs2-vr-tools\advancedfx
Copy-Item D:\Dev\cs2-vr-spectator\src\AfxHookSource2\* .\AfxHookSource2\ -Force
git apply D:\Dev\cs2-vr-spectator\docs\patches\001-vswhere-products.patch
git apply D:\Dev\cs2-vr-spectator\docs\patches\002-per-pass-camera.patch
```

The split is deliberate: everything this project wrote is a normal source file in
[`../src/`](../src/), and only the edits to someone else's code are a patch. A 900-line
file is not reviewable as a diff.

**Do not apply these with anything that rewrites line endings.** The tree is CRLF and a
`sed -i` turned a two-line change into 262 insertions the first time.

Then the OpenXR headers — only the headers are needed at build time; the loader is opened
by path at runtime:

```powershell
$dst = 'D:\Dev\cs2-vr-tools\openxr'
New-Item -ItemType Directory -Force $dst
Invoke-WebRequest 'https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-1.1.63/OpenXR.Loader.1.1.63.nupkg' -OutFile "$dst\openxr.zip"
Expand-Archive "$dst\openxr.zip" -DestinationPath "$dst\pkg"
```

Build:

```powershell
$env:Path = "$env:USERPROFILE\.cargo\bin;$env:Path"
cd D:\Dev\cs2-vr-tools\advancedfx
cmake --preset x64-release
# ShaderBuilder is invoked by bare name, so its output directory has to be on PATH
$env:Path = "D:\Dev\cs2-vr-tools\advancedfx\build\x64-release\ShaderBuilder;$env:Path"
cmake --build --preset x64-release --target AfxHookSource2
```

If the OpenXR headers live somewhere else, pass
`-DAFXVR_OPENXR_DIR=<path>` to the configure step.

Copy the result into the self-built HLAE:

```powershell
Copy-Item D:\Dev\cs2-vr-tools\advancedfx\build\x64-release\AfxHookSource2\Release\AfxHookSource2.dll `
          D:\Dev\cs2-vr-tools\hlae-selfbuilt\x64\AfxHookSource2.dll
```

### Two build failures that do not say what is wrong

`error MSB3644: The reference assemblies for .NETFramework,Version=v4.6.2 were not found`
— `AfxHookSource2` depends on `ShaderBuilder`, which is a C# project. Install the
targeting pack.

`'ShaderBuilder.exe' is not recognized ... exited with code 9009` — exit code 9009 on
Windows means "command not found", not a shader problem. Put its build directory on PATH,
as above.

## 3. Configs

```powershell
Copy-Item D:\Dev\cs2-vr-spectator\scripts\cs2\vr.cfg `
          'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo\cfg\'
```

Put a demo where the game can find it — `game\csgo\pro_mirage.dem` in these examples. Use
a GOTV demo; a locally recorded bot demo did not replay at all.

## 4. Running

Start Steam. Put the headset on and start Link, so the runtime has a headset — **only
then** start SteamVR. Without one it takes foreground focus, makes the desktop unusable,
and gives nothing back.

```powershell
D:\Dev\cs2-vr-spectator\scripts\launch-cs2-experiment.ps1 -SelfBuilt -VrReady -ExecCfg vr `
    -Width 2528 -Height 2780 -Demo pro_mirage.dem
```

- `-SelfBuilt` injects the patched hook instead of the released one.
- `-VrReady` adds `-noborder` and caps the frame rate; an uncapped CS2 and a VR compositor
  fight over the GPU and both stall.
- `-Width`/`-Height` are the **per-eye** resolution: submission copies the back buffer, so
  the window size is the eye size. The window may exceed the display.
- `-ExecCfg vr` loads the key bindings at startup, because with a runtime up the game
  window stops taking typed input reliably.

HLAE exits immediately after injecting — that is normal. CS2 takes 40–60 s to appear.

Then press **F9** to connect to the headset. `game\csgo\console.log` should show:

```
AFXVR: runtime "SteamVR/OpenXR" ...
AFXVR: swapchains 2528x2780, back buffer format 27 -> swapchain 29, 3 images per eye.
AFXVR: submitting frames to the headset.
```

`scripts/send-key.ps1 -Key F9` presses it from outside, which is usually easier than
finding the keyboard with a headset on.

## Controls

Keyboard, from `vr.cfg`:

| | |
| --- | --- |
| F9 / F5 | headset on / off |
| F6 / F7 | free camera on / off |
| F8 / F10 | free look on / off |
| F1 | recentre |
| F4, F2 / F3 | pause, slow motion / normal speed |
| HOME / END | HUD off / on |
| INS / DEL | frame rate logging on / off |

Controllers:

| | |
| --- | --- |
| left stick | walk, in the direction you are looking |
| right stick | turn, and rise or descend |
| triggers | previous / next player |
| right grip | next camera mode |
| X | free look on / off |
| Y | back onto the player |
| A | pause |
| B | recentre |

## Shutting down, in this order

1. **F5** — stop the session.
2. Close CS2.
3. Close SteamVR.

Backwards is how you get an unkillable CS2: killing the runtime while an OpenXR instance
is live leaves the game alive, holding the hook DLL and the log, until a reboot.

## When something is wrong

[`workflow.md`](workflow.md) has the failures that cost this project time and how each was
recognised. The short list:

- **CS2 stops responding and the headset stutters** — the game and the compositor are
  fighting for the GPU. Cap the frame rate.
- **`XR_ERROR_FORM_FACTOR_UNAVAILABLE`** — SteamVR is up but has no headset. Link is not
  active.
- **`xrCreateSession` fails with `XR_ERROR_RUNTIME_FAILURE`** — restart SteamVR; it gets
  into this state after several sessions.
- **The game exits by itself** — check Steam is still running; CS2 goes with it.
- **The console opens but commands do nothing** — check the game is presenting at all. A
  stalled present starves the command handling too.
