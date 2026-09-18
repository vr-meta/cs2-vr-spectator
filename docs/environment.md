# Reference environment

Recorded 2026-09-18. This is the single machine all early results are measured on.
Milestone 1 requires the exact hardware and runtime versions to be pinned before any
rendering experiment, so that later performance numbers mean something.

## Hardware

| Component | Value |
| --- | --- |
| CPU | AMD Ryzen AI 9 HX 370 |
| RAM | 31.1 GB |
| dGPU | NVIDIA GeForce RTX 4070 Laptop |
| dGPU driver | 32.0.15.9144 (Vulkan driver reports 591.44.0) |
| iGPU | AMD Radeon 890M |
| OS | Windows 11 Pro 10.0.26200 |

This is a laptop with a hybrid GPU setup. Two virtual display adapters are also
present: Meta Virtual Monitor (15.52.32.370) and SuperDisplay Virtual Adapter.
Anything that enumerates adapters must not assume adapter 0 is the RTX 4070.

**Unverified:** whether Quest Link encoding runs on the RTX 4070 or falls back to
the Radeon 890M. Confirm before recording any frame-timing result.

## Headset and VR runtimes

| Item | Value |
| --- | --- |
| Headset | Meta Quest 3 over PC VR link |
| Meta runtime | `OVRService` running, `OVRLibraryService` stopped |
| SteamVR | installed at `D:\SteamLibrary\steamapps\common\SteamVR` (app 250820) |
| Active OpenXR runtime | `D:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json` |

The active OpenXR runtime is currently SteamVR, not Meta. For a Link-connected
Quest this adds a translation layer; switching the active runtime to Meta is worth
measuring once a baseline exists. Registry key: `HKLM\SOFTWARE\Khronos\OpenXR\1`.

## Game

| Item | Value |
| --- | --- |
| CS2 | app 730, `D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive` |
| Install state | installed 2026-09-18 (StateFlags 4), 67 GB on disk |
| ClientVersion | 2000908 |
| PatchVersion | 1.41.8.1 |
| SourceRevision | 10981323 |
| Version date | Sep 09 2026, 15:23:58 |

From `game/csgo/steam.inf`. Every result recorded against this build must cite it —
Source 2 plugins and offset-based hooks break on patches.

Render backends present in `game/bin/win64/`:

```
rendersystemdx11.dll     4.5 MB
rendersystemvulkan.dll   6.1 MB
rendersystemempty.dll    1.7 MB
```

A Vulkan backend does ship, contrary to what note 01 first assumed from the community
module dump. Whether it can actually be selected is untested.

**Not yet obtained:** `.dem` files. Milestone 2 cannot be validated without at least
one local demo on a known map.

## Working reference: portal2vr

[vr-meta/portal2vr](https://github.com/vr-meta/portal2vr) is installed and has been
run on this exact machine, which makes it a live reference rather than a code read.

From `D:\SteamLibrary\steamapps\common\Portal 2\portal2_d3d9.log`:

- DXVK build `279b4b`, translating Direct3D 9 to Vulkan.
- Built-in extension providers: Win32 WSI, **OpenVR**, **OpenXR**.
- Frames submitted through the OpenVR compositor interface.
- Ran on the RTX 4070 with Vulkan 1.4.325.

Note that the DXVK fork carries both an OpenVR and an OpenXR provider. The Portal 2
integration using OpenVR is a choice made in that project, not a constraint.

## Toolchain

Installed 2026-09-18 via `scripts/install-toolchain.ps1`:

| Tool | Version | Scope |
| --- | --- | --- |
| Ninja | 1.13.2 | user |
| CMake | 4.4.3 | user |
| Visual Studio 2022 Build Tools | C++ x64 workload + Windows 11 SDK 22621 | machine |

Already present: Git, `gh` (authenticated), Python (Windows Store shim).

Not installed: Vulkan SDK. CS2 is D3D11 only, so it matters solely for reading the
portal2vr DXVK reference. Skipped to avoid competing with the CS2 download for bandwidth.

Note on CMake: 4.x rejects `cmake_minimum_required` below 3.5. HLAE declares 3.24, so
it is unaffected, but other dependencies may not be.

Run `scripts/check-toolchain.ps1` to re-verify. A new shell is required after install
for the PATH changes to apply.

### Nothing needs building for the first experiment

Both tools experiment 00 depends on ship as binaries:

- HLAE `v2.192.2`, released 2026-09-12 — current with recent CS2 patches.
- cvar-unhide-s2 `v0.5.0`, released 2025-08-01.

The cvar-unhide plugin is over a year old, and Source 2 plugins break on game updates.
If it fails to load against the current CS2 build, it has to be rebuilt — which is the
first thing the toolchain will actually be used for.

## Reproducing this snapshot

```powershell
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
(Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1').ActiveRuntime
Get-Service OVRService, OVRLibraryService
(Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath
```

## Test demo

`game/csgo/pro_mirage.dem` — MOUZ vs Natus Vincere, mirage, from the HLTV demo package
for match 2398099 (StarLadder StarSeries Fall 2026). 308 MB, GOTV.

A locally recorded bot demo was tried first and **did not replay** — the console filled
with `Cannot process snapshot tick N, it is a delta from tick N-1, which we do not have`
and the scene was frozen. Measurements taken against it were void. Prefer a GOTV demo:
it also provides real smoke, grenades and movement, which is what stereo will break on.
