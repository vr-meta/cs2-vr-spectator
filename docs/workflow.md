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
scripts\sweep-offset.ps1                                   # F5..F8 sweep + captures
```

Keys are sent with `SendInput` using the **scancode flag**; plain `SendKeys` does not
reach the game. Stream recordings land as TGA under
`game/bin/win64/<record name>/take####/<stream>/` — note that is next to `cs2.exe`, not
in `csgo/`.

TGA here is uncompressed 24-bit: header is 18 bytes, then BGR rows bottom-up. Comparing
raw bytes is enough; no decoder needed.

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
