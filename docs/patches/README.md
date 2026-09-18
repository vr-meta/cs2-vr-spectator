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

### Note on applying patches to this tree

Do not use `sed -i` on these files. The tree uses CRLF, and Git Bash `sed` rewrote every
line ending on the first attempt — a two-line change showed up as 262 insertions and 262
deletions. Use a method that preserves bytes, e.g. PowerShell:

```powershell
$raw = [IO.File]::ReadAllText($path)
[IO.File]::WriteAllText($path, $raw.Replace($old, $new))
```
