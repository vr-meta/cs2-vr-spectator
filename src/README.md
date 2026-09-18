# Source

The C++ this project actually wrote, kept here rather than only inside a patch file.

`AfxHookSource2/` is built as part of HLAE's `AfxHookSource2` module: the files are copied
into a checkout of [advancedfx](https://github.com/advancedfx/advancedfx) and compiled
with it. They are ours, MIT like the tree they join.

| | |
| --- | --- |
| `MirvVr.h` / `MirvVr.cpp` | the engine-facing half — a camera pose per render pass, which is what makes stereo possible at all |
| `MirvVrXr.h` / `MirvVrXr.cpp` | the headset-facing half — the OpenXR session, swapchains, frame loop, controller input |

The edits to *advancedfx's own* files stay in
[`../docs/patches/`](../docs/patches/): a handful of hooks in `main.cpp`,
`RenderSystemDX11Hooks.cpp`, `RenderServiceHooks.cpp` and two `CMakeLists.txt` entries.
Those have to be a patch, because they are changes to someone else's code. Ours do not,
and should not be — a 900-line file is not reviewable as a diff.

## Building

See [`../docs/install.md`](../docs/install.md). In short: clone advancedfx, copy these
files in, apply the patches, build.

```powershell
Copy-Item src\AfxHookSource2\* D:\Dev\cs2-vr-tools\advancedfx\AfxHookSource2\ -Force
```

`.github/workflows/build-hook.yml` does exactly this, so the copy step is exercised on
every change.

## What to know before reading

Two things explain most of the shape of this code, and both were measured rather than
assumed:

**The engine resolves the camera once per frame, outside the render pass loop** — but the
`CViewRender` object it writes into is persistent and re-read by every pass. So the eyes
are separated by rewriting that object between passes, which is what `MirvVr` does.
Position, orientation and field of view all ride the same lever. Field offsets are for
CS2 build 2000908 and will move on a game update:
[`../docs/05-view-setup-point.md`](../docs/05-view-setup-point.md).

**Every OpenXR call except the pose handover happens on the render thread**, where HLAE
hands over the finished texture for a pass. Splitting `xrBeginFrame` and `xrEndFrame`
across threads is a race; the price is that a frame renders with the poses located during
the previous one, and the projection layer reports *those* poses, not fresh ones — get
that wrong and the world swims whenever the head turns.
