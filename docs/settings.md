# Picture settings, and how to choose them for a machine

Everything here was measured on one machine on 2026-09-20. **One machine is not a rule**, so
this is written as the measurement, the mechanism, and the method to repeat it — not as a
table of recommended values for hardware nobody here has run.

## What actually decides picture quality

Three numbers, and the first two surprise people:

1. **The per-eye image is the game window.** Submission copies the back buffer, so `-w`/`-h`
   in `start-vr.ps1` *is* the eye resolution. Not a render-scale slider — the window.
2. **Only part of it reaches the eye.** The image is rendered symmetric and submitted with the
   runtime's own asymmetric frustum, cropped to match (`mirv_vr_crop`, and experiment 23 for
   why). Ask a live session:

   ```
   mirv_vr_crop
     Current: on. Last rectangles: eye 0 2035x2199 at (0,503), eye 1 2035x2199 at (493,503).
   ```

   Against a 2528x2780 buffer, that is 4.5M of 7.0M pixels used: **36% of every frame is
   rendered and thrown away.** Inherent to the approach, but you pay for it.
3. **What the runtime wants**, logged at session start:

   ```
   AFXVR: view 0 recommended 2064x2272
   ```

   Compare it with the crop above. Here the crop is **2035x2199 against 2064x2272 — 0.97×.**
   Below native. There is no supersampling to smooth anything, which is why edges looked jagged
   and why the first instinct ("raise the resolution") was pointing at a real gap.

## Is it worth changing the resolution? Mostly no — with one narrow exception

The question has an arithmetic answer, because the crop above converts a window size into what
the eye actually receives. Measured here:

```
window 2528 x 2780   ->   crop 2035 x 2199
                          2035/2528 = 0.805 across,  2199/2780 = 0.791 down
```

So about **80% of each side** survives, and the runtime wants 2064x2272. Working backwards, the
window that lands exactly on the runtime's recommendation is:

```
2064 / 0.805 = 2564        2272 / 0.791 = 2872        ->  about 2564 x 2872
```

That is **+4.8% pixels** from where it is now, and it closes the 0.97× gap — the only change to
the window with a clear, cheap, measurable payoff. Everything else is worse:

- **Raising it further** is supersampling, and it costs pixels quadratically while the benefit
  falls off quickly. 1.4× per side means about 3590x4020, twice the pixels, and on this machine
  that lands well past the VRAM cliff described below. MSAA buys the same smooth edges for far
  less.
- **Lowering it** costs visibly, because the image is already *below* the runtime's native
  request. There is no slack to give back. Do it only if the frame rate is genuinely unusable
  and MSAA is already off.

Two cautions before anybody acts on the 2564x2872 figure:

- **0.805 is not a constant.** It comes from this runtime's frustum and this field-of-view
  configuration. Re-measure it with `mirv_vr_crop` and the `view N recommended` line rather than
  copying the number — that is two lines of `console.log` and thirty seconds.
- **Untested above the display width.** 2564 is wider than this 2560-pixel display. The window
  already exceeds the display *height* happily under `setting.fullscreen 1`, so it plausibly
  works across too, but nobody here has run it. Try it and read the `swapchains WxH` line back;
  if CS2 clamps, the log will say so plainly.

In short: nudge it to about 2564x2872 once, verify from the log, and then leave the resolution
alone and spend the rest of the budget on MSAA.

## Why resolution is the expensive answer

To get anti-aliasing from supersampling you need roughly 1.4× per side after the crop, so a
window near **3590x4020** — twice the pixels. At 33 fps that buys about 17. In a headset, low
frame rate is worse than a jagged edge.

MSAA reaches the same edges far cheaper, because it multisamples geometry edges rather than
everything. Use it instead. It needs the hook's resolve path (experiment 23); without it MSAA
produces a **black headset with working audio** and no error anywhere.

## Measured, this machine

RTX 4070 Laptop, **8 GB VRAM**, 31 GB system RAM, Quest 3 over Link, window 2528x2780.

| `msaa_samples` | fps | scratch render targets | verdict |
|---|---|---|---|
| 0 | 32–35 | 0.85 GB | jagged edges |
| **2** | **35–36, stable** | **1.06 GB** | **what this machine should use** |
| 4 | 28.8 → 8.0, decaying | 1.27 GB | unusable: judder, VRAM exhausted |

The important row is the last one, and the important word is **decaying**. MSAA 4x did not cost
a steady 20% — it started near 29 and fell to 8 over ten minutes. That shape is memory
pressure, not arithmetic. Confirmed:

```
nvidia-smi:  8188 MiB total, 6617 MiB used   <- while running at MSAA 2x
RT 2528x2780 RGBA16161616F 2xMSAA : 112445440 Bytes
RT 2528x2780 RGBA16161616F 4xMSAA : 224890880 Bytes
```

Under 1.6 GB free at 2x, and 4x roughly doubles the multisampled targets. It does not fit, the
driver spills to system memory, and the frame rate collapses.

Note also that MSAA 2x measured **no slower than no MSAA at all** (35–36 against 32–35). On this
machine anti-aliasing was effectively free, and the whole cost of 4x was memory.

## The rule, expressed as the thing that actually binds

Not "a good GPU can do 4x" — a GPU with a big *number* and 8 GB still could not. The binding
constraint is **free VRAM at the chosen window size**:

- Measure `nvidia-smi --query-gpu=memory.total,memory.used --format=csv` *while a session runs*
  at MSAA 2x. That is the real figure; idle desktop numbers are meaningless.
- If the headroom is comfortably larger than the multisampled targets the engine already
  reports (`grep "xMSAA" console.log` shows each one's byte count), 4x is worth trying.
- **Then watch for ten minutes.** A configuration that is over the line looks fine at first.
  The tell is a frame rate that falls steadily rather than one that is simply low.

At 2528x2780, 4x needs roughly 2 GB of headroom that an 8 GB card does not have. A 16 GB card
plausibly does — untested, and it should stay marked untested until somebody runs it.

## The settings themselves

`<Steam>\userdata\<id>\730\local\cfg\cs2_video.txt`, edited with **CS2 closed** — the game
rewrites this file on exit and will overwrite changes made while it runs.

| key | value here | why |
|---|---|---|
| `setting.fullscreen` | `1` | See experiment 22. `0` makes Windows clamp the tall window to the display height, costing ~31° of vertical field of view. `1` keeps the full 2528x2780 in game, at the price of a known menu-panel fault that Fix 2 is meant to remove. |
| `setting.defaultres` / `defaultresheight` | `2528` / `2780` | The eye size. Must match `start-vr.ps1`'s `-Width`/`-Height`. |
| `setting.msaa_samples` | `2` | Anti-aliasing. `4` only with the VRAM headroom above. Needs the hook's resolve. |
| `setting.nowindowborder` | `1` | A title bar eats rows that are eye resolution. |
| `setting.r_csgo_cmaa_enable` | `1` | Cheap post-process AA, harmless alongside MSAA. |

## Warning signs, and what they actually mean

- **Black headset, audio fine, session FOCUSED, frames submitting.** A texture mismatch on the
  way into the swapchain. Since experiment 23 the hook says which; before it, this was silent.
- **Frame rate decaying over a session rather than merely low.** VRAM, not GPU load.
- **HUD elements with solid rectangles instead of transparency.** The panel's transparent-black
  clear is not running; look for `CreateRenderTargetView for the panel failed` in `console.log`.
- **`copy xN` in the frame-time line is not evidence.** It counts calls, not copies, and reads
  as reassurance while nothing is being copied at all.

## Tuning this automatically

`install-cs2-vr` could read the same two numbers this document is built on — total VRAM, and
the per-eye size the operator is going to run — and write `msaa_samples` accordingly, instead of
leaving every operator to find the 8 fps cliff themselves.

It should be conservative in exactly one direction: **default to 2 and let the operator raise
it.** A too-low setting looks like slightly rougher edges; a too-high one looks like a working
session that degrades into judder ten minutes later, in a headset, which is both harder to
diagnose and unpleasant to be inside. Anything it writes goes through the same
back-up-and-restore rules as `setting.fullscreen`, because this is the operator's own CS2
configuration and not ours.
