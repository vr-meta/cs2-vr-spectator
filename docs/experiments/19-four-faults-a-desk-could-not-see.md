# 19 — Four faults a desk could not see

All four of these were shipped after passing every check available without a headset, and
all four were found within a few minutes of somebody wearing one. They are collected
together because the pattern is the point: each survived because the desk test and the
worn case differ in a way that was not obvious until afterwards.

## 1. The camera was frozen wherever the session started

**Report.** "X and Y always put me back in the same place, where I appear at the start."

**Measured.** `mirv_vr_views`, three times across 42 seconds of a *playing* demo:

```
14:13:40  org (-1776.09 -1974.80 -201.78)
14:14:11  org (-1775.90 -1974.80 -202.15)
14:14:17  org (-1775.81 -1974.84 -202.27)
14:14:22  org (-1775.98 -1974.78 -201.94)
```

Under a centimetre of movement, all of it the eye offset swinging with the head. The
spectated player crossed half of Mirage in that time. The midpoint of the two eyes was
integer-clean at Mirage CT spawn for the whole session.

**Cause.** `AfxVr_BeforeViewSetupRead` puts the base camera back into the view struct at
the top of the trampoline, before HLAE reads the origin out of it. It exists because the
eye poses are written into a persistent object between passes, and the engine would
otherwise read the last pass's write back as its own camera and accumulate it.

That is right only while the engine is not computing a camera of its own. On a **paused**
demo it does not, the struct still holds our write, and restoring is exactly correct. On a
**playing** demo it writes a fresh camera every frame, the restore threw it away, and
`AfxVr_AfterViewSetup` then recorded the restored value as the new base. The base could
never change again.

**Why the desk missed it.** Experiments 03 through 07 were all run paused — that is how the
mechanism was discovered and how every stereo pair was compared. Paused, the two behaviours
are indistinguishable. Free look hid the rest: the orientation comes from the head, so only
the position was stuck, and until free look became the default nobody followed a moving
player for long enough to notice.

**Fix.** Restore only when the struct still holds, exactly, the last thing this module
wrote — which is the question "has anyone else been here". Every writer records what it
wrote, including the head pose the trampoline writes on our behalf. The decision is pure
arithmetic and lives in `MirvVrMath.h` with tests.

The comparison deliberately ignores the fov field, because the engine rescales that in
place for reasons of its own (experiment 18) and a change there does not mean the camera
was recomputed.

## 2. The controller was pressing nothing

**Report.** "The triggers do not switch the camera, but the arrow keys on the keyboard do."

The same command, `+attack`, by two routes: one worked and one did not.

**Cause.** The demo's spectator controls are driven from the **key event**, not from the
button state a console `+attack` sets. The on-screen hint reads "[MOUSE1]: Next Player"
because it is a binding lookup in the input layer, and that layer never sees a command
dispatched through `ExecuteClientCmd`.

So X and Y had never switched players either. It was masked all day by fault 1: every
switch landed the viewer in the same place regardless, so there was nothing to see.

**Fix.** Synthesise a real key with `SendInput` and the scancode flag — the same thing
`scripts/send-key.ps1` does from outside, which was already known to work. The keys are
`RIGHT`, `LEFT` and `UP`, so the controller takes exactly the path the operator had proved,
through the binds in `vr_keys.cfg`.

That is a genuine coupling between a config file and a DLL, and it is stated in both. The
hook also checks the game window is in the foreground and says so loudly when it is not:
with a headset on, a window that has quietly lost focus is invisible, and the key would
land in some other application.

## 3. Whoever you watched was frozen

**Report.** "I switch to a player, turn the camera, and see that everyone is running, while
*he* is frozen in position."

Only the target. Everybody else animated and moved normally.

**Cause.** TrueView. It re-simulates the spectated player on its own clock, advanced once
per rendered frame — and HLAE runs the client frame path three times per real frame for the
two eyes, so that clock is stepped, rewound and re-stepped every frame. The console had
been saying so on every switch, in a line nobody had connected to the symptom:

```
[Prediction] Not enough TrueView command lookahead.  Desired tick 60279 (2 ahead).
```

Everyone else is plain interpolated demo data, which is why only the one entity whose
prediction time keeps going backwards is affected.

**Fix.** `cl_demo_predict 0` in `vr.cfg`. The log goes silent and the operator's verdict was
"the camera works as it should". `cl_demo_predict 2` puts TrueView back.

**Why the desk missed it.** Nothing was ever spectated in-eye on a playing demo at a desk;
the forced-pass measurements all ran from a free camera or a freeze.

## 4. The HUD panels leaned

**Report.** With a mirror screenshot: the score strip sloping down to the right by about
ten degrees, the timeline up to the right by thirty-five, the radar tilted too — opposite
in sign above and below eye level, and proportional to the elevation.

**Cause.** One sign in a quaternion product expanded by hand. `PlaceRegion` had
`z = +sin(yaw/2)·sin(el/2)` where `qYaw ⊗ qPitch` gives minus that. The wrong sign is
`qPitch ⊗ qYaw` — a pitch about the **world's** X axis instead of the quad's own — which
rolls every panel by about `sin(yaw)·elevation`.

**Why the desk missed it.** That roll is **exactly zero at yaw 0**, and a desk check faces
forwards. It only appears once the anchor is placed at some other yaw, which is to say once
somebody wears it and happens not to be facing north.

**Fix.** Composed with the same helper the eye code uses rather than expanded by hand, in
`MirvVrMath.h`, with the property asserted over a grid of yaws and pitches: the panel's own
horizontal axis must stay horizontal. The wrong sign is pinned as a negative control — at
yaw 90 and elevation 30 it tilts by half.

## What to take from it

- **Pause is not a neutral condition.** Two of these four hid behind a paused demo. Any
  mechanism that writes into engine state needs one check on a playing one before it is
  believed.
- **Yaw 0 is not a neutral condition either.** A rotation bug proportional to `sin(yaw)`
  is invisible from a chair facing the monitor.
- **The same command by two routes is not the same command.** A console dispatch and a key
  press reach different layers, and only one of them is what the game's own UI is talking
  about when it names a key.
- **One fault can mask another.** The keyless taps had been broken all day and produced no
  report, because the frozen camera made a working switch look identical to a broken one.
