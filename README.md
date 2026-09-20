# CS2 VR Spectator

Watch Counter-Strike 2 demos from inside the map, in a VR headset — stand next to a
bombsite, look around, lean round a corner, fly above the round while it plays. CS2 itself
still loads the map, plays the demo and renders the world; this project gives it two eyes
and a head.

**Alpha.** It works and is comfortable enough to watch a match in, on the one setup it was
built and worn on: a Meta Quest 3 over Link, Windows 11, an RTX 4070 laptop. Expect rough
edges, and expect a CS2 update to break it until a new release is made.

> **Read this first.** This starts CS2 with `-insecure` and loads a DLL into it. That is
> fine for watching **your own demo files** and playing **offline with bots**, and for
> nothing else. It cannot join matchmaking or any VAC-protected server, and nothing here may
> be changed to try. It does not contain or redistribute anything of Valve's.

## Install and run

1. Download `cs2-vr-spectator-<version>-cs2-<build>.zip` from
   [Releases](https://github.com/vr-meta/cs2-vr-spectator/releases) and unpack it anywhere
   — but not inside OneDrive, Documents or the Steam library, and keep its folders as they
   are (the hook finds its files from its own location).
2. Put the headset on **Link**. **Close SteamVR** if it is running.
3. Run `cs2vr.exe`. CS2 starts and its own menu appears on a screen in the headset; point a
   controller at it and pull the trigger to click. Open a demo from the *Watch* tab, or skip
   the menu: `cs2vr.exe watch C:\path\to\match.dem`.

That is the whole install. To remove it, delete the folder, and delete
`game\csgo\cfg\cs2vr\` and `game\csgo\cs2vr_demos\` inside your CS2 installation — the only
places anything is written outside the folder you unpacked.

The exe is **not signed** and it loads a DLL into a game, so SmartScreen ("More info → Run
anyway") and some antivirus will object. Each release has a SHA-256 next to it.

`cs2vr.exe check` looks at the machine and starts nothing. It says what it found and, in a
sentence each, what is in the way:

| It says | It means |
| --- | --- |
| `build  !` …is build X but this release was made for build Y | CS2 has updated. The hook checks the game's memory layout and refuses to move the camera if it changed — the view then ignores your head. Look for a newer release. |
| `SteamVR   RUNNING` | On a Quest over Link, SteamVR holds the headset, and a second VR program beside it is never given a frame: the game freezes. Close it. |
| `Link      Meta's VR service is NOT running` | No headset for the game to find. Start Link first. |
| `hook      NOT FOUND` | The zip was unpacked partly, or the folders were rearranged. |

## Controls

Watching a demo (the hook prints the live table with **PgUp**, into `console.log`):

| Left controller | | Right controller | |
| --- | --- | --- | --- |
| stick | fly, in the direction you look | stick | snap turn; up/down to rise and descend |
| stick click | camera: first person → chase → free | stick click | show / hide the HUD (hold: recentre) |
| trigger | previous player | trigger | next player |
| grip | free look on / off | grip | back onto the player |
| X / Y | back / forward 10 s | A / B | pause / slow motion |
| Menu | CS2's own menu on a screen (hold: Escape) | | |

Your head is always free. Lean, crouch and step and the camera moves with you. The HUD —
score, radar, timeline — hangs around you as panels that follow your position and keep
their direction. Keyboard equivalents are in `cfg\vr_keys.cfg`; **F9 / F5** start and stop
the headset session, and the arrow keys switch players and camera.

Playing offline against bots works too, and is rougher: `cs2vr.exe play de_inferno`. The
left stick walks where you look, the right controller aims like a pistol — a crosshair
shows where the shot will really go, which trails your hand by a frame or two — the right
trigger fires, the right stick snap-turns. Treat it as an experiment; the thinking behind
it is in [`docs/07-release-plan.md`](docs/07-release-plan.md).

## What to expect

- **About 35 frames a second** on the reference machine, where the headset wants 72. Head
  turns are smooth regardless — the pose each image was drawn from is reported exactly, so
  the runtime's reprojection holds the world still — but the edges smear during fast turns
  and moving through the map is not as smooth as looking around it. Most of a frame is not
  the game rendering, so lowering the graphics settings does not help
  ([where the frame goes](docs/experiments/13-where-the-frame-goes.md)).
- **The game window is the size of one eye** (2528×2780 by default) and taller than most
  monitors. Windows clips it; that is expected. It also means there is no usable console on
  the monitor — use the menu in the headset, a key, or a config.
- **Player name tags are off.** CS2 lays them out once per frame for a flat screen; they
  cannot be right in two eyes ([why](docs/experiments/15-hud-per-eye.md)). The x-ray
  outlines are drawn in the world and work.
- **Only Meta's PC runtime has been worn.** SteamVR with other headsets should work — the
  frame is submitted in the runtime's own frustum either way — but nobody has tried.

- **Only ever start it with `cs2vr.exe`.** Do not load `hook\x64\AfxHookSource2.dll` by
  hand into a CS2 you started from Steam. The hook checks for `-insecure` itself, and
  without it puts up an error dialog and ends the game — and if it cannot end it, a dialog
  that never closes. With a headset on, a dialog on the monitor is invisible: the game just
  appears to hang. `cs2vr.exe` has no way to start the game without `-insecure`, so this
  only happens to people who go round it.

If something goes wrong, the hook's side of the story is in
`<CS2>\game\csgo\console.log`, on the lines starting `AFXVR:`. Include them in an issue,
with the output of `cs2vr.exe check`.

## How it works

CS2's own stereo hooks are dead ends. Stereo comes from [HLAE](https://www.advancedfx.org/)'s
multi-pass rendering, which re-renders one frame several times, plus a change of ours that
gives each pass its own camera: the engine resolves the camera once per frame, but the
object holding it is persistent and re-read by every pass, so rewriting it between passes
separates the eyes. The two passes go to OpenXR as one projection layer; the HUD and CS2's
menu go separately, as panels in space.

How each part was established — including the explanations that turned out wrong — is in
[`docs/`](docs/README.md), one experiment per question.

## Building it and changing it

[CONTRIBUTING.md](CONTRIBUTING.md): the toolchain, how the source is laid out (our files,
plus patches against HLAE), building the hook and the launcher, the tests, the two kinds of
launch, and the rules that were each paid for. Open work is in the
[issues](https://github.com/vr-meta/cs2-vr-spectator/issues); the plan is
[`docs/07-release-plan.md`](docs/07-release-plan.md).

The hook is built on [advancedfx / HLAE](https://github.com/advancedfx/advancedfx) (MIT).
Everything else that ships in a release is listed in [THIRD-PARTY.md](THIRD-PARTY.md). The
organisation's [Portal 2 VR](https://github.com/vr-meta/portal2vr) was the reference for
what a VR integration of a Source game has to get right.

## Licence

**[PolyForm Noncommercial 1.0.0](LICENSE)** — use it, change it, share it, for any
noncommercial purpose. Commercial use is not licensed. Keep the notice:

> Required Notice: Copyright 2026 butschster \<butschster@gmail.com\>

That makes this project **source-available, not open source** in the OSI sense, and it is
worth saying plainly before anyone spends an evening on it: contributions are welcome and
the code is here to be read and improved, but the noncommercial restriction is real and
GitHub will show the licence as non-standard.

The parts that are not ours keep their own terms regardless of that — you retain your MIT
rights to the advancedfx code inside `AfxHookSource2.dll`, and Apache-2.0 to the OpenXR
loader. [THIRD-PARTY.md](THIRD-PARTY.md) has the list.
