---
name: run-cs2-vr
description: Start a cs2-vr-spectator session in either of its two modes - watching one of the user's own CS2 demos, or playing offline against bots - and then act as the operator's hands while they are wearing the headset, adjusting the HUD, the aim and the field of view live through the hook's console pipe. Use when asked to launch, start, run, watch a demo in VR, play CS2 in VR, or to change something while a VR session is already running.
---

# Running a cs2-vr-spectator session

Two modes: **watch** a demo, or **play** offline against bots. Both are one command. Most
of the work in this skill is what happens either side of it — the checks before, and being
useful to somebody who has a headset on and cannot reach the keyboard.

## The rule that outranks the rest

**The person wearing the headset is the instrument, and they are not you.** So:

- **Never start the game without telling them first, in the same message.** From outside,
  a launch you meant as a check and a launch they were about to wear look identical, and
  they may be holding controllers, mid-round, or not in the room.
- **Never press keys or move the mouse for them** while a session is running. The hook is
  already sending synthetic input on their behalf; a second source of it produces behaviour
  nobody can reproduce afterwards.
- **Only their own demo files.** Do not download a demo, do not fetch one from a match
  service, and do not go near matchmaking or any VAC-protected server. This loads a DLL
  into a game started with `-insecure`; that is for their own recordings and offline bots.
- When they describe how something feels, that is data and it outranks your reading of the
  code. Several confident explanations in this project's history were wrong and a worn eye
  found them.

## 1. Which launcher

| They have | Use | Notes |
| --- | --- | --- |
| An unpacked release | `cs2vr.exe` | `menu`, `watch <demo>`, `play <map>`, `check`, `selftest` |
| The source tree | `scripts\start-vr.ps1` | `-Demo <file>`, `-NoDemo`, `-NoStart` |

Do not mix them, and do not hand-roll a launch with your own command line. Both of these
fix the per-eye window size, the frame cap, Meta's runtime and `-insecure` in one place
precisely because remembering them at a prompt went wrong: a size typed once at a desk got
written into CS2's own video settings, and the next worn session ran at a quarter of the
pixels before anybody noticed.

**Warn them that a launch can change the desktop resolution**, and that it is CS2 doing it,
not this project. With `setting.fullscreen 1` in CS2's own `cs2_video.txt` the game ignores
`-windowed` and takes the display: a per-eye size no monitor offers snaps to the driver's
nearest legal mode and everything on the desktop moves. It comes back when CS2 closes. If
they have windows arranged for work, that is worth knowing before rather than after.

## 2. Before starting

Run `cs2vr.exe check`. It starts nothing and reports every precondition as a line. Show
them its output rather than summarising it. Exit code 0 means go.

The three that stop a launch, and why they are not pedantry:

- **SteamVR must not be running.** On a Quest over Link it holds the headset, and a second
  VR application underneath it is never scheduled: `xrWaitFrame` takes hundreds of
  milliseconds, the frame rate collapses, and CS2 stops responding with its render thread
  parked inside the wait. It reads as a hang, not as a conflict.
- **CS2 must not be running.** The hook has to be in the game from its first instruction.
- **The CS2 build must match the release.** The hook writes into the game's memory at
  offsets measured on one build. On another it refuses to write, and what the person sees is
  a camera that ignores their head — which looks broken rather than out of date.

With a source tree there is no `check`; confirm the same three by hand and make sure the
headset is on Link (`OVRServer_x64` running) before saying it is ready.

## 3. Mode: watching a demo

```
cs2vr.exe watch "C:\path\to\their\match.dem"
```

Ask which demo; never choose one for them, and never go looking outside directories they
named. An absolute path is staged into `game\csgo\cs2vr_demos\` by the launcher, as a hard
link where it can be, because `playdemo` only reliably looks inside the game's own tree.

This is the finished half of the project. Expect it to work: stereo, head tracking,
leaning, sound that follows the head, the HUD on panels around them.

## 4. Mode: playing against bots

```
cs2vr.exe play de_inferno --bots 6
```

Offline casual, `sv_lan 1`, nothing on the internet. They can also just run `cs2vr.exe` and
drive CS2's own menu from inside the headset with the controller pointer — mode, map, start,
team — which is a real route and often the nicer one.

**Say these three things before they put the headset on**, because discovering them mid-round
is worse:

- **A real match is not possible and that is not a bug.** Between a synthetic mouse, a servo
  still learning what one mouse count is worth, and about half the frame rate the headset
  wants, they are a beat behind anyone on a flat screen. Bots are the point.
- **Aim is not steady yet.** The crosshair shows where the shot will actually go; the hand
  shows where they are pointing. When the two disagree, the crosshair is right. It can drift
  after a respawn, a spectator camera or a teleport.
- **Health and ammo may be in the wrong place or missing.** Those two HUD rectangles are
  estimates, not measurements — see step 6, where fixing them is a two-minute job if they are
  willing.

If the weapon model looks oversized, swims against the world, or follows their head instead
of their hands, that is the known viewmodel bug: send `r_drawviewmodel 0`. Most sessions so
far have been played with the gun off.

## 5. While it starts

The launcher follows `console.log` and prints the hook's `AFXVR:` lines itself. Two of them
mean it worked, and which one depends on how it was started:

- `AFXVR: submitting frames to the headset.` — a demo or a map, in stereo.
- `AFXVR: showing the menu in the headset. Point a controller at it.` — no demo; CS2's own
  menu is up. This is the normal ending for a bare `cs2vr.exe`.

Anything else: read `<CS2>\game\csgo\console.log` and quote the last `AFXVR:` lines
**verbatim**. They are written to be read by a person; your summary of them is worth less
than the lines.

**If they say it froze, check whether the game window is minimised before anything else.**
A minimised window gets no present events, CS2's swap chain spins on
`QueuePresentAndWait looped ... without a present event`, and the headset falls to about
two frames a second. It is indistinguishable from a hang from inside the headset, it has
happened during a real session, and restoring the window fixes it immediately.

**Say you are about to restore it, and why, before you do.** It is very often minimised
because *they* minimised it — to type to you, which is the one thing they can do without
taking the headset off — and a window that comes back by itself while they are describing a
problem is you reaching into their machine unannounced. Tell them, then restore:

```powershell
$h = (Get-Process cs2).MainWindowHandle
Add-Type 'using System;using System.Runtime.InteropServices;public class U{[DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int c);}'
[U]::ShowWindow($h, 9)   # SW_RESTORE
```

## 6. Being their hands: the console pipe

There is **no usable console in a worn session** — the window is taller than the display,
Windows clamps it, and Panorama puts the input line off the bottom of the screen. The hook
opens a named pipe instead, and this is the whole reason you are useful once the game is up.

**`cs2vr.exe` does not exit when the game starts — its window becomes the remote control**,
and if somebody is sitting at the keyboard that is the route to reach for first: `r` puts the
picture back in the headset, `s` stops the session and leaves the game running, `c` sends a
console command, `q` closes the window and leaves the game alone. Anything else typed at the
prompt goes through as a console command. It exists because both of those were needed, by
hand, on the day this was first worn, and nobody who downloaded a zip could have done either.

From a source tree: `scripts\send-command.ps1 "mirv_vr_panel spread 1.2"` (pass `-Log` with
their CS2 path; the default in that script is one developer's disk). From an unpacked
release there is no `scripts\`, so write the pipe directly:

```powershell
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'cs2vr', [System.IO.Pipes.PipeDirection]::Out)
$pipe.Connect(2000)
$w = New-Object System.IO.StreamWriter($pipe)
$w.WriteLine('mirv_vr_controls')
$w.Flush(); $w.Dispose()
```

One line, one command, queued and dispatched on the engine thread in order. **There is no
reply on the pipe** — the answer appears in `console.log`, so read it back from there.

**Connect, write, disconnect — every time.** The hook creates the pipe with
`nMaxInstances = 1`: one client at a time, and it re-creates it after each disconnect. A
handle held open between commands works perfectly for whoever is holding it and silently
takes the pipe away from everyone else — `send-command.ps1`, the operator, another agent —
with no message anywhere saying why. So do not keep the handle, and if a connect fails
once, retry briefly: there is a real window during the re-create in which nothing is wrong.

If `tools/server/` is built (`cargo run --manifest-path tools/server/Cargo.toml`), prefer
it to either route above: `POST /command` with `X-Cs2Vr: 1` writes the line and returns the
log lines that followed, so a command and its answer are one request instead of a write
plus a hunt through `console.log`. `GET /state` gives the mode, map, frame rate and
per-eye size without parsing anything. It attaches to a running session and never starts
one.

What is worth sending, and when:

| They say | Send |
| --- | --- |
| "I can't see my health / ammo" | `mirv_vr_panel rect health 0.00 0.88 0.30 1.00` — then walk the numbers with them; left, top, right, bottom as fractions of the frame |
| "the HUD is in the wrong place" | `mirv_vr_panel region ammo -30 -42 20 0.9` — azimuth, elevation, width in degrees, distance in metres |
| "the panels are too spread out" | `mirv_vr_panel spread 0.8` (or `1.2` for further into the corners) |
| "I can't aim" | `mirv_vr_aim hand`, then check `console.log` for a `gain` line that is not zero |
| "turning is horrible" | `mirv_vr_turn smooth` or `mirv_vr_turn snap 30` |
| "the field of view is wrong" | `mirv_vr_fov` to see it, and read `docs/experiments/18` before changing it — the vertical is not free |
| "where am I?" | `mirv_vr_recenter` |
| "what does this button do?" | `mirv_vr_controls` — prints the live layout into `console.log` |
| anything at all is wrong | `mirv_vr_version` and `mirv_vr_selftest` first, before theorising |

**Change one thing at a time and write down what it was.** Two dials moved together produce
a reading that describes neither. When a number turns out right, say so explicitly so it can
be written back into the code — a good value discovered in a headset and then lost is the
most expensive kind of mistake here.

## 7. Stopping

**Order matters:** stop the session (F5, or `mirv_vr_xr stop`), then close CS2, then the
runtime. Killing CS2 while a session is live can leave a process that cannot be killed and
still holds the hook DLL, and then the only way to build again is to rename the DLL aside.

If that has already happened, say so plainly rather than retrying the kill.

## 8. Afterwards

Report what was run, which mode, what the person said about it, and any number that was
changed and turned out better. The last of those is the point: this project is built out of
values that only a worn eye could settle, and the ones that never made it back into the code
had to be found twice.
