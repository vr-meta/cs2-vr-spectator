# The view setup point — where a per-pass camera has to go

Date: 2026-09-18. Source read of `AfxHookSource2/main.cpp` in the cloned tree at
`D:\Dev\cs2-vr-tools\advancedfx`. Not yet modified or tested.

This is the answer to "where does a render pass obtain its camera", which experiment 03
left open. Phase B of `04-plan.md` starts here.

## The function

`main.cpp:637` — `CS2_Client_CSetupView_Trampoline_IsPlayingDemo(void *ThisCViewSetup)`,
installed at `main.cpp:1088`. It is HLAE's hook on the engine's view setup, and it is
where every camera override in HLAE is actually applied.

Field offsets into `CViewSetup` (build 2000908 — these will move on game updates):

| offset | field |
| --- | --- |
| `+0x434` | width (int) |
| `+0x43C` | height (int) |
| `+0x498` | fov (float) |
| `+0x4a0` | view origin (float[3]) |
| `+0x4b8` | view angles (float[3]) |

## What it does, in order

1. Reads the game's own camera into locals `Tx,Ty,Tz, Rx,Ry,Rz, Fov`, and stores it in
   `g_MirvInputEx.GameCamera*`.
2. Applies overrides in sequence, each setting `originOrAnglesOverriden` if it fires:
   - `g_CamPath` (campath evaluation)
   - `g_S2CamIO.GetCamImport()` (imported camera data)
   - `MirvFovOverride(Fov)`
   - **`g_MirvInputEx.m_MirvInput->Override(...)`** — this is `mirv_input`
   - `AfxHookSource2Rs_OnCViewRenderSetupView(...)` — the Rust/JS scripting hook
3. Exports to `CamIO` if export is running.
4. **If anything overrode, writes the locals back** into `pViewOrigin`, `pViewAngles`,
   `*pFov`, and sets `g_bViewOverriden`.
5. Saves the result into `g_MirvInputEx.LastCamera*`.

## Why experiment 03 failed, precisely

`mirv_input position` sets state consumed at step 2 of *this* function. This function
runs when the engine sets up the view for a frame. The `beforeCommands` of a stream run
later, inside the pass loop in `RenderServiceHooks.cpp`, by which time steps 1–4 have
already executed and the view is written.

So the command did dispatch — the absence of an `AFXWARNING` proved that — it simply
arrived after the only consumer had read its input.

## What phase B has to do

Give each stream a camera offset and apply it here, at step 2, as one more override in
the chain. The pieces needed:

- **Knowing which pass is being set up.** The pass loop lives in
  `RenderServiceHooks.cpp` (`My_Engine2_RenderService_OnClientOutput`), which iterates
  `m_RecordingExtraPasses`. Something must expose "the stream currently being rendered"
  to this trampoline — a global set by the pass loop is the crude version and probably
  enough to prove the concept.
- **A per-stream offset.** A position delta, and later a full pose, stored in
  `CStreamSettings` next to the existing per-pass settings.
- **Applying it in the right space.** The offset must be perpendicular to the view
  direction, not along a world axis, or the two eyes will not be side by side. Angles are
  available in the same struct (`Rx,Ry,Rz`), so the right vector can be derived here.

**Open question that decides the difficulty:** does this trampoline run once per frame, or
once per render pass? If once per pass, the change is small — read the current stream's
offset and add it. If once per frame, the view is shared across passes and something
deeper is needed, likely re-invoking the engine's view setup per pass.

This is the first thing to determine in phase B, and it is cheap to answer: add a counter
or a log line to the trampoline, rebuild, record two streams, and count the calls.

## Reminder

The self-built DLL is at
`D:\Dev\cs2-vr-tools\advancedfx\build\x64-release\AfxHookSource2\Release\AfxHookSource2.dll`
and is staged into `D:\Dev\cs2-vr-tools\hlae-selfbuilt\x64\`. Build recipe and the two
environment obstacles are in [`patches/README.md`](patches/README.md).
