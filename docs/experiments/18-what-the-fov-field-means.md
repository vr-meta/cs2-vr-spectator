# 18 — What the field of view field means, settled by a person wearing the headset

**The question.** [Experiment 06](06-per-eye-projection.md) established what CS2's `fov`
number means: it is the horizontal angle **at 4:3**. The engine derives the vertical from
that and recomputes the horizontal for the window's real aspect. Measured against the
engine's own projection matrix, not assumed — at fov 90 on a 16:9 window,
`proj[0][0] = 0.75` and `proj[1][1] = 1.3333`.

On the 2528×2780 portrait buffer that turns a request for 108° into about 86° rendered, so
to get the 108° the Quest's lenses need, the hook asked for 127.3°.

What nobody could establish was whether that is still the convention of the *same field* by
the time a render pass runs. The engine's second fov computation is inlined into
`SetUpView`; if it rewrites the field in place, then by pass time the field holds the
already-scaled horizontal angle, and writing the 4:3 number there scales it twice.

The two answers are not close:

| if the field at pass time is | we ask | we render | the crop assumes | the world is shown at |
| --- | --- | --- | --- | --- |
| the 4:3 number | 127.3° | 108° | 108° | 1.00 |
| the true horizontal | 127.3° | 127.3° | 108° | tan54/tan63.65 = **0.68** |

The second case is a wide-angle lens with the world shrunk to two thirds, and a residue of
(1/0.68 − 1)·θ on every head turn that no reprojection can remove.

Three rounds of reading HLAE's and the engine's code did not settle it. Two of the desk
measurements that would have settled it were unavailable: the projection matrix the hook
can read is built once a frame from the base camera, after the passes, and
`AfxVr_BeforeViewSetupRead` masks the field's own value in any log taken at the trampoline.

## The instrument

Put the dial on the triggers and give it to the person wearing the headset.

`mirv_vr_triggers fov` makes the right trigger widen what the engine is **asked** for and
the left narrow it, one per cent a step, held to repeat. What the runtime is **told** does
not change.

That asymmetry is the whole design. Every other field-of-view control in the hook moves the
ask and the claim together, which with the crop in place is invisible by construction — the
image and the rectangle cut out of it change in step, and the result differs only in
sharpness. That property is useful as an invariance check and useless as a dial. This one
changes how much world goes into the image while the frustum it is shown in stays honest,
which is the one thing an eye can judge.

And the criterion given to the operator was not "until it looks right":

> Look at a far corner, turn and nod your head, and stop when the corner stays nailed to
> the world. Ignore how big things look.

## The result

The sweep, from the log: 0.870 → 0.836 → 0.879, then a narrowing bracket 0.844 / 0.853 /
0.861, converging on

```
AFXVR: ask scale 0.853 - engine asked 108.5 deg, runtime still told 108.0
```

One step is one per cent, so this is 0.853 ± 0.009 — **108.5° ± 1.1°**.

The prediction for "the field is already scaled" was 108/127.3 = 0.848. Inside one step of
the dial.

**At pass time the field holds the true horizontal field of view of the image, already
scaled for the window's aspect.** The 4:3 model is right for what the trampoline reads and
for the matrix the engine builds once a frame; it is wrong for the field the per-pass write
goes into. Both are true, of the same field, at different moments.

## What it explains

Every field-of-view complaint of the day, all of which had been attributed to other things
in turn: "the fov is off", "I would reduce it", "the world pulls when I turn", "when I look
down at his legs the angle changes a lot and his legs ride away forward". A 1.47× wide-angle
lens with a 0.68 scale error does all of that.

## Baked in

- The aspect fix is **off** for the per-pass write: the wanted horizontal goes in directly.
  `SourceFovForWanted` stays, because it is still the right arithmetic for anything written
  at the trampoline.
- A pass with no eye gets back **the field's own value**, captured at the top of pass 0
  before anything is written — not `g_BaseFov`, which is the trampoline's number and
  therefore the other convention. That needs no conversion and cannot be wrong.
- `mirv_vr_reset` no longer turns the aspect fix back on.
- The ask scale survives a reset. It is a calibration, and a reset that silently eats a
  calibration is an evening.

The dial stays available as `mirv_vr_triggers fov`, because it is the right way to re-check
the convention after any change to the crop or the window shape. The triggers default to
switching players.

## The lesson worth keeping

A question that three readings of the source could not answer was answered in thirty
seconds by one person, one dial, and a criterion that did not depend on taste. The
instrument was cheap: a multiplier in one function, an edge-triggered repeat, and a printed
line per step so the number could be read out of the log afterwards.
