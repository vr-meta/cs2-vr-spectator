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
| Install state | downloading as of 2026-09-18 (StateFlags 1026, ~60 GB total) |
| Build id | TBD — record from `version` in-game once installed |

Free space on `D:` was 77 GB against a 59.8 GB download. Tight but sufficient.

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

Nothing is installed yet. All of the following are required before the first build:

- Visual Studio 2022 Build Tools, C++ x64 workload (no `cl`, no `vswhere`)
- CMake (absent)
- Ninja (absent)
- Vulkan SDK (`VULKAN_SDK` unset)

Present: Git, `gh` (authenticated), Python (Windows Store shim).

## Reproducing this snapshot

```powershell
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
(Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1').ActiveRuntime
Get-Service OVRService, OVRLibraryService
(Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath
```
