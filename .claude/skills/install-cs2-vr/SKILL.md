---
name: install-cs2-vr
description: Install cs2-vr-spectator from its GitHub releases onto this Windows machine and get to a first worn session - download the release zip, verify it, unpack it, check the prerequisites (CS2, Link, no SteamVR, matching CS2 build) and start it. Use when asked to install, set up, update or reinstall cs2-vr-spectator, or when someone has the repository but no working build.
---

# Installing cs2-vr-spectator from GitHub

The aim is the three steps in `docs/07-release-plan.md`: unpack a zip, put the headset on
Link, run `cs2vr.exe`. This skill does the unpacking and every check around it, and says
plainly when something it depends on does not exist yet.

**Never** start CS2 without `-insecure`, never copy Valve's files anywhere, and do not
proceed if the person wants to use this on anything but their own demo files.

## 1. Is there a release?

```
gh release list --repo vr-meta/cs2-vr-spectator --limit 5
```

- **A release exists** → continue with step 2.
- **No releases** (still true as of 2026-09-20) → say so plainly. **Do not improvise a
  download**; there is nowhere else to get this from, and anything that looks like it is not
  this project. Building from source is the only install, it needs a C++ toolchain and about
  an hour, and it is described in `CONTRIBUTING.md`: the toolchain, the advancedfx clone,
  `src/AfxHookSource2` copied in, `docs/patches/*.patch` applied **without touching line
  endings**, then `cmake -S tools/launcher` for `cs2vr.exe`. A source tree can also be
  started with `scripts\start-vr.ps1`, which is what the developers use. Offer that, and
  stop here if they do not want it.

## 2. Download and verify

Pick the newest release whose asset name carries the CS2 build the machine has. The build
is `ClientVersion` in `<CS2>\game\csgo\steam.inf`; the asset is
`cs2-vr-spectator-<version>-cs2-<build>.zip`. Find CS2 through Steam, not by guessing:
`HKCU\Software\Valve\Steam\SteamPath` → `steamapps\libraryfolders.vdf` → the library that
holds `appmanifest_730.acf`.

```
gh release download <tag> --repo vr-meta/cs2-vr-spectator --pattern "*.zip" --pattern "*.sha256" --dir <scratch>
```

Compare `Get-FileHash -Algorithm SHA256` with the published `.sha256`. A mismatch ends the
install. If no asset matches the installed CS2 build, say which builds are available and
that a game update is the usual reason; do not install a mismatched one silently — the
hook will refuse to write the camera and it will look broken.

## 3. Unpack

Ask where; default `%LOCALAPPDATA%\cs2-vr-spectator\<version>\`. Not inside the Steam
library, not inside OneDrive-synced or Controlled-Folder-Access folders (Documents,
Videos, Pictures): unsigned tools are blocked from writing there on this kind of machine.
`Expand-Archive`, then `Unblock-File` on the unpacked tree — the zip came from the
internet, and SmartScreen will otherwise stop an unsigned exe that injects a DLL. Tell the
person that is why, and that antivirus may still object.

Then confirm the layout survived, because it is load-bearing and fails silently:

```
hook\x64\AfxHookSource2.dll      (+ openxr_loader.dll and the OpenEXR / runtime DLLs)
hook\resources\shaders\...
```

The hook finds its resources from **its own path** — strip the file name, go up one
directory — and it looks itself up by the exact name `AfxHookSource2.dll`. Move the DLL up
a level or flatten the folders, and every resource path resolves one directory too high.
Rename it and it is worse: the lookup returns nothing, the folder is left **empty**, and
`resources\...` is then resolved against the process's working directory — CS2's install,
not ours. Neither case reports an error. So:

- never "tidy" this tree;
- no version stamp in the DLL's file name — the version lives in the folder name;
- a second build for comparison is a second complete tree (`<version>\hook\x64\...`), never
  a second file beside the first.

See `docs/experiments/21-what-the-zip-has-to-contain.md`.

## 4. Check what a session needs, and report each as a line

**`cs2vr.exe check` from the unpacked folder does all of this and starts nothing.** Run it
first and show the person its output; it is the same list, from the program that will act on
it, so it cannot drift out of date the way this table can. Exit code 0 means go. Use the
table below to explain whatever it reports, and to check by hand only if the launcher is
missing or refuses to run at all.

`cs2vr.exe selftest` is worth running once on a machine you have not seen before: it proves,
against Windows' own `cmd.exe` and without touching CS2, that a DLL and its neighbouring
dependencies can actually be loaded into a freshly created process here. If that fails,
nothing else will work and the reason will be antivirus or policy, not this project.

| Check | How | If it fails |
| --- | --- | --- |
| Link is up | `OVRServer_x64` running and a `Reality Labs Composite XRSP Interface` device present | Start Link in the headset first |
| SteamVR is not running | no `vrserver`, `vrmonitor`, `vrcompositor` | Close it. On a Quest it holds the headset and our session is never scheduled |
| CS2 is not running | no `cs2` | Close it; the hook cannot be replaced under a running game |
| CS2 build matches the release | `steam.inf` vs. the asset name | See step 2 |
| A demo exists | any `.dem` the person names | Ask for one; do not download demos |

## 5. First run

Run `cs2vr.exe` from the unpacked folder — with no arguments it stops at CS2's own menu,
shown in the headset with a controller pointer, which is the friendliest first thing to see.
`cs2vr.exe watch <demo>` goes straight into a demo, `cs2vr.exe play <map>` into a game
against bots. The launcher follows `console.log` itself and prints the `AFXVR:` lines as
they appear, so usually there is nothing to read separately.

**Do not press keys for the person, and never leave a game running they have not been told
about.** They are the one who has to put the headset on.

Two lines mean it worked, and which one you get depends on how it was started:

- `AFXVR: submitting frames to the headset.` — a demo or a map is being rendered in stereo.
- `AFXVR: showing the menu in the headset. Point a controller at it.` — no demo; CS2's own
  menu is on a screen in front of them. This is the normal ending for a bare `cs2vr.exe`.

Anything else, read `<CS2>\game\csgo\console.log` and report the last few `AFXVR:` lines
verbatim rather than summarising them. A session that stays `IDLE` means the headset is not
on a face, or another VR application is in front of it.

## 6. Say what was done

Where it was unpacked, which release and CS2 build, which checks passed, and how to remove
it: delete the folder, and delete `<CS2>\game\csgo\cfg\cs2vr\` — that directory is the only
thing the launcher writes outside its own folder.

## Updating

Same steps into a new `<version>` folder; keep the old one until the new one has been worn
once. After a CS2 update there may be no matching release yet — say that, and point at the
releases page rather than forcing an old build.
