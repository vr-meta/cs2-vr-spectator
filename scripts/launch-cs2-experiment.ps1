# Launches CS2 for experiment work.
#
# -insecure is mandatory: the cvar-unhide plugin will not load without it, and it
# keeps this build away from VAC-protected servers. Do not remove it.
#
# This launches the game directly rather than through Steam so the arguments are
# explicit and repeatable. Steam must already be running.

param(
    [switch]$Vulkan,          # try the Vulkan backend instead of D3D11
    [string]$Demo             # optional .dem to play on startup
)

$ErrorActionPreference = 'Stop'

$cs2 = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe'
if (-not (Test-Path $cs2)) { throw "cs2.exe not found at $cs2" }

if (-not (Get-Process steam -ErrorAction SilentlyContinue)) {
    throw 'Steam is not running. Start it first.'
}

$argsList = @(
    '-insecure'                     # required by the plugin; keeps us off VAC servers
    '-novid'                        # skip the intro
    '-allow_third_party_software'
    '-windowed'                     # keep the desktop usable while probing
    '-w', '1280', '-h', '720'
    '+con_enable', '1'              # developer console
    '+sv_cheats', '1'
)

if ($Vulkan) { $argsList += '-vulkan' }

if ($Demo) {
    if (-not (Test-Path $Demo)) { throw "Demo not found: $Demo" }
    $argsList += '+playdemo'
    $argsList += $Demo
}

Write-Host 'Launching CS2 with:' -ForegroundColor Cyan
Write-Host "  $($argsList -join ' ')"
Write-Host ''
Write-Host 'Once in game, open the console (~) and run:' -ForegroundColor Green
Write-Host '  exec exp00_dump      -> captures version + full cvarlist to game/csgo/exp00_cvarlist.log'
Write-Host '  exec exp00_probe     -> checks the four convars, binds the F5-F9 sweep'
Write-Host ''
Write-Host 'First thing to verify: the log must contain developmentonly convars.' -ForegroundColor Yellow
Write-Host 'If it does not, the plugin did not load and every convar result is meaningless.'

Start-Process -FilePath $cs2 -ArgumentList $argsList
