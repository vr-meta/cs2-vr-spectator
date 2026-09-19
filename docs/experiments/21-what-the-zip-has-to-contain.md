# What the hook actually needs from the HLAE tree

Date: 2026-09-19. Phase 0, item 5 of [the release plan](../07-release-plan.md): before a
zip can be assembled, somebody has to know what goes in it. The plan said Process Monitor;
two cheaper sources answered it better, and one of them changed a design decision.

## The question

The development machine has the whole of HLAE unpacked twice — about 200 MB of installer,
HLAE.exe, a .NET dependency, two other games' hooks. A release should carry the subset
`AfxHookSource2.dll` cannot run without, and nothing else, because everything else is
somebody's download and somebody's antivirus scan.

## Modules, read off a running game

Not Process Monitor: the loader has already answered this, and a running process will say
so. With a worn session up:

```powershell
(Get-Process cs2).Modules |
    Where-Object { $_.FileName -like '*cs2-vr-tools*' } |
    Select-Object -ExpandProperty FileName
```

Ten, all from `hlae-selfbuilt\x64\`:

| File | Why |
| --- | --- |
| `AfxHookSource2.dll` | ours |
| `OpenEXR-3_3.dll`, `OpenEXRCore-3_3.dll`, `Iex-3_3.dll`, `IlmThread-3_3.dll`, `Imath-3_1.dll` | statically imported by the hook for image output; loaded whether or not anything is recorded |
| `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` | the C++ runtime, shipped beside the DLL so no redistributable has to be installed |
| `openxr_loader.dll` | ours, Apache-2.0 |

Not loaded: `AfxCppCli.dll`, `NGettext.dll`, `AfxHookSource.dll`, `AfxHookGoldSrc.dll`,
HLAE.exe itself. HLAE.exe injects and then has no further part in a session — which is what
makes the plan's "the launcher injects it itself" a removal of a dependency rather than a
reimplementation of one.

## Data, read off the source

`AfxHook.dat` is Source 1 and GoldSrc; nothing in `AfxHookSource2/` mentions it. What the
hook does open, all under `resources\`:

| Path | When |
| --- | --- |
| `resources\shaders\` (230 KB, 12 files) | drawing and depth passes |
| `resources\AfxHookSource2\cs2` | added as a `GAME` search path at startup, unconditionally — it need not exist, but the call is made every launch |
| `resources\hexfont.tga` (132 KB) | `CampathDrawer` only |
| `resources\AfxHookSource2\snippets\` (57 KB) | `mirv_script_load` with a relative name |

`resources\AfxHookSource\` (1.7 MB) is Source 1's and can go. The zip's `resources\` is
about 400 KB.

## The thing that changed a decision

`GetHlaeFolder()` is not an environment variable and does not come from HLAE.exe. It is
computed in `AfxHookSource2/hlaeFolder.cpp` from **the hook DLL's own module path**: strip
the file name, then strip one more directory.

So the layout is not a matter of taste. The DLL must sit in a directory named anything,
inside the folder that holds `resources\`:

```
hook\
  x64\AfxHookSource2.dll   + the OpenEXR and runtime DLLs + openxr_loader.dll
  resources\shaders\...
  resources\AfxHookSource2\...
  resources\hexfont.tga
```

Put the DLL at `hook\AfxHookSource2.dll` instead and every resource path resolves one level
too high, silently — no error, just no shaders.

And it finds itself with `GetModuleHandleW(L"AfxHookSource2.dll")`, by that exact name. Give
the file any other name and the handle is null, the folder string is left empty, and every
`resources\...` path becomes relative to the process's working directory — which is CS2's,
not ours. Also silent. So the zip may not rename the DLL to something friendlier, an
installer may not version-stamp it, and a second copy kept side by side for comparison has
to live in a separate tree rather than beside the first under a different name.

This is also why the loader search added in *the hook stops needing this particular
machine* looks both beside the DLL and one directory up: beside it is where the zip puts
it, one up is where a hand-assembled HLAE tree has it.

## What was not done

No Process Monitor pass. The two sources above agree and are reproducible in a shell, but
they only see what a session actually exercised: a path opened by a code path nobody ran —
`mirv_script_load`, a campath being drawn — would not appear in the module list. Both are
in the table because they were found by reading, not by watching. If something is missing
from a release it will be one of that kind, and Process Monitor is still the way to catch
it.
