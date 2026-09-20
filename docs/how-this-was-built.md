# How this was built

A narrative source for writing about the project: what was tried, in what order, which
explanations turned out wrong, and the numbers that settled each question. Every figure here
comes from an experiment note in `docs/experiments/`, which is linked at each point so a claim
can be checked rather than taken.

It is deliberately written around the wrong turns. The parts worth reading about are not the
things that worked first time.

---

## 1. The question, and a dead end that took one experiment

The starting point was not ambition. It was watching played matches, wondering whether a
recording could be watched in VR, and having two pieces of prior art to suggest it was not
absurd — [Portal 2 VR](https://github.com/vr-meta/portal2vr), already run on this machine, and
[L4D2VR](https://github.com/sd805/l4d2vr) as an example of the shape of the problem.

The obvious first move was to look for stereo support CS2 already had. It has some: the demo
playback path ships convars with names that promise exactly this.

**They do nothing.** The demo eye-offset convar has no effect, and the multiview path is
absent from the build ([experiment 00](experiments/00-results.md)). That was the whole of the
first experiment, and its value was negative in the useful sense: it removed an approach
before any time was spent building on it.

## 2. The thing that made it possible was built for something else

Stereo needs one frame rendered twice from two positions with nothing advancing in between.
That is the expensive half, and **it already existed** — in
[HLAE](https://www.advancedfx.org/), a tool for making Counter-Strike films, which re-renders
one frame several times from a single simulation state to produce depth passes, mattes and
isolated players ([experiment 02](experiments/02-multipass.md)).

Nobody built that for VR. It is a filmmaker's feature, and it happens to be the hardest part
of a stereo bridge, already written and already debugged.

## 3. One lever, found by failing to use it from a config

The plan was to set a different camera per pass from HLAE's own configuration. That failed,
and the failure was informative: **the view is resolved once per frame, before any pass
command runs** ([experiment 03](experiments/03-per-pass-camera.md)). No amount of
configuration can reach inside the pass loop.

But the object holding the resolved camera is **persistent, and re-read by every pass**. So a
change inside the render path — rewriting that object between passes — gives each eye its own
camera. Position, orientation and field of view all travel through the same single write.

Measured, on a paused demo ([experiment 04](experiments/04-per-pass-camera.md)):

| take | frames differing between the two passes |
| --- | --- |
| control (no per-pass write) | **0.00%** |
| test (per-pass camera) | **88.89%**, with correct parallax |

And the check that the pair was honest rather than merely different
([experiment 05](experiments/05-stereo-pair.md)): with eye separation set to zero on a
*playing* demo, the two eyes stayed identical while consecutive frames differed by up to 98%.
Nothing advances between passes — which is the property the whole approach depends on.

## 4. SteamVR, and why it was dropped

OpenXR came up inside CS2 and found the Quest through Link
([experiment 08](experiments/08-openxr-instance.md)). That worked. Almost everything around it
did not, and the pattern took a while to see:

> **Every operational problem this project had came from the runtime, not from the code.**

The list, each item learned by losing time to it:

- Started without a headset, SteamVR takes foreground focus and makes the desktop unusable —
  while giving nothing, since `xrGetSystem` returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE` anyway.
- After several sessions are created and destroyed quickly, `xrCreateSession` starts failing
  with `XR_ERROR_RUNTIME_FAILURE` and only a restart clears it.
- Kill the runtime with an `XrInstance` still open inside CS2 and the game hangs — alive,
  burning a core, not pumping messages, holding the hook DLL until a reboot. `Stop-Process
  -Force` does not touch it.
- An uncapped game and a VR compositor fight over the GPU and both lose. CS2 stops presenting
  (`QueuePresentAndWait() looped for 23 iterations`) and, because its main loop is stuck
  there, stops handling console commands — which looks exactly like a dead console and is not.

For a Quest on Link, SteamVR is a translation layer over the Oculus runtime with no purpose in
this project. The obvious switch was to edit
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` — **machine-wide, administrator rights, and it
changes every VR application installed.** That cost is why it sat unexamined for a while.

**The finding that unblocked it was small and is worth the paragraph**
([experiment 12](experiments/12-openxr-runtime.md)): the OpenXR loader honours the
`XR_RUNTIME_JSON` environment variable, which selects a runtime **for one process**. No
registry, no elevation, nothing left behind on exit.

```
=== default (registry says SteamVR) ===
runtime : SteamVR/OpenXR 2.17.10

=== with XR_RUNTIME_JSON pointing at Meta ===
runtime : Oculus 1.207.0
```

The registry was never touched, and every other VR application on the machine still got
SteamVR. That is the difference between a decision and an experiment — and it is the reason
the switch happened at all.

*Aside worth keeping: the same mechanism is what would make a simulated headset safe to test
against today, since a simulator is just another runtime JSON and cannot disturb the worn
setup.*

## 5. What changed when SteamVR went away: the field of view

Dropping SteamVR moved a constraint that had been invisible underneath it.

**The Oculus runtime reports `fovMutable = false` and ignores a submitted field of view.** You
do not get to tell it the frustum you rendered. You render symmetric, and submit the runtime's
own frustum with the image rectangle cropped to it.

That makes one question load-bearing that had not been before: *what, exactly, do we ask the
engine for?* And the answer turned out to be genuinely ambiguous.

CS2's `fov` number is the horizontal angle **at 4:3** — measured against the engine's own
projection matrix, not assumed ([experiment 06](experiments/06-per-eye-projection.md)). The
engine derives the vertical from it and recomputes the horizontal for the window's real aspect.
On the tall per-eye buffer, a request for 108° comes out as about 86°, so the hook asked for
127.3° to get 108° back.

**But is that still the convention of the same field by the time a pass runs?** The engine's
second computation is inlined; if it rewrites the field in place, writing the 4:3 number there
scales it twice. The two answers are not close:

| if the field at pass time is | we ask | we render | the world is shown at |
| --- | --- | --- | --- |
| the 4:3 number | 127.3° | 108° | 1.00 |
| the true horizontal | 127.3° | 127.3° | **0.68** |

The second case is a wide-angle lens with the world shrunk to two thirds, plus a residue on
every head turn that no reprojection can remove.

**Three rounds of reading HLAE and the engine did not settle it**, and both desk measurements
that would have were unavailable: the projection matrix the hook can read is built after the
passes, and our own code masked the field's value in the log at the trampoline.

It was settled in about thirty seconds ([experiment 18](experiments/18-what-the-fov-field-means.md))
by putting the number on the controller triggers and handing it to the person wearing the
headset. The sweep converged on **0.853**, against a prediction of 108/127.3 = **0.848** —
inside one step of the dial.

Two details make that work, and both are transferable:

- **The dial must move exactly one thing.** Half the existing fov controls moved the render
  and the crop together, which the crop makes invisible *by construction* — a perfect
  invariance check and a useless instrument.
- **The criterion must be about motion, not appearance**: *"look at a far corner, turn and nod
  your head, stop when the corner stays nailed to the world; ignore how big things look."*
  Appearance is where taste and the actual fault get confused.

**The answer:** the same field means the 4:3 number at the once-per-frame trampoline and the
true horizontal angle at pass time. Both true, of the same field, at different moments — which
is why no amount of reading settled it.

## 6. The long fight: resolution

This is the thread that never fully closed, and it has more separate causes than it looks.

**Where the number came from.** SteamVR asked for **2528x2780 per eye** — its own
supersampling over the Quest 3's native ~2064x2208 — and that became the window size
([experiment 08](experiments/08-openxr-instance.md)). The Oculus runtime asks for
**2064x2272**.

**Why that is not simply too big.** The crop to the runtime's frustum throws away the corners:
against a 2528x2780 buffer the eye actually receives 2035x2199, so **36% of every frame is
rendered and discarded** — inherent to the approach, and paid for every frame. After the crop
the image is 0.97× the runtime's recommendation, i.e. *below* native, with no supersampling
left to smooth anything ([`docs/settings.md`](settings.md)). The instinct "the edges are
jagged, raise the resolution" was pointing at a real gap, and the arithmetic says the window
that lands exactly on the recommendation is about 2564x2872 — **+4.8% pixels**, the only
change to the window with a clear payoff.

**Then the consequences of a window taller than any monitor:**

- Windows clamps it, so Panorama puts the console's input line off the bottom of the screen.
  **There is no usable console in a worn session** — which is why the hook grew a named pipe,
  and later a control server, purely so somebody could adjust things for a person who cannot
  reach the keyboard.
- `setting.fullscreen 1` in CS2's own `cs2_video.txt` **beats `-windowed` on the command
  line**, forces a display-mode change, and snaps an impossible size to the driver's nearest
  legal mode ([experiment 22](experiments/22-which-rectangle-is-which.md)). A stray 2048x1536
  had been seen once and written off as noise; it was the driver, and the number was
  reproducible.
- That produces a frame rendered into a sub-rect of a larger buffer, the rest black and never
  written. CS2's menu panel then showed the padding as content while the click path stretched
  across the whole buffer: **hovering black activated the item that would have been there if
  the frame filled the rectangle.**
- Setting `fullscreen 0` fixes the panel and costs about **31° of vertical field of view**,
  because Windows then clamps the window to the display height and Source renders the
  *horizontal* angle with the vertical following from the image's shape. Tried, measured,
  reverted ([experiment 23](experiments/23-msaa-and-the-silent-black-headset.md)).
- And **CS2 rewrites `cs2_video.txt` on exit**, so nothing can be built on restoring a value
  at shutdown. Restore at *launch* if a backup exists, then take a fresh one — which makes a
  crash, a kill and a clean exit the same case.

**Anti-aliasing turned out to be the cheap win the resolution was not.** MSAA 2x costs nothing
measurable — 35–36 fps against 32–35 without it. MSAA 4x is not slow, it **exhausts VRAM**: 28
fps decaying to 8.0 over ten minutes, with judder. *A constant cost does not decay* — the shape
of the number identified it as memory before `nvidia-smi` confirmed 6617 MiB of 8188 in use.

## 7. What the method turned into

None of the following was planned. Each is a rule that exists because breaking it cost a day.

**Reading code and wearing the headset find almost disjoint sets of faults.** In one day,
wearing it found eight real faults nothing else would have — judder, sound not following the
head, a frozen camera, the field of view, controllers pressing nothing, the spectated player
frozen, the HUD leaning, hands glued to the head. In the same day a code review found fourteen
that no amount of wearing would have diagnosed, including hand aiming that was **dead on
arrival**: selecting it zeroed the gain estimate, the servo refuses a zero gain and sends
nothing, and with nothing sent the estimator can never learn.

**The person wearing the headset is an instrument, and their offhand remarks are primary
evidence.** Three wrong conclusions were caught by casual observations — "the bots are not
moving" (the demo was not replaying, so a measurement of a dead convar was measuring a still
image), "maybe I moved it with the mouse" (which forced a stricter control), and "maybe take a
ready-made demo" (which unblocked the whole experiment). When the numbers and the person
disagree, the person is usually right about the state of the world.

**Anything that can be expressed as arithmetic goes in one header with no engine, no Windows
and no OpenXR**, so it builds and tests alone in seconds. It is the only part verifiable
without a headset.

**Two structural rules account for more than half the defects found on one day:** every held
input belongs in the single held-input list — the hook physically holds keys down on behalf of
someone who cannot reach the keyboard, and a forgotten release leaves them walking into a wall
— and every decision the render thread acts on travels in the frame ticket, because the render
thread runs a varying 0.09 to 0.69 frames behind the engine thread, so any shared global may
already belong to the next frame.

**The instrument can disturb the experiment.** A menu panel appeared in front of the operator
twelve times in twenty-four seconds during the very demo whose behaviour was being
investigated. The cause was the tooling's own console window taking the foreground, which the
game reads as a cursor event. It presented as a bug in the hook.

**Say what was measured, and say when something was not.** The `docs/experiments/` directory
is mostly a record of finding out, and several of its most useful entries exist to document an
explanation that was confident and wrong.

---

## Quick numbers

| | |
| --- | --- |
| Per-eye render | 2528x2780; after crop to the runtime frustum, 2035x2199 |
| Discarded per frame by the crop | 36% |
| Frame rate | 35–36 fps, against the 72 the headset wants |
| Of which is not rendering | more than half — three scene traversals are 11 ms of 21–26 |
| Field of view | 108° horizontal, settled worn to ±1.1° |
| Vertical fov lost to `fullscreen 0` | ~31° |
| MSAA 4x | 28 fps decaying to 8.0 — VRAM, not GPU load |
| Pure-logic assertions, no engine or headset | ~3400 |
| Experiments recorded | 24 |
