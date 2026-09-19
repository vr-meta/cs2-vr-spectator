# Plan for something a stranger can install

Date: 2026-09-19, written the afternoon it became comfortable to wear. Until now "install"
has meant: clone two repositories, install a toolchain, apply patches, build, unpack HLAE
twice, copy a DLL, copy three configs by hand, and run a PowerShell script whose paths are
this machine's. That was right for finding out whether it works. It does. This is the plan
for the next person.

**The goal, stated as the README it should make true:**

> 1. Download `cs2-vr-spectator-<version>.zip` from Releases and unpack it anywhere.
> 2. Put the headset on Link. Close SteamVR.
> 3. Run `cs2vr.exe`, pick a demo, press **Watch**.

Nothing else. No PowerShell, no toolchain, no HLAE download, no copying configs, no
editing paths. Everything below exists to make those three lines honest — and the README is
rewritten to say exactly that, and only when it is true (see the last section).

## What the scripts do today, and who does it afterwards

Every line of `start-vr.ps1` / `launch-cs2-experiment.ps1` / `send-key.ps1` is a decision
somebody had to learn the hard way. None of them may be lost in the move.

| Today (script, by hand) | Afterwards |
| --- | --- |
| CS2 path hard-coded to `D:\SteamLibrary\...` | Launcher finds it: Steam path from the registry → `libraryfolders.vdf` → `appmanifest_730.acf` |
| HLAE unpacked twice under `D:\Dev\cs2-vr-tools`, our DLL copied in | Ships in the zip: the subset of HLAE the hook needs at runtime, with our `AfxHookSource2.dll` already in place. advancedfx is MIT; its licence ships too |
| `HLAE.exe -customLoader -noGui -autoStart ...` does the injection | Launcher injects itself (create suspended, `LoadLibraryW` in the target, resume) — about a hundred lines, and removes the .NET Framework dependency HLAE.exe brings. Falls back to the bundled HLAE loader if that proves fragile |
| `openxr_loader.dll` opened from `D:\Dev\...` or `AFXVR_OPENXR_LOADER` | Ships next to the hook; the hook opens it relative to **its own** module path. The hard-coded path in `kLoaderPaths` goes |
| `-MetaRuntime` sets `XR_RUNTIME_JSON` to a fixed Meta path | Launcher reads `HKLM\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes`, prefers Meta's when `OVRServer_x64` is running, sets it for the child only |
| Eye size typed as `-Width 2528 -Height 2780` | Launcher asks the runtime (`xrEnumerateViewConfigurationViews`, the code is already in `tools/xr-probe`) and passes what it says |
| Refuse if SteamVR or CS2 is running; warn if Link is not up | Same checks, as status lights before **Watch** is enabled — each with the one-sentence reason from `cs2-vr-runtime-handling` |
| `+fps_max 90`, `-insecure`, `-windowed -noborder`, explicit `-w/-h` | Always, not optional. `-insecure` is not a setting; the launcher has no way to start without it |
| `vr.cfg`, `vr_keys.cfg`, `vr_layout.cfg` copied into `game\csgo\cfg` by hand | Launcher writes them to `game\csgo\cfg\cs2vr\` on every start (`+exec cs2vr/vr`) and offers to remove them. Nothing of ours is left loose in Valve's directories |
| Wait for `AFXVR: CS2 build` in `console.log`, sleep 20 s, synthesise F9 | The hook starts the session itself when told to (`AFXVR_AUTOSTART=1` in the child's environment) once the first plausible view has been read. No log scraping, no synthetic keys, no focus dependence |
| `send-key.ps1` presses PGDN/PGUP to re-read the layout; there is no console in a worn launch | A control pipe (`\\.\pipe\cs2vr`): launcher writes a console line, the hook queues it with the existing `QueueCommand`. That is a remote console, which the worn launch has never had, and it makes layout sliders trivial |
| Demo named on the command line, must already sit in `game\csgo` | File picker and drag-and-drop; recent list. Verify first whether `playdemo` takes an absolute path — if not, the launcher hard-links or copies into `game\csgo\cs2vr_demos\` |
| Supported CS2 build is a `#define`; a game update means a rebuild | Stays a build-time fact for now, but the launcher reads `steam.inf` **before** starting and says "this release was made for build X, you have Y" in words. The hook's own plausibility gate remains the safety net |

## Order of work

**Phase 0 — make the hook relocatable.** No launcher yet; each item is testable with the
existing scripts.
1. OpenXR loader found relative to the hook DLL.
2. `AFXVR_AUTOSTART`: start the session without F9.
3. Config directory: exec from `cfg/cs2vr/`.
4. The control pipe, with `mirv_vr_pipe 0|1` to turn it off. Line-based, local only,
   commands executed on the engine thread through the queue that already exists.
5. Inventory what the hook actually opens at runtime from the HLAE tree (shaders under
   `resources\`, `AfxHook.dat`, the OpenEXR DLLs?) with Process Monitor once, and write the
   list down. That list is the zip.

**Phase 1 — `cs2vr.exe`, command line first.** `cs2vr watch <demo>` does everything in the
table. C++ and CMake, in `tools/launcher/`, because the toolchain, the CI job and the
OpenXR probing code are already here and the result is one small exe with no runtime to
install. Pure logic (VDF parsing, runtime selection, argument assembly, `steam.inf`
comparison) goes in a header with tests, the way `MirvVrMath.h` does. `start-vr.ps1` becomes
a two-line wrapper and then goes.

**Phase 2 — releases from CI.** A `release.yml` on tags `v*`:
1. the existing jobs (tests, patches apply, hook build) plus the launcher build;
2. assemble `cs2-vr-spectator-<version>-cs2-<build>.zip`: `cs2vr.exe`,
   `hook\x64\AfxHookSource2.dll` with `openxr_loader.dll` and the runtime DLLs beside it,
   `hook\resources\` — **that layout is forced, not chosen**: the hook derives its
   resource folder from its own module path, one directory up, and a DLL at
   `hook\AfxHookSource2.dll` loses its shaders without an error
   ([experiment 21](experiments/21-what-the-zip-has-to-contain.md)) — then `cfg\`, `LICENSE`, `THIRD-PARTY.md`
   (advancedfx MIT, OpenXR loader Apache-2.0, OpenEXR BSD), `README.md`;
3. SHA-256 alongside; `gh release create` with notes generated from the commit subjects,
   which in this repository already read as release notes;
4. the CS2 build number in the asset name and the release title, because it is the first
   thing anyone needs to know when it stops working after a game update.
   Unsigned, and the release notes say so: an unsigned exe that injects a DLL will be
   flagged by SmartScreen and by some antivirus. Signing is a cost decision, not a
   technical one; note it, do not pretend it away.

**Phase 3 — a window.** One dialog: demo, **Watch**, three status lights (Link up, SteamVR
off, CS2 build matches), and a second tab with the HUD layout and the handful of numbers
that are tuned worn (spread, distances, turn mode), written through the pipe so they apply
live. Win32 or Dear ImGui; decide when Phase 1 works. The command line stays.

## Entering VR from a normal game, instead of launching into it

Asked for the same afternoon: start CS2 at an ordinary size, use its own menus, open any
demo from the Watch tab, and only when the map is up press F9 — "and the resolution and the
rest fix themselves". And the launcher must not be tied to one recording.

Two things stand in the way, and only one of them is ours.

**Ours: the eye size is the window size.** Submission is a `CopyResource` of the back
buffer, so today the window is 2528x2780 from the first frame — which is why the menu is
unusable and the console's input line is off the bottom of a 1600-row display. For F9 to
"fix the resolution" the back buffer has to change size while the game runs:

1. *The engine's own path.* The video settings page changes resolution without a restart,
   so the function exists; `mat_setvideomode` does not (Source 1 only). One desk experiment:
   with `mirv_cvar_unhide_all`, look for what the settings page calls (`find vid`, `find
   video`, `find setting.`), and failing a command, find the function behind it. On F9:
   remember the current mode, switch to borderless at the size the runtime recommends, wait
   for the resize to reach our existing swap-chain hooks, create the OpenXR swapchains,
   start the session. On F5: the reverse. `EnsureSwapchains` must learn to be recreated
   when the back buffer changes size — it assumes one size per process.
2. *If that cannot be found:* keep launching at eye size, but let the launcher own demo
   selection (Phase 3), so CS2's menu is never needed. Cheap, already planned, and the
   honest fallback.
   **Or put CS2's own menu in the headset**, the operator's second suggestion, and the
   better of the two fallbacks because it needs no launcher UI at all and the pieces
   mostly exist: `mirv_vr_panel sheet` already hangs the whole window on one quad.
   What it takes:
   - A session that does not depend on a map. Today `xrWaitFrame`/`xrBeginFrame`/
     `xrEndFrame` are driven by the eye passes, and the quads are submitted "only with a
     world layer under them". Menu mode is the opposite: no extra passes at all (nothing
     worth rendering twice, and the view struct is not a camera there), the frame loop
     driven from the main pass's present, and a frame of nothing but the sheet — opaque,
     not cleared to transparent — on black. A cinema screen: about 70 degrees wide, two
     metres away, room-fixed.
   - Switching by itself: in a map with a demo playing → eyes and HUD quads; otherwise →
     the sheet. The pause menu inside a demo wants the sheet too, so a controller gesture
     (long press on Menu) to call it up, rather than guessing when Panorama has focus.
   - The pointer's second stage, which this makes necessary rather than nice: ray → (u,v)
     on the quad → window client pixels → synthetic mouse, trigger as the left button,
     stick as the wheel. Panorama accepts synthetic mouse input (experiment 11).
   - Two things to find out at a desk before building any of it, both at 1264x1390 since
     only the aspect matters: **what CS2's main menu looks like at 0.91:1** — Panorama is
     laid out for 4:3 and wider, and a menu that overlaps itself is not a menu — and
     **whether the 2780-row back buffer is stretched or cropped into the 1600-row
     window**. Stretched, every pixel of the sheet is reachable by a synthetic mouse;
     cropped, the bottom 42% is not, and the window has to be moved while clicking.
   - If the engine's resolution switch (1) is found as well, the two combine: the menu runs
     at an ordinary size and aspect on the sheet, and entering a map resizes for the eyes.
     That is the version worth shipping; this fallback is what works without it.
3. *Not recommended:* keep a normal window and scale it into the eyes with a shader blit.
   At 2560x1600 that is about 14 px/degree against the 28 we have now, and most of a 16:10
   frame lands outside the headset's frustum.

**Ours too: VR mode is a set of settings, and they must come back off.** `vr.cfg` changes
`fps_max`, `cl_demo_predict`, `hud_scaling`, `cl_drawhud`, the key layout. Entering on F9
means applying them then, and leaving on F5 means restoring what was there — read each
value before writing it. The video mode is the one that matters most: CS2 persists it to
`cs2_video.txt`, and a half-size desk launch already leaked into a worn session once. The
launcher keeps a copy of that file and puts it back on exit, whatever happened in between.

**Not ours: `-insecure`.** The hook is injected at start, so the whole launch is
`-insecure` whether or not F9 is ever pressed. Menus, demos and offline play work;
matchmaking and VAC-protected servers do not, and must not. "Enter the game normally"
means the game's interface, not an ordinary online session — the README has to say this
in its second paragraph, because it is the first thing someone will try.

F9 itself should refuse, with a reason the operator can perceive in a headset (a sound, or a
line on the HUD sheet), unless a map is loaded and a demo is playing. The launcher's
**Watch** button becomes two: *Open CS2* (menus, pick a demo there, F9 when ready) and
*Watch this demo* (straight in, as today).

## After the first release: playing, not only watching

Asked as "theoretically": could an ordinary game run in the headset, with the hook telling
a live game from a recording — look around with the head, shoot and walk with the
controllers? Offline, yes. **Online, no, and nothing here may try**: the hook is an
injected DLL, the launch is `-insecure`, and a head that looks around corners independently
of the aim would be an advantage on any server where it mattered. Bots, a local server,
practice maps.

- *Telling the two apart* is one call the hook already makes (`IsPlayingDemo`). Spectator
  mode keeps today's controls; player mode swaps the whole mapping.
- *The head is not the aim.* We only move the rendered camera; the engine still owns the
  player's view angles, and the weapon model is drawn along the camera. Either the head
  aims (write the head's yaw and pitch into the player's view angles — simplest, and what
  VR players expect) or the aim stays on a stick and needs its own marker in the world.
- *Walking* is the left stick sent as real W/A/S/D key events, the route the spectator
  controls already use — digital, so no analogue speed, and "forward" has to mean where
  the head faces. Physical steps move only the camera, never the player's body, so in
  player mode the room-scale offset is clamped to a lean (about 0.3 m): more than that and
  the eyes leave the hitbox they belong to.
- *Shooting, jumping, crouching, reloading* are key events on triggers and buttons.
- *The crosshair, ammunition and health* come back into the world: the HUD quads carry
  the last two; the crosshair has to sit along the true aim.
- *Frame rate is the real obstacle.* 34 frames a second is watchable seated; with
  artificial locomotion it is nausea. This waits for a steady 72.

## What this deliberately does not do

- Ship anything of Valve's, start without `-insecure`, or touch a VAC-protected session.
  The boundary in `install.md` moves into the launcher's code and the README's first screen.
- Auto-update, telemetry, an installer. A zip that can be deleted is the whole install.
- Hide the experiment scripts. `scripts/` stays for development; the release simply does
  not need it.

## The README

Rewritten from scratch **in the same commit that tags the first release**, not before: a
README describing a launcher that does not exist is worse than the one we have. Structure:

1. One paragraph and one picture: what this is.
2. The boundary: own demos only, `-insecure`, no redistribution of game files.
3. **Install and run** — the three steps at the top of this page, plus what each status
   light means and what to do when it is red.
4. Controls: the controller table as it is today (sticks, triggers = players, X/Y = seek,
   left stick click = camera type, right stick click = HUD, Menu = place HUD), keyboard
   equivalents.
5. What to expect: frame rate, which CS2 build this release matches, what a game update
   does and where to look for a new release.
6. Troubleshooting, from `cs2-vr-runtime-handling`: SteamVR holding the headset, a session
   that stays IDLE, the hook refusing to write after an update.
7. Building from source → `docs/install.md`, unchanged in substance.
8. How it works, in ten lines, linking to the experiments for anyone who wants the story.

The current README's content — what is known, what was measured — moves to
`docs/` where it already mostly lives.
