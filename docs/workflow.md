# How experiments are actually run here

Practical procedure, worked out over the first session. Written down because rediscovering
it costs an hour.

## Division of labour

The game console cannot be driven from outside — **CS2 has no `-netconport`** (the string
is absent from `engine2.dll`). So:

- **The operator** types commands into the game console and presses keys in-game.
- **Claude** prepares `.cfg` files, launches the game, captures and compares images, and
  reads results from `game/csgo/console.log` (enabled by `-condebug`).

Keep console interaction to one `exec` per step: everything else goes in the cfg.

**Switch the keyboard to English before typing in the console.** A Russian layout turns
`~` into `ё` and the command silently fails as `Unknown command: ёmirv_cvar_unhide_all`.

## Launch

```powershell
# Release HLAE
D:\Dev\cs2-vr-spectator\scripts\launch-cs2-experiment.ps1

# Self-built hook (same, but hook from hlae-selfbuilt)
```

Launching goes through HLAE with `-customLoader -noGui -autoStart`. HLAE exits
immediately after injecting — that is normal, not a failure. **CS2 takes 40–60 s to
appear**; checking too early looks like a failed launch.

Verify the hook really attached, and from which build:

```powershell
(Get-Process cs2).Modules | Where-Object { $_.ModuleName -match 'AfxHookSource2' } |
  Select-Object FileName
```

First command in the console, always:

```
mirv_cvar_unhide_all
```

It should report ~1929 convars unhidden. If it says `Unknown command`, the hook is not
attached and every later result is meaningless.

## Windowed, not fullscreen

Use `-windowed`. In fullscreen, bringing the window forward for a screenshot loses focus
and the capture grabs whatever else is on screen — one capture caught the desktop terminal
and showed up as a spurious "100% of pixels changed".

**Close the in-game console before capturing.** An open console covers the scene and
swallows the F-keys, which reads as "the binds do nothing".

## Capture and comparison

```powershell
scripts\grab-cs2-window.ps1 -Name <name> [-OutDir <dir>]   # PNG of the client area
scripts\send-key.ps1 -Key F7                               # one key press
scripts\sweep-offset.ps1                                   # F5..F8 sweep + captures
scripts\compare-tga.ps1 -Take exp04_probe\take0001         # eyeL vs eyeR, per frame
scripts\tga-to-png.ps1 -In <x.tga> -Out <x.png>            # to actually look at one
```

Keys are sent with `SendInput` using the **scancode flag**; plain `SendKeys` does not
reach the game. Binding the experiment's actions to F-keys in the cfg means the whole
run can be driven from outside, with only `exec` left to the operator.

Stream recordings land as TGA under `game/bin/win64/<record name>/take####/<stream>/` —
note that is next to `cs2.exe`, not in `csgo/`.

TGA here is uncompressed 24-bit: 18-byte header, then BGR. Comparing raw bytes is enough
for "are these the same image"; no decoder needed. To *look* at one, mind the row order:
HLAE sets bit 5 of the descriptor at offset 17, so rows are **top-down**, not the
bottom-up that TGA defaults to. Getting that backwards yields an upside-down image that
is easy to mistake for a broken render.

**Stop recordings.** `mirv_streams record start` writes about 300 MB per second at
1280x720 with two streams. One forgotten `record end` produced 28.6 GB in 90 seconds.

## What cannot be automated: console text

Key presses can be synthesised (`send-key.ps1`, scancodes), and the console **toggle**
works that way — but typed **text** does not reach the console. CS2's UI is Panorama, and
synthetic Unicode input lands nowhere: no command runs, and nothing appears in
`console.log`, not even `Unknown command`. A driver for this was written and deleted.

So the division of labour stands: the operator types commands, and everything an
experiment needs afterwards goes on an F-key bind, which *can* be driven from outside.

**The mouse is a different story.** It was reasonable to assume Panorama ignored synthetic
mouse input the way it ignores synthetic text. It does not: a synthesised click on the
demo timeline seeks, and a synthesised drag scrubs it
(`scripts/drag-in-cs2.ps1`, experiment [11](experiments/11-seeking.md)). So the parts of
the UI that have no console command behind them *can* be driven after all — which is worth
remembering the next time something seems unreachable.

Two traps when driving keys:

- The console toggle is a toggle. Pressing it when the console is already open closes it,
  and anything sent afterwards goes to the game as binds. Check the state first with a
  screenshot.
- Typing into the game by accident triggers binds. One stray attempt jumped the demo from
  0:51 to 22:06.

## With a VR runtime running

**SteamVR stalls CS2's presentation.** With SteamVR up, `console.log` fills with

```
CSwapChainBase::QueuePresentAndWait() looped for 23 iterations without a present event.
```

and the game stops showing frames. Because the main loop is stuck there, it also stops
processing console commands — which reads as "nothing works", including the console. It
is not the console. Killing SteamVR restores the game immediately and completely.

Other rules learned the hard way:

- **Never kill the runtime while an OpenXR instance is live.** CS2 then hangs unkillable —
  one thread, no window, holding the hook DLL and `console.log` — until a reboot. Run
  `mirv_vr_xr stop`, or close the game first.
- **Cap the game's framerate.** An uncapped CS2 and a VR compositor fight over the GPU and
  both lose. `launch-cs2-experiment.ps1 -VrReady` sets an eye-sized window and
  `fps_max 90`.
- **Start SteamVR only with the Quest already in Link.** Without a headset it takes
  foreground focus, makes the desktop unusable, and gives nothing: `xrCreateInstance`
  succeeds but `xrGetSystem` returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE`.
- CS2 dies when Steam does. If the game "just exited", check Steam is still running.

## Demos

Use `pro_mirage.dem` (GOTV, MOUZ vs NAVI). A locally recorded bot demo **did not replay
at all** — console full of `Cannot process snapshot tick N`, scene frozen.

- `exec exp00_play_pro`, then **F2** to slow down, **F4** to pause.
- **Never drag the timeline slider** — it crashed the game (access violation).
- **Never `playdemo` over a playing demo** — also crashes. Restart the game instead.
- Once `spec_mode 6` is set, the view cannot return to the default; restart.

## Rules that earned their place

Four measurements were thrown away in the first session, each because something other than
the thing under test differed between captures.

1. **Verify the subject is alive before measuring its response.** A frozen demo and a dead
   convar produce identical screenshots.
2. **Choose an indicator whose absence is unmissable.** The whole map disappearing cannot
   be misread; a weapon model that is not on screen can.
3. **Read back what a command actually did.** An invalid value prints help and silently
   leaves the setting unchanged (`worldAction hide` — valid values are `draw|noDraw|zOnly`).
4. **Always capture a return to baseline and require it pixel-identical.** A test that
   cannot reproduce its own starting point is not measuring what it thinks it is.
5. **Look for the absence of expected warnings.** `AFXWARNING: ... is not a command or
   cvar` distinguishes "the command did nothing" from "the command never ran".

And when searching logs: HLAE prints command lists quoted per argument
(`"mirv_input" "position"`), so a grep for `mirv_input position` finds nothing and looks
like proof of absence. It was not.
