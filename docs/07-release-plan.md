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
2. assemble `cs2-vr-spectator-<version>-cs2-<build>.zip`: `cs2vr.exe`, `hook\` (our DLL,
   `openxr_loader.dll`, the HLAE runtime subset), `cfg\`, `LICENSE`, `THIRD-PARTY.md`
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
