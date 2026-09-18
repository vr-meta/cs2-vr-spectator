# Experiment 05 — is the stereo pair correct?

Date: 2026-09-18. Self-built `AfxHookSource2.dll` with the phase B per-pass camera,
CS2 build 2000908, `pro_mirage.dem`, windowed 1280x720.

Phase C of [`../04-plan.md`](../04-plan.md). Experiment 04 proved the eyes *can* differ.
This asks whether they differ *only* by viewpoint, at a real interpupillary distance, on
the content that breaks stereo most often.

Viewable pair: <https://claude.ai/artifact/3iESVvwPKZjiEDLX9eAC8x>

## Scale

CS2 units are inches, so a 63 mm interpupillary distance is **2.5 units** — eight times
smaller than the 20 units experiment 04 used to make the effect unmissable.

Sanity check before trusting any image: at 90° horizontal over 1280 px the focal length
is 640 px, so disparity ≈ `2.5 × 640 / distance`. A crate at ~60 units should shift about
27 px and the viewmodel at ~20 units about 80 px, while distant buildings should barely
move. Measured on the pair, they do. The arithmetic and the picture agree.

## The measurement that mattered

A stereo pair is only correct if both eyes see the same instant. The pass loop calls
`SceneSystem::FrameUpdate` between passes, which could advance smoke, particles and
tracers — and eyes seeing different moments of the same smoke is exactly the error that
is unbearable in a headset and invisible in a screenshot.

It needs no visual judgement to test: **set the separation to zero on a *playing* demo
and the two passes must produce the same image.**

Playing demo, live smoke filling half the frame, 18 sampled frames of one take:

| comparison | pixels differing | max deviation | reading |
| --- | --- | --- | --- |
| between eyes, separation 0 | 0.00 – 0.17 % | 1 – 3 | same instant |
| frame N vs N+1, one eye | 45 – 98 % | 255 | the scene is moving hard |
| between eyes, separation 2.5 | 49.05 % | 193 | parallax |

The second row is what makes the first meaningful: the scene is changing enormously
between frames, so "the eyes are identical" is a real constraint and not an artefact of
nothing happening.

Earlier takes agree: paused demo with smoke, worst frame 0.34 % / max 2; playing demo
without smoke, worst frame 0.40 % / max 2.

## The one blemish, and what it was

One frame of the smoke take reached a deviation of 33 rather than 3. Rather than round it
away, amplify the difference eight times and look at it (`scripts/diff-image.ps1`):

The difference image is black across the entire frame — the whole smoke cloud included —
with one bright patch in the top right corner: the kill feed. That notification's fade is
driven by wall-clock time, and the two passes are microseconds apart in wall-clock time.

So the residual is UI, not scene. Volumetric smoke, the thing most likely to break,
came out identical to the byte.

## Result

**Phase C passes.** The pair differs only by viewpoint; the simulation holds still across
passes; real-IPD parallax is geometrically correct.

## What it does not prove

- Both passes render at **window resolution**, not per-eye resolution (note 03).
- Both use the game's own **symmetric projection**. A headset needs an asymmetric frustum
  per eye from `xrLocateViews`. The projection is built in
  `New_CViewRender_UnkMakeMatrix`, once per frame — the same shape of problem the camera
  turned out to be, and probably the same shape of answer.
- Only **position** is offset. Head tracking also needs the view angles, at `+0x4b8` in
  the same struct.
- The **HUD** is painted identically into both eyes at screen depth, which in a headset
  reads as a pane of glass welded to the viewer's face.
- The kill-feed finding generalises: anything in the UI animated by wall-clock time will
  differ slightly between eyes. Harmless on disk, worth remembering in a headset.

## Method notes

Two tooling problems cost time and are fixed rather than worked around:

- `SetForegroundWindow` is refused whenever the calling process is not itself in the
  foreground, and it fails **silently** — the key then goes to whatever window does have
  focus and the experiment looks like the bind does nothing. Same failure mode as the
  open console swallowing F-keys in experiment 00. `scripts/send-key.ps1` now attaches to
  the foreground thread's input queue, falls back to a synthetic ALT tap, and throws if
  the game is still not in front.
- Comparing a 1280x720 pair in a PowerShell loop takes about ten seconds, which makes
  scanning a take impossible. The pixel loop in `scripts/analyse-take.ps1` is C# now:
  29 frames in under a second.
