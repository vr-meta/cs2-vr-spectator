# Experiment 08 — does OpenXR come up inside CS2?

Date: 2026-09-18. Self-built hook, CS2 build 2000908.

Phase D step 3, deliberately the smallest testable piece of the bridge: bring up an
OpenXR instance inside the game's process and ask the runtime what it is. No session, no
swapchain, no frames. If this had failed there would have been no point writing the rest.

## How the loader is opened

`openxr_loader.dll` is opened by explicit path with `LoadLibraryW` and every entry point
resolved through `xrGetInstanceProcAddr`, rather than linking the import library.

Linking would make `AfxHookSource2.dll` refuse to load whenever the loader is not on the
game's DLL search path — and this DLL is injected into a process that knows nothing about
OpenXR, so that is the normal case, not the exception. The failure would also be silent
and at load time, which is the worst place for it.

SDK: OpenXR 1.1.63, unpacked to `D:\Dev\cs2-vr-tools\openxr`. Only the headers are needed
at build time; `AFXVR_OPENXR_DIR` in `AfxHookSource2/CMakeLists.txt` points at them.

## Result

First run, with SteamVR up but no headset attached:

```
AFXVR: runtime "SteamVR/OpenXR" version 2.17.…
AFXVR: no head mounted display available (XR_ERROR_FORM_FACTOR_UNAVAILABLE).
```

Second run, Quest 3 connected through Link:

```
AFXVR: runtime "SteamVR/OpenXR" version 2.17.10
AFXVR: system "SteamVR/OpenXR : oculus", max swapchain 8192x8192, 16 layers
AFXVR: view 0 recommended 2528x2780 (max 8192x8192), 1 samples
AFXVR: view 1 recommended 2528x2780 (max 8192x8192), 1 samples
AFXVR: instance up. Game D3D11 device: 000002226E2C06F0
```

The loader loads, `xrCreateInstance` succeeds with `XR_KHR_D3D11_enable`, the
hand-resolved function table works, the headset is found through Link, and the game's
D3D11 device is in hand. Everything this step can establish is established.

**2528x2780 per eye** is what SteamVR asks for — above the Quest 3's native ~2064x2208,
which is SteamVR's own supersampling. Nothing forces us to match it: the plan is to
render at the window size and report the fov actually used. The first test runs at
1080x1080 per eye.

## Two operational findings that cost the session time

**Never kill the runtime while an instance is live.** SteamVR was force-closed with an
`XrInstance` still open inside CS2, and the game hung — alive, burning a core, not
pumping messages. `mirv_vr_xr stop` first, then anything else.

**An uncapped game and a VR compositor fight over the GPU and both lose.** With SteamVR
rendering its own scene at 90 Hz and CS2 rendering hundreds of frames a second, the game
stopped responding and the headset stuttered. `scripts/launch-cs2-experiment.ps1` gained
`-VrReady`, which sets an eye-sized window and `fps_max 90`.

**SteamVR without a headset makes the machine unusable** — it takes foreground focus and
gives nothing in return, as the first run shows. Start it only once the Quest is actually
in Link. Same class of problem as the focus loss in `send-key.ps1`.

## Next

With a headset attached, `xrGetSystem` should succeed and
`xrEnumerateViewConfigurationViews` will report the recommended per-eye resolution, which
is the number the window size has to be set against. Then:

- `xrCreateSession` with `XrGraphicsBindingD3D11KHR` on `g_pDevice`.
- A reference space, and swapchains matching the back buffer.
- `xrWaitFrame` / `xrBeginFrame` at the top of the pass loop, `xrLocateViews` filling the
  two eyes through `AfxVr_SetEye`, `xrEndFrame` after them.
- Submission: `CopyResource` into the acquired swapchain image, from the render command
  queue's `BeforePresent` hook, which already hands over the device context and the
  finished texture.
