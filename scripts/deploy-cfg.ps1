# Copies this project's configs into CS2, under a directory of their own.
#
#   .\deploy-cfg.ps1              # copy, and remove stale loose copies
#   .\deploy-cfg.ps1 -KeepLoose   # copy only
#
# They go to <CS2>\game\csgo\cfg\cs2vr\ and nowhere else. Not tidiness:
#
#  - Removing this project becomes deleting one folder. Nothing of ours is left mixed in
#    with Valve's own configs, which is a promise docs/07-release-plan.md makes on the
#    launcher's behalf and which the launcher will keep by calling the same rule.
#  - A stale copy cannot be reached by habit. These files used to sit loose in cfg\, where
#    `exec vr_keys` meant whichever version was last copied by hand. Layouts diverging from
#    the repository while somebody is wearing the headset is a mistake this project has
#    already made once, in the other direction (see check-cfg.ps1's reserved keys).
#
# Every exec inside the files names cs2vr/ explicitly, because CS2 resolves exec against
# cfg\ and not against the file doing the exec'ing.

param(
    [string]$Cs2 = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive',
    [switch]$KeepLoose
)

$ErrorActionPreference = 'Stop'

$source = Join-Path (Split-Path -Parent $PSCommandPath) 'cs2'
$cfgRoot = Join-Path $Cs2 'game\csgo\cfg'
$target  = Join-Path $cfgRoot 'cs2vr'

if (-not (Test-Path $cfgRoot)) { throw "Not a CS2 installation: $cfgRoot does not exist." }

$files = Get-ChildItem -Path $source -Filter *.cfg | Sort-Object Name
if (0 -eq $files.Count) { throw "No configs in $source." }

New-Item -ItemType Directory -Force $target | Out-Null
foreach ($f in $files) { Copy-Item $f.FullName (Join-Path $target $f.Name) -Force }

Write-Host "$($files.Count) configs -> $target" -ForegroundColor Green

# The loose copies from before this directory existed. Only names we ship, so nothing of
# the game's or the player's own is ever considered.
if (-not $KeepLoose) {
    $removed = 0
    foreach ($f in $files) {
        $stale = Join-Path $cfgRoot $f.Name
        if (Test-Path $stale) {
            Write-Host "  removing stale $stale" -ForegroundColor Yellow
            Remove-Item $stale -Force
            $removed++
        }
    }
    if ($removed -gt 0) {
        Write-Host "$removed loose copy(ies) removed from cfg\. Pass -KeepLoose to leave them." -ForegroundColor Yellow
    }
}
