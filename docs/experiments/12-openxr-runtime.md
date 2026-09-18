# 12 — Choosing the OpenXR runtime without changing the machine

**Question.** [Issue #6](https://github.com/vr-meta/cs2-vr-spectator/issues/6) collected
every operational problem this project has had, and every one came from SteamVR rather
than from the code: it stalls the game, it refuses sessions after a few have been created,
it hangs CS2 unkillably if it is killed first, and without a headset it takes the desktop
hostage. For a Quest on Link, SteamVR is a translation layer this project has no use for.

The issue proposed switching to Meta's runtime by editing
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`, and flagged the cost: that is a
machine-wide setting affecting every VR application installed, and it needs administrator
rights.

**Answer.** It does not have to be machine-wide. The OpenXR loader honours the
`XR_RUNTIME_JSON` environment variable, which selects a runtime **for one process**. No
registry, no elevation, nothing left behind when the process exits.

## Shown rather than assumed

`tools/xr-probe` opens the loader by explicit path — exactly as the hook does — creates an
instance, and prints what answered. Creating an instance needs no headset; only
`xrGetSystem` does, which is why this can be run with everything switched off.

Same shell, one second apart:

```
=== default (registry says SteamVR) ===
XR_RUNTIME_JSON : (not set - the registry decides)
runtime         : SteamVR/OpenXR 2.17.10
headset         : none connected (XR_ERROR_FORM_FACTOR_UNAVAILABLE)

=== with XR_RUNTIME_JSON pointing at Meta ===
XR_RUNTIME_JSON : C:\Program Files\Meta Horizon\Support\oculus-runtime\oculus_openxr_64.json
runtime         : Oculus 1.207.0
headset         : none connected (XR_ERROR_FORM_FACTOR_UNAVAILABLE)
```

The registry was not touched. `ActiveRuntime` still points at SteamVR, and every other VR
application on the machine still gets SteamVR.

Two things worth noticing in that output:

- **No SteamVR process started.** Creating an instance against SteamVR's runtime did not
  launch `vrmonitor` or `vrserver`, so the probe is safe to run on a working desktop. The
  "SteamVR without a headset makes the desktop unusable" problem is about starting the
  SteamVR *application*, not about the runtime answering.
- **Both runtimes report no headset identically.** `XR_ERROR_FORM_FACTOR_UNAVAILABLE` is
  the runtime saying "I am here, there is no headset", not a failure to reach it.

## How it is used

```powershell
scripts\launch-cs2-experiment.ps1 -SelfBuilt -VrReady -MetaRuntime -ExecCfg vr ...
```

`Start-Process` passes the shell's environment to HLAE, and HLAE passes it to CS2, so the
variable reaches the process that matters and nothing else. The launcher also clears the
variable when the switch is absent, so a previous run in the same shell cannot leak into
the next one — the failure that would otherwise be maddening to diagnose.

`scripts/openxr-runtime.ps1` reports what the machine is set to and what is installed. It
can also make the machine-wide change, recording the previous value so `-Restore` undoes
it, but it says plainly that the per-process route is the one to prefer.

## What this does not answer

Whether Meta's runtime is actually better here. That needs a headset and a measurement,
and it belongs with the frame budget in
[issue #1](https://github.com/vr-meta/cs2-vr-spectator/issues/1) rather than here. What
this settles is that trying it costs a command-line switch instead of an administrator
prompt and a machine-wide change — which is the difference between an experiment and a
decision.

The specific SteamVR failures in issue #6 stay documented in
[`../workflow.md`](../workflow.md) and in the runtime-handling notes, because SteamVR
remains the default and someone will hit them again.
