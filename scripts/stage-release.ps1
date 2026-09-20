# Assembles a release: the folder a stranger unpacks, the zip of it, and its SHA-256.
#
#   .\stage-release.ps1 -HookTree D:\Dev\cs2-vr-tools\hlae-selfbuilt `
#                       -Launcher build\launcher\Release\cs2vr.exe `
#                       -Version 0.1.0-alpha.1
#
# Used by .github/workflows/release.yml and by hand, so that what CI publishes and what is
# tried at a desk are made by the same lines. This is developer tooling; nothing in the
# release itself needs PowerShell.
#
# THE LAYOUT IS NOT A MATTER OF TASTE. The hook finds its resources from its own path - one
# directory up from the DLL - and finds itself by the exact name AfxHookSource2.dll. Put it
# anywhere else, or call it anything else, and the shaders are silently not found
# (docs/experiments/21-what-the-zip-has-to-contain.md). So:
#
#   cs2vr.exe
#   hook\x64\AfxHookSource2.dll  + the DLLs it imports + openxr_loader.dll
#   hook\resources\shaders\ ...
#   cfg\vr*.cfg
#   README.md  LICENSE  THIRD-PARTY.md  hook\LICENSES\

param(
    # A tree shaped like HLAE's: x64\ with the built hook and its DLLs, resources\ beside it.
    [Parameter(Mandatory = $true)][string]$HookTree,
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$Version,
    # The CS2 ClientVersion the hook's offsets were measured on. It goes into the file name
    # because it is the first thing anyone needs to know when a release stops working.
    # Left empty it is read out of the hook's own version header, which is where the number
    # lives: a second copy here would one day disagree with what the hook prints.
    [string]$Cs2Build,
    # openxr_loader.dll, if it is not already in the hook tree's x64\.
    [string]$OpenXrLoader,
    # Where the folder and the zip go. Default: build\release in the repository, which git
    # ignores. (Worked out below, not here: Windows PowerShell 5.1 does not have the
    # script's own path yet while it is still binding parameter defaults.)
    [string]$Out
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $PSCommandPath) '..')).Path
if (-not $Out) { $Out = Join-Path $repo 'build\release' }

function Need($path, $what) {
    if (-not (Test-Path $path)) { throw "$what not found: $path" }
    return (Resolve-Path $path).Path
}

$hookX64   = Need (Join-Path $HookTree 'x64') 'The hook tree''s x64 folder'
$resources = Need (Join-Path $HookTree 'resources') 'The hook tree''s resources folder'
$launcherExe = Need $Launcher 'cs2vr.exe'

if (-not $Cs2Build) {
    $header = Need (Join-Path $repo 'src\AfxHookSource2\MirvVrVersion.h') 'MirvVrVersion.h'
    $found = Select-String -Path $header -Pattern '#define\s+AFXVR_TESTED_CLIENT_VERSION\s+"(\d+)"' | Select-Object -First 1
    if (-not $found) { throw "AFXVR_TESTED_CLIENT_VERSION not found in $header" }
    $Cs2Build = $found.Matches[0].Groups[1].Value
}

$name  = "cs2-vr-spectator-$Version-cs2-$Cs2Build"
$stage = Join-Path $Out $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'hook\x64'), (Join-Path $stage 'hook\resources'), (Join-Path $stage 'cfg') | Out-Null

# --- the hook and what it imports -------------------------------------------------------
# Read off a running game's module list, not guessed: these are loaded whether or not
# anything is recorded. The C++ runtime ships beside the DLL so nothing has to be installed.
$required = @(
    'AfxHookSource2.dll',
    'OpenEXR-3_3.dll', 'OpenEXRCore-3_3.dll', 'Iex-3_3.dll', 'IlmThread-3_3.dll', 'Imath-3_1.dll'
)
foreach ($f in $required) {
    Copy-Item (Need (Join-Path $hookX64 $f) "A DLL the hook imports ($f)") (Join-Path $stage 'hook\x64') -Force
}
$runtime = Get-ChildItem $hookX64 -File | Where-Object { $_.Name -match '^(msvcp140|vcruntime140|concrt140).*\.dll$' }
if (-not $runtime) { throw "No C++ runtime DLLs (msvcp140*.dll, vcruntime140*.dll) in $hookX64." }
$runtime | ForEach-Object { Copy-Item $_.FullName (Join-Path $stage 'hook\x64') -Force }

$loader = if ($OpenXrLoader) { $OpenXrLoader } else { Join-Path $hookX64 'openxr_loader.dll' }
Copy-Item (Need $loader 'openxr_loader.dll (pass -OpenXrLoader)') (Join-Path $stage 'hook\x64\openxr_loader.dll') -Force

# --- resources: what AfxHookSource2 opens, and not Source 1's 1.7 MB ---------------------
Copy-Item (Need (Join-Path $resources 'shaders') 'resources\shaders') (Join-Path $stage 'hook\resources') -Recurse -Force
foreach ($optional in @('AfxHookSource2', 'snippets', 'hexfont.tga')) {
    $p = Join-Path $resources $optional
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage 'hook\resources') -Recurse -Force }
}
# The hook adds this as a search path on every launch; it need not contain anything.
New-Item -ItemType Directory -Force (Join-Path $stage 'hook\resources\AfxHookSource2\cs2') | Out-Null

# advancedfx's licence travels with advancedfx's code: the hook DLL is mostly theirs and MIT
# asks for the notice to go with it. CI stages it as LICENSE-advancedfx.txt; a hand-made
# HLAE tree carries LICENSES\ instead. Whichever is there is copied, and one must be.
$carried = 0
foreach ($licences in @('LICENSE-advancedfx.txt', 'LICENSES', 'LICENSE')) {
    $p = Join-Path $HookTree $licences
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage 'hook') -Recurse -Force; $carried++ }
}
if (0 -eq $carried) { throw "No advancedfx licence in $HookTree (LICENSE-advancedfx.txt or LICENSES\). A release without it is not redistributable." }

# --- ours -------------------------------------------------------------------------------
Copy-Item $launcherExe (Join-Path $stage 'cs2vr.exe') -Force

# Only the configs a session uses. The exp*.cfg files are records of answered questions
# and bind keys that start measurements; they are not for somebody wearing a headset.
$cfgs = Get-ChildItem (Join-Path $repo 'scripts\cs2') -Filter 'vr*.cfg'
if (-not $cfgs) { throw 'No vr*.cfg in scripts\cs2.' }
$cfgs | ForEach-Object { Copy-Item $_.FullName (Join-Path $stage 'cfg') -Force }

# NOTICE is not optional decoration: Apache 2.0 section 4(d) requires it to travel with
# every distribution, and a zip is a distribution.
foreach ($doc in @('README.md', 'LICENSE', 'NOTICE', 'THIRD-PARTY.md')) {
    $p = Join-Path $repo $doc
    if (Test-Path $p) {
        Copy-Item $p $stage -Force
    } elseif ($doc -in @('LICENSE', 'NOTICE')) {
        # Not a warning. Publishing a zip without these is distributing the work without
        # the terms it is distributed under, which is the one mistake here that cannot be
        # fixed by uploading a better zip afterwards.
        throw "$doc is missing from the repository. A release cannot be made without it."
    } else {
        Write-Warning "$doc is missing from the repository; the release will not carry it."
    }
}

Set-Content -Path (Join-Path $stage 'VERSION.txt') -Encoding utf8 -Value @(
    "cs2-vr-spectator $Version",
    "made for CS2 build (ClientVersion) $Cs2Build",
    "https://github.com/vr-meta/cs2-vr-spectator"
)

# --- checks that fail the release rather than the user ----------------------------------
$dll = Join-Path $stage 'hook\x64\AfxHookSource2.dll'
if (-not (Test-Path (Join-Path (Split-Path -Parent (Split-Path -Parent $dll)) 'resources\shaders'))) {
    throw 'Layout is wrong: resources\shaders is not one directory up from the hook DLL.'
}
$machinePaths = Get-ChildItem $stage -Recurse -File -Include *.cfg, *.md, *.txt |
    Select-String -Pattern 'D:\\Dev\\|D:\\SteamLibrary|C:\\Users\\' -SimpleMatch:$false
if ($machinePaths) {
    $machinePaths | ForEach-Object { Write-Warning "a path from the development machine: $($_.Path):$($_.LineNumber)" }
}

# --- zip and checksum -------------------------------------------------------------------
$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Encoding ascii -Value "$hash  $name.zip"

# For whoever writes the release notes, so they quote the same number the file name does.
Set-Content -Path (Join-Path $Out 'cs2-build.txt') -Encoding ascii -Value $Cs2Build

$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "$name.zip  $size MB" -ForegroundColor Green
Write-Host "sha256  $hash"
Write-Host "staged  $stage"
