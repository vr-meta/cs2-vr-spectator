# 23 — MSAA, and the black headset that said nothing

## The question

The operator, worn, said the edges were jagged: *«мне не нравятся лесенки в игре»*. The
obvious lever was resolution, since the per-eye image is the window and the window can be
made bigger. The obvious lever was the wrong one.

## What reaches the eye, measured

Asked of the live session through the control server's `POST /command`, which is the first
time that tooling answered a real question rather than being tested:

```
mirv_vr_crop
  Current: on. Last rectangles: eye 0 2035x2199 at (0,503), eye 1 2035x2199 at (493,503).

AFXVR: view 0 recommended 2064x2272
AFXVR: view 1 recommended 2064x2272
AFXVR: swapchains 2528x2780, back buffer format 27 -> swapchain 29
```

So:

| | pixels |
|---|---|
| rendered per eye | 2528 x 2780 |
| **submitted** per eye, after the crop | **2035 x 2199** |
| what the runtime asks for | 2064 x 2272 |

**0.97× of the runtime's recommendation.** There is no supersampling at all — the compositor
is very slightly *upscaling*. Nothing anywhere was smoothing an edge: no MSAA, and no
downsample to hide one. That is the whole reason the jaggies were visible.

It also shows that 36% of every rendered frame is discarded by the crop (4.5M of 7.0M pixels
reach the eye). That is inherent to rendering symmetric and submitting the runtime's
asymmetric frustum — `mirv_vr_crop`'s own help explains why it is done — but it is paid for.

Raising the window was therefore the expensive answer: supersampling at 1.4× per side needs
about 3590x4020, twice the pixels, and the session was already at 33 fps against a headset
that wants 72.

## The fault that made MSAA impossible, and how it presented

`setting.msaa_samples 4` was tried first. The result was a **black headset with working
audio**. Everything that could be checked said the session was healthy:

```
session FOCUSED
28.0 frames/s submitted at 2528x2780 per eye
copy 0.01 x2                     <- two eye copies, every frame
/state -> world_in_eyes: true, per_eye [2528,2780]
```

`CopyResource` requires **identical sample counts**. With MSAA on, CS2's captured render
target is multisampled (`RT 2528x2780 RGBA8888 4xMSAA` in its own log) while an XR swapchain
image is always single-sample. In that case `CopyResource` does **nothing**: it returns void,
sets no error, raises nothing but a debug-layer message nobody was listening for. The
swapchain images stayed at their cleared value and were submitted faithfully, 28 times a
second.

`copy x2` counts the calls, not their effect. That number was the most misleading thing in
the log, because it looked like evidence the copy was happening.

This is the same failure mode the file's own comments already warned about twice — "every
CopyResource had a source of one size and a destination of another, which does nothing at all
and silently" — arriving through the one dimension nobody had thought to guard: samples.

## The fix

`ResolveSubresource` is the D3D11 call for multisample to single-sample, and resolving **is**
the anti-aliasing: it averages the samples. So one helper, `CopyOrResolve`, now stands in
front of all three copies into a swapchain (eye, panel, menu panel):

- sizes differ → say so, submit nothing;
- sample counts equal → `CopyResource`, as before;
- multisampled source into single-sample destination → `ResolveSubresource` with the concrete
  format `EnsureSwapchains` already chose from the typeless family;
- anything else → say so.

Every branch that cannot produce a picture now says why, once. **A black headset must never
again be how this project reports a texture mismatch** — that is the part worth keeping even
if MSAA is later turned off.

## Measured after the fix

```
[RenderPipelineCsgo] RT 2528x2780 RGBA8888 4xMSAA
AFXVR: the game renders 4 MSAA samples; they will be resolved on the way in.
AFXVR: eye: the game renders 4 MSAA samples; resolving them into the swapchain.
```

| `msaa_samples` | fps | scratch render targets |
|---|---|---|
| 0 | 32–35 | 0.85 GB |
| **2** | **35–36, stable** | **1.06 GB** |
| 4 | 28.8 → 8.0, decaying | 1.27 GB |

**The first reading of this was wrong and is worth recording as wrong.** Seeing 26.5–28.5 just
after launching at 4x, this note originally said MSAA cost "about 20% of the frame rate" — a
steady price worth paying. It was not steady. Over ten minutes it fell to 8.0, the operator
reported judder, and the shape of the decay is the whole diagnosis: a constant cost does not
decay. It was **memory**.

```
nvidia-smi:  8188 MiB total, 6617 MiB used   <- while running at MSAA 2x
RT 2528x2780 RGBA16161616F 2xMSAA : 112445440 Bytes
RT 2528x2780 RGBA16161616F 4xMSAA : 224890880 Bytes
```

Under 1.6 GB free at 2x on an 8 GB card, and 4x roughly doubles the multisampled targets. It
does not fit, the driver spills to system memory, and the frame rate collapses.

So the honest result is better than the one first claimed: at 2x, **anti-aliasing was free** —
35–36 fps against 32–35 without it, which is no worse within the noise. The entire cost of 4x
was VRAM, not arithmetic. `docs/settings.md` carries the rule this implies.

## And a second fault the same setting exposed

With MSAA on, the HUD panels showed solid rectangles where they should have been transparent —
the operator's words: *«на элементах HUD вместо прозрачности свои рамки»*, and after the resolve
landed, *«прозрачность не появилась»*.

The first theory was that `ResolveSubresource` averages the alpha channel and smears a clean
zero into intermediate values. That theory was wrong, and the log already knew:

```
AFXVR: CreateRenderTargetView for the panel failed (0x80070057).
```

`E_INVALIDARG`. The panel's transparent-black clear builds a render target view with
`D3D11_RTV_DIMENSION_TEXTURE2D`, and on a **multisampled** texture that is invalid — it must be
`TEXTURE2DMS`. The view was never created, `g_PanelClearRtv` stayed null, the clear never ran,
and every pixel the HUD did not touch kept whatever the buffer already held. Opaque.

Fixed by choosing the view dimension from the sample count. It took minutes instead of another
worn session **only because somebody had written that warning defensively long before MSAA was
ever tried.** That is the argument for the warnings added in this same change.

The alpha probe needed the same treatment: `ProbePanelAlpha` read back through
`CopyResource(pStaging, pTexture)`, which under MSAA silently copies nothing, leaving the probe
to count uninitialised staging memory and report it as Panorama's alpha. It now resolves into a
default-usage texture first. A diagnostic that lies confidently is worse than no diagnostic, and
MSAA is now a configuration this project recommends.

## What was learned that outlives the fix

- **Per-eye image quality is not the back buffer size.** It is the crop, and the crop was
  below the runtime's recommendation. Any future "make it sharper" question starts with
  `mirv_vr_crop` and `view N recommended`, not with the window.
- **`copy xN` in the frame-time line counts calls, not copies.** It cannot distinguish a
  working submission from a silent no-op, and it read as reassurance during an hour when the
  headset was black.
- **A negative result worth not repeating:** Source 2's `con_filter_enable` /
  `con_filter_text_out` were accepted by the engine but did not change `-condebug` output at
  all — log growth 18194 B/s before, 17784 B/s after. The console filter applies to the
  on-screen console, not the file.
