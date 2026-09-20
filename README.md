# CS2 VR Spectator

Counter-Strike 2 in a VR headset. **Watch** a demo from inside the map — stand next to a
bombsite, look around, lean round a corner, fly above the round while it plays. Or
**play**, offline against bots, on your feet: walk where your body faces, aim with the
controller, look wherever you like while you do it. CS2 itself still loads the map, runs
the game and renders the world; this project gives it two eyes, a head and a pair of hands.

## Why this exists

I like watching played matches. At some point I wanted to watch a recording in VR, just for
fun, and wondered whether it was even possible. I had already run
[Portal 2 in VR](https://github.com/vr-meta/portal2vr), and there are examples of how
[Left 4 Dead 2](https://github.com/sd805/l4d2vr) was done — so I tried it for CS2, and it
turned out rather well. This repository is the result.

Then I tried adding a play mode as well. It has its own downsides, and one of them is not
fixable: **you will not play a real match in this.** There is not enough reaction speed —
between a synthetic mouse, a servo learning what it is worth, and half the frame rate the
headset wants, you are a beat behind anyone playing flat. Running around and shooting at
bots, though, works fine, and is good fun.

It is a hobby project, and that is the honest frame for everything below: one person, one
headset, one machine, and a list of things found out the hard way.

## Where it is now

**Alpha**, on the one setup it was built and worn on: a Meta Quest 3 over Link, Windows 11,
an RTX 4070 laptop. Expect a CS2 update to break it until a new release is made.

The two halves are not equally finished, and the difference is worth knowing before you
start:

- **Watching works and is comfortable.** Stereo, head tracking, leaning, sound that follows
  your head, the HUD on panels around you. This is what the project was built for and what
  has had the most hours in a headset.
- **Playing is for bots, and has bugs.** You can walk, shoot, reload, crouch, jump, defuse
  and finish a round. You cannot be competitive at it — see above — and today **shooting is
  not yet steady and the in-game HUD is not yet right**; see
  [Playing](#playing-against-bots) for exactly what is wrong.

> **Read this first.** This starts CS2 with `-insecure` and loads a DLL into it. That is
> for watching **your own demo files** and playing **offline against bots**, and for nothing
> else. `-insecure` is what keeps you off VAC-secured servers; `cs2vr.exe` always passes it
> and has no way not to, and nothing here may be changed to get round that.
>
> **I have never tried this on a real match and I have not tested what VAC makes of it.**
> Do not find out. Do not load the hook into a CS2 you started normally, and do not play
> matchmaking with any of this on your machine. If you go around the launcher and get
> yourself banned, that is yours, not mine.
>
> It does not contain or redistribute anything of Valve's.

## Install and run

### Let an AI do it

It is 2026: you do not have to read an install guide. This repository ships a **skill** for
coding agents — Claude Code, Codex, anything that can fetch a file and run PowerShell — that
does the whole thing: finds the right release for the CS2 build you actually have, checks
the SHA-256, unpacks it somewhere sensible, unblocks it, verifies the folder layout the hook
depends on, checks that Link is up and SteamVR is not, and starts it. It also knows when to
stop and say so, which is the part that matters.

If you have cloned this repository, your agent already has it — just ask:

```
/install-cs2-vr
```

If you have not, hand it the skill by link. Paste this and nothing else:

```
Read https://raw.githubusercontent.com/vr-meta/cs2-vr-spectator/main/.claude/skills/install-cs2-vr/SKILL.md
and follow it to install cs2-vr-spectator on this machine. I have a Meta Quest 3 on Link and
my own CS2 demo files. Tell me what you are about to run before you run it, and stop if a
check fails instead of working around it.
```

The skill is [`.claude/skills/install-cs2-vr/SKILL.md`](.claude/skills/install-cs2-vr/SKILL.md)
— it is short, and worth reading yourself if you would rather know what is happening than
watch it happen.

### Or do it yourself

1. Download `cs2-vr-spectator-<version>-cs2-<build>.zip` from
   [Releases](https://github.com/vr-meta/cs2-vr-spectator/releases) and unpack it anywhere
   — but not inside OneDrive, Documents or the Steam library, and keep its folders as they
   are (the hook finds its files from its own location).
2. Put the headset on **Link**. **Close SteamVR** if it is running.
3. Run `cs2vr.exe`. CS2 starts and its own menu appears on a screen in the headset; point a
   controller at it and pull the trigger to click. Or skip the menu and say what you want:
   `cs2vr.exe watch C:\path\to\match.dem`, or `cs2vr.exe play de_inferno`.

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

Two layouts. The hook picks between them from what the game is doing — a demo, a live map,
a menu — and prints the live one with **PgUp**, into `console.log`. The **Menu** button
brings up CS2's own menu on a screen either way.

### Watching a demo

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

### Playing against bots

Two ways in, and both work.

`cs2vr.exe play de_inferno` sets a game up for you — offline casual, `sv_lan 1`, six bots,
nothing on the internet. Or just run `cs2vr.exe` and **use CS2's own menu from inside the
headset**: it hangs on a screen in front of you, a ray comes out of your controller with a
dot on the end, and the trigger clicks. Pick a mode, pick a map, start it, choose your team
when the picker comes up — all without taking the headset off. That menu is a real part of
this project rather than a fallback, and it is the same screen the **Menu** button brings up
over a running game, so settings and the buy menu are reachable too.

Bots are not a limitation of either route. They are the point: the input path is too slow
for anyone who shoots back properly.

The controls change with the mode, because the same twelve buttons cannot mean the same
things in both:

| Left controller | | Right controller | |
| --- | --- | --- | --- |
| stick | walk, relative to your body | stick | turn your body |
| stick click | slow walk on / off | stick click | reload |
| trigger | use: defuse, plant, open, pick up a gun | trigger | fire |
| grip | crouch (held) | grip | jump (held) |
| X / Y | pick a team, while the picker is up | A / B | next weapon / alternative fire |

**Your head and your aim are separate.** The right controller points where you shoot, your
head looks wherever you want, and your body turns with the right stick or follows where you
look. Aiming is limited to a window of about 22° around your gaze — the game has one mouse
and one crosshair, and letting the hand drag the view a long way off where you are looking
is what made earlier versions unpleasant.

**What is wrong with it today.** These are the two things to fix next, and they are known,
not suspected:

- **Shooting is not steady.** The game is driven by a synthetic mouse, and the hook has to
  *learn* what one mouse count is worth by watching the game's own aim respond — sensitivity
  is a convar nobody can read from in here. Until that estimate settles, the crosshair
  trails your hand, and it can drift or overshoot after a respawn, a spectator camera or a
  teleport. The crosshair shows where the shot will really go, not where your hand points;
  when the two disagree, believe the crosshair.
- **The in-game HUD is not placed right.** Health and ammo are cut out of the game's own
  frame and hung beside you, and **those two rectangles are estimates rather than
  measurements** — the rest of the HUD was measured off a screen capture, and these two live
  in a part of the frame the display clips away, where no capture can reach them. So they
  may show the wrong crop, or nothing. `mirv_vr_panel rect ammo <l> <t> <r> <b>` moves them
  while you wear it.
- **The weapon model is drawn with its own field of view**, which the engine keeps in a
  separate field from the world's. It comes out oversized, at the wrong depth, and it
  follows your head rather than your hands. `r_drawviewmodel 0` in the console turns the gun
  off, which is how it has mostly been played so far.

Everything above is reachable without a console through the pipe — `scripts/send-command.ps1`
writes to it, and `cs2vr.exe` leaves it open. The thinking behind the layout is in
[`docs/07-release-plan.md`](docs/07-release-plan.md).

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

**If you feel like improving it, you are welcome.** It is one person's hobby project and
there is plenty left: steadying the aim, measuring the two HUD rectangles that are still
guesses, the weapon model's own field of view, and whatever a headset that is not a Quest 3
turns out to do. Issues and pull requests are both fine, and so is just telling me what it
did wrong — a paste of the `AFXVR:` lines from `console.log` is worth more than a careful
description.

The hook is built on [advancedfx / HLAE](https://github.com/advancedfx/advancedfx) (MIT).
Everything else that ships in a release is listed in [THIRD-PARTY.md](THIRD-PARTY.md).
[Portal 2 VR](https://github.com/vr-meta/portal2vr) was the reference for what a VR
integration of a Source game has to get right.

## Licence

**[Apache License 2.0](LICENSE).** Use it, change it, ship it, sell it. What you owe in
return is attribution: keep the [`NOTICE`](NOTICE) file with anything you distribute, so

> Copyright 2026 butschster \<butschster@gmail.com\>

travels with the code. Say which files you changed, and keep the licence and copyright
headers you found. That is the whole of it.

The parts that are not ours keep their own terms regardless — you retain your MIT rights to
the advancedfx code inside `AfxHookSource2.dll`, BSD-3-Clause to OpenEXR, and Apache-2.0 to
the OpenXR loader. [THIRD-PARTY.md](THIRD-PARTY.md) has the list.

A licence is not permission to break Valve's rules. It grants you rights over **this** code
and none whatsoever over Counter-Strike 2 or Valve's services: own demos and offline play,
never matchmaking, never a VAC-protected server.
