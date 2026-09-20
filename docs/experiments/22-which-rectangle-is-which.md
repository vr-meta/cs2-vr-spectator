# 22 — Which rectangle is which, and why the menu panel did not line up

## The question

Worn, the menu panel showed a frame the operator could not reconcile with where their
controller ray went. Their words: *"разметка все равно осталась на экран"* — the layout still
looks like it is for the monitor. Aiming at what looked like empty space near the bottom of
the panel activated the game's Start button.

Two explanations were proposed before anything was measured, and both were wrong:

- that the panel copies the whole back buffer onto a quad whose aspect is computed from
  something else. It is not. `g_MenuAspect` (`MirvVrXr.cpp:1427`) and
  `g_SwapchainWidth/Height` (`:1526`) come from the *same* `pTexture->GetDesc` probe inside
  `EnsureSwapchains`, and `BuildSheetQuad` (`:4423`) spans `imageRect` across exactly that
  extent. They cannot disagree with each other.
- that the copy is a sub-rect left stale by a size change. It is not. `imageRect` is always
  the full extent, and `EnsureSwapchains` (`:1440`) destroys and rebuilds the swapchains on
  any size change — the log shows it firing correctly twice in one session.

Both were killed by reading, which is the only reason the real answer got looked for.

## What the log already knew

From `console.log` of the 11:31 session on 2026-09-20:

```
11:31:46  AFXVR: menu frame source 2560x1600 format 28      <- at the menu
11:32:26  back buffer is now 2528x2780, was 2560x1600       <- a map loads
11:32:26  swapchains 2528x2780, back buffer format 27 -> swapchain 29
```

Format 27 is `R8G8B8A8_TYPELESS`, the engine's captured render target. Format 28 is
`R8G8B8A8_UNORM` — *not* typeless, so not a capture. That is `g_pSwapChain->GetBuffer(0)`,
the fallback patch 002 takes because the menu view is called `CSGOMainMenu` and nothing is
captured. The panel's source at the menu is a different texture from the one it gets in
game, of a different size and a different format.

Meanwhile the engine's own render pipeline and display mode are 2528x2780 throughout the
same session (`RenderPipelineCsgo RT 2528x2780`, `m_DisplayMode m_nWidth: 2528 -> 2528`),
including before the panel ever appears.

So at the menu three rectangles are in play and nothing in the code ties them together: what
the panel **shows** (the presented surface), what the pointer maps u,v **onto**
(`GetClientRect`, `MoveMouseToSheet:1861`), and what Panorama **laid out for**. In game all
three collapse to 2528x2780, which is why only the menu was ever wrong, and why this survived
to a worn session.

## The three runs

Desk launches through `launch-cs2-experiment.ps1`, small windows, nobody wearing anything, no
XR session. The rectangles read from outside the process with `scripts/probe-cs2-window.ps1`;
the engine's own sizes from `console.log`.

| run | cfg `fullscreen` | asked for | client / window | desktop mode | engine RT / `m_DisplayMode` |
|-----|------------------|-----------|-----------------|--------------|-----------------------------|
| A | 1 | 1280x720 | 1280x720 | **changed to 1280x720** | 1280x720 |
| B | 1 | 1000x1400 | **2048x1536** | **changed to 2048x1536** | **1000x1400** |
| C | 0 | 1000x1400 | 1000x1400 | 2560x1600, untouched | 1000x1400 |

**Run B is the reproduction, and it explains a number this project had seen and not
understood.** `setting.fullscreen 1` makes CS2 ignore `-windowed` outright and force a
display-mode change. 1000x1400 is not a mode any monitor offers, so the driver snaps to the
nearest one it has — 2048x1536, a 4:3 mode — and the desktop goes with it. The engine keeps
rendering and laying out at the size it was asked for, 1000x1400. Two surfaces of different
aspect in the same instant, measured rather than inferred.

2048x1536 had been seen once before and written off as noise. It is not noise: it is what an
impossible request resolves to on this machine.

Worn, the same mechanism with the real numbers: asked for 2528x2780 (impossible), got
2560x1600 as the nearest legal mode, and that became the presented surface, the window, the
client rect *and* the desktop — while the engine went on rendering 2528x2780.

**Run A** is worth keeping because it shows the mechanism firing even when nothing looks
wrong: 1280x720 *is* a legal mode, so everything agrees, and the only visible effect is that
the desktop resolution changed under the operator.

**Run C is the control.** Windowed, and client == engine == 1000x1400 with the desktop
untouched.

## What only the operator could measure

The measurements above prove two surfaces disagree. They do not say *how* the frame is placed
in the larger one, and that distinction decides everything: a pure stretch preserves
fractional positions and would therefore misalign nothing at all — it would only look
squashed. This was left as an open gap rather than filled with a guess. The operator closed
it, unprompted:

> «я видел обрезанный экран игры, но все равно элементы управления реагировали как будто он
> во весь экран растянут»

> «при наведении на чёрные области я наводил на пункты меню, которые должны быть в тех
> местах»

> «в экранном режиме windowed управление корректное»

So it is a **crop, not a stretch**. The 2528x2780 frame occupies a sub-rect of the 2560x1600
buffer and the rest is black — never written. The panel copies the whole buffer, black
included, and `BuildSheetQuad` spans u,v across all of it. The click path maps the client rect
onto the engine's full layout. Hovering black therefore activates the item that *would* be
there if the frame filled the rectangle.

**Letterbox on the image, full-stretch on the hit-test.** That is the whole fault in one
sentence, and the third quote is the control from the other side: windowed, it is correct.

## What was in the process, and what was not

Runs A, B and C carried **upstream HLAE's `AfxHookSource2.dll`**, not this project's.
`launch-cs2-experiment.ps1` selects ours only with `-SelfBuilt`, which was not passed:

```
hlae\x64\AfxHookSource2.dll             Sep 12   AFXVR strings: 0     mirv_vr_panel: 0
hlae-selfbuilt\x64\AfxHookSource2.dll   Sep 20   AFXVR strings: 128   mirv_vr_panel: 40
```

The absence of `AFXVR` lines was first read as an injection failure and reported as one. It
was not. Two traps, both worth knowing:

- upstream's hook contains none of this project's code, so a **working** injection produces
  zero `AFXVR` lines and looks identical to no injection at all. The banner
  `| AfxHookSource2 (Sep 12 2026 18:07:50) |` is the thing to grep for, not `AFXVR`.
- the cfg's exec lands about **six seconds** after the command line
  (`[InputService] execing cs2vr/exp22_window`). Grepping for its marker before that shows an
  absence which is only earliness.

This makes the numbers *better* provenanced, not worse: none of this project's code was in the
process, so the display-mode behaviour above is pure CS2 with nothing of ours able to have
influenced the window. It is recorded here because a measurement is worth nothing if the next
reader cannot tell what was loaded.

## Also measured, and load-bearing elsewhere

**CS2 rewrites `cs2_video.txt` on exit.** After run A it had written `setting.defaultres 1280`
/ `defaultresheight 720` into the file itself, from that launch's `-w`/`-h`. Any design where
the launcher owns `setting.fullscreen` therefore has a third party writing the same file
between "write ours" and "put theirs back". Restore-on-exit cannot be the load-bearing
mechanism; the reliable shape is: on every launch, if a backup exists then the previous run
did not restore, so put it back *before* reading anything, then take a fresh backup. That
makes a crash, a kill and a normal exit the same case.

## Still not measured

- **Where the frame lands in the oversized buffer, and at what scale.** Narrowing `imageRect`
  to the sub-rect is meaningless without it, and it is not to be inferred.
- **How CS2 maps a client coordinate into its layout** when the two differ. If that is not
  the inverse of the placement above, then pointing the hit-test at a narrower image only
  moves the disagreement instead of removing it. The test of any fix is not "the image looks
  right" but "hovering a thing highlights that thing".

Both are answerable at a desk, with `-SelfBuilt` so our hook's size lines are present, and
`grab-cs2-window.ps1` for a picture of the letterboxing.

## Conclusions

- `setting.fullscreen 1` in `<Steam>\userdata\<id>\730\local\cfg\cs2_video.txt` beats
  `-windowed` on the command line, changes the desktop mode, and snaps an impossible
  resolution to the nearest legal one. No work inside the hook reaches that.
- With it off, the presented surface, the client rect and the engine's layout are the same
  rectangle, and both the picture and the pointer are correct. Confirmed by run C and by the
  operator worn.
- The hook should still not trust the whole back buffer. While `imageRect` and `g_MenuAspect`
  follow the swapchain extent rather than the rendered frame, any future surface larger than
  the frame shows padding as content and takes the quad's shape from the padding.
  Configuration is not a substitute for agreeing by construction.
