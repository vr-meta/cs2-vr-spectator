# Patches against advancedfx

Changes made to the HLAE source tree at `D:\Dev\cs2-vr-tools\advancedfx`. Kept here so
they survive a re-clone and can be reapplied or offered upstream.

advancedfx is MIT licensed, so modification and reuse are permitted with attribution.

## 001 — vswhere needs `-products *` to see Build Tools

**Problem.** `cmake --preset x64-release` failed during configure:

```
CMake Error at CMakeLists.txt:36 (string):
  string sub-command REPLACE requires at least four arguments.
CMake Error at CMakeLists.txt:43 (string):
  string sub-command REPLACE requires at least four arguments.
```

The `string(REPLACE ...)` calls operate on `VS_INSTALLPATH` and `VS_MSBUILD`, which were
empty because the `vswhere` queries above them returned nothing.

**Cause.** `vswhere` only reports full Visual Studio editions (Community, Professional,
Enterprise) unless `-products *` is passed. This machine has **Build Tools**, which is
sufficient to compile the C++ hook but invisible to the default query.

Confirmed directly:

| query | result |
| --- | --- |
| `vswhere -latest -version [17.0,18.0) -property installationPath` | *(empty)* |
| `vswhere -latest -products * -version [17.0,18.0) -property installationPath` | `...\2022\BuildTools` |
| same with `-requires Microsoft.Component.MSBuild -find MSBuild\**\**\Bin\MSBuild.exe` | found |

Note the third row: the MSBuild component was already installed. The initial diagnosis —
that a component was missing and needed a UAC-prompted install — was wrong. Nothing was
missing except a flag.

**Fix.** Add `-products *` to both `vswhere` invocations in `CMakeLists.txt`:

```cmake
COMMAND "...vswhere.exe" "-latest" "-products" "*" "-version" "[17.0,18.0)" ...
```

Two lines changed. Configure then completes.

**Worth upstreaming?** Probably. It makes the project buildable with Build Tools instead
of requiring a full Visual Studio install, and `-products *` is harmless for people who
do have Community — it widens the search rather than narrowing it.

## Build environment notes (not patches, but required)

Two further obstacles hit after the vswhere fix. Neither needs a source change, but both
stop the build cold and neither error says what is actually wrong.

### .NET Framework 4.6.2 Targeting Pack is required, even for the x64 hook

```
error MSB3644: The reference assemblies for .NETFramework,Version=v4.6.2 were not found.
```

`AfxHookSource2` depends on `ShaderBuilder`, which is a **C# project** that compiles the
hook's shaders through SharpDX. So the .NET dependency is not GUI-only, as
`docs/04-plan.md` first assumed.

Installed with:

```powershell
vs_installer.exe modify --installPath "...\2022\BuildTools" `
  --add Microsoft.Net.Component.4.6.2.TargetingPack --quiet --norestart
```

### ShaderBuilder.exe must be on PATH

```
'ShaderBuilder.exe' is not recognized as an internal or external command
...exited with code 9009
```

`AfxHookSource2/CMakeLists.txt` invokes `ShaderBuilder.exe` by bare name, with no path,
for each shader. It builds to `build/x64-release/ShaderBuilder/`, which is not on PATH.
Exit code 9009 on Windows means "command not found" and is easy to misread as a shader
compilation failure — the shaders are fine, the tool is simply not found.

Worked around by prepending that directory to `PATH` for the build rather than patching:

```powershell
$env:Path = "D:\Dev\cs2-vr-tools\advancedfx\build\x64-release\ShaderBuilder;$env:Path"
cmake --build --preset x64-release --target AfxHookSource2
```

The official `cmake/MultiBuild.cmake` route probably sets this up itself; building a
single target directly does not.

## Working build recipe

```powershell
$env:Path = "$env:USERPROFILE\.cargo\bin;<cmake>\bin;" +
            "D:\Dev\cs2-vr-tools\advancedfx\build\x64-release\ShaderBuilder;$env:Path"
cd D:\Dev\cs2-vr-tools\advancedfx
cmake --preset x64-release
cmake --build --preset x64-release --target AfxHookSource2
```

Output: `build/x64-release/AfxHookSource2/Release/AfxHookSource2.dll` (5.26 MB).

**Verified 2026-09-18:** the self-built DLL loads into CS2 and reproduces experiment 02 —
two streams, `eyeR` with `worldAction noDraw`, 80.4% of pixels differing from `eyeL`
(release build gave 74.1% on a different frame). Behaviour matches the shipped binary.

Keep the released HLAE installed alongside at `D:\Dev\cs2-vr-tools\hlae`; the self-built
copy lives at `D:\Dev\cs2-vr-tools\hlae-selfbuilt`. Swapping one file answers "is this my
build or my change?".

### Note on applying patches to this tree

Do not use `sed -i` on these files. The tree uses CRLF, and Git Bash `sed` rewrote every
line ending on the first attempt — a two-line change showed up as 262 insertions and 262
deletions. Use a method that preserves bytes, e.g. PowerShell:

```powershell
$raw = [IO.File]::ReadAllText($path)
[IO.File]::WriteAllText($path, $raw.Replace($old, $new))
```
