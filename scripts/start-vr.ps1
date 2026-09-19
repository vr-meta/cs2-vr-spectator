# The only supported way to start a session you are going to wear.
#
#   .\start-vr.ps1
#   .\start-vr.ps1 -Demo other.dem
#   .\start-vr.ps1 -NoStart        # launch, but leave starting the session to F9
#
# Everything a worn session needs is decided here rather than remembered at the prompt,
# because the day it was remembered wrong cost an hour: a desk measurement launched at
# 1264x1390 wrote that size into CS2's own video settings, the next launch inherited it,
# and the headset spent a session at a quarter of the pixels before anyone noticed.
#
# What it fixes in place:
#
#   - Meta's runtime, explicitly. The machine's registry points OpenXR at SteamVR, so
#     "no flag" means SteamVR, and on a Quest over Link SteamVR is a translation layer with
#     nothing to translate and a long history of stalling this project.
#   - A frame cap. An uncapped CS2 and a VR compositor fight over the GPU and both stall;
#     the symptom is "QueuePresentAndWait looped for N iterations without a present event"
#     and a game that stops responding.
#   - The full per-eye size, stated every time, so nothing can be inherited.
#   - The configs, copied out of the repository every time (deploy-cfg.ps1), into
#     cfg\cs2vr\ where nothing of the game's own is mixed in with them.
#   - AFXVR_AUTOSTART, so the session begins when the demo starts playing. Nothing is
#     timed, nothing is synthesised, and the window does not need focus.
#
# Desk measurements are the other script, launch-cs2-experiment.ps1, with an exp*.cfg.
# Do not use -ExecCfg vr for one: it binds F9, and F9 starts a headset session.

param(
    [string]$Demo    = 'pro_mirage.dem',
    [int]$Width      = 2528,     # per eye: submission copies the back buffer, so the
    [int]$Height     = 2780,     # window size IS the eye size
    [int]$FpsMax     = 90,
    [switch]$NoStart,            # skip the F9
    [switch]$KeepSteamVr         # start anyway with SteamVR running
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSCommandPath

# SteamVR and the Oculus runtime both want the headset. With Link, the second one to ask
# gets a compositor that never schedules it, which reads as the game freezing.
$steamVr = Get-Process -Name 'vrserver', 'vrmonitor', 'vrcompositor' -ErrorAction SilentlyContinue
if ($steamVr -and -not $KeepSteamVr) {
    Write-Host 'SteamVR is running.' -ForegroundColor Red
    Write-Host '  On a Quest over Link it has nothing to translate and it takes the headset.'
    Write-Host '  Close it and run this again, or pass -KeepSteamVr if you mean it.'
    exit 1
}

$oculus = Get-Process -Name 'OVRServer_x64' -ErrorAction SilentlyContinue
if (-not $oculus) {
    Write-Host 'The Oculus runtime is not running. Start Link on the headset first.' -ForegroundColor Yellow
}

if (Get-Process -Name cs2 -ErrorAction SilentlyContinue) {
    Write-Host 'CS2 is already running. Close it first - the hook DLL cannot be replaced while it is.' -ForegroundColor Red
    exit 1
}

# A fresh log, so what is read back afterwards belongs to this launch.
$log = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo\console.log'
if (Test-Path $log) { Remove-Item $log -Force }

Write-Host "per eye  : ${Width}x${Height}" -ForegroundColor Cyan
Write-Host "runtime  : Meta, for this launch only (XR_RUNTIME_JSON)" -ForegroundColor Cyan
Write-Host "frame cap: $FpsMax" -ForegroundColor Cyan

# Always, so a layout edited in the repository is the layout the headset gets. They used to
# be copied by hand, which is how a config in the game and a config in git drift apart.
& (Join-Path $here 'deploy-cfg.ps1')

& (Join-Path $here 'launch-cs2-experiment.ps1') `
    -SelfBuilt -VrReady -MetaRuntime `
    -ExecCfg vr -Width $Width -Height $Height -FpsMax $FpsMax -Demo $Demo `
    -AutoStart:(-not $NoStart)

if ($NoStart) {
    Write-Host 'Launched. Press F9 in the game when the demo is up.' -ForegroundColor Green
    exit 0
}

# Nothing to press and nothing to time: the hook starts the session itself the moment the
# demo tick moves. This only waits so the log below has something in it.
Write-Host 'Waiting for the session...' -NoNewline
for ($i = 0; $i -lt 120; $i++) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'AFXVR: autostart' -Quiet)) { break }
    Start-Sleep -Seconds 2
    Write-Host '.' -NoNewline
}
Write-Host ''
Start-Sleep -Seconds 3
Write-Host ''
Get-Content $log | Select-String -Pattern 'AFXVR' | Select-Object -Last 12 | ForEach-Object { $_.Line }
Write-Host ''
Write-Host 'If the last line is not "submitting frames to the headset", the session did not start.' -ForegroundColor Yellow
