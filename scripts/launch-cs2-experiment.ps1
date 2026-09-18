# Launches CS2 through HLAE so AfxHookSource2 is injected.
#
# HLAE is what makes the experiment possible: mirv_cvar_unhide_all exposes the
# developmentonly convars under test, and the multi-pass render machinery lives here
# too. It injects into the running game and touches no game files, so Steam has
# nothing to revert - unlike the gameinfo.gi edit that cvar-unhide-s2 needed, which
# Steam deleted and which crashed the game (see docs/experiments/00).
#
# -insecure is mandatory: it is this project's operating boundary. HLAE is technically
# a hack and joining VAC-protected servers with it risks a ban. Local demos only.
#
# Steam must be running.

param(
    [switch]$Vulkan,          # try the Vulkan backend instead of D3D11
    [string]$Demo,            # optional .dem to play on startup
    [switch]$Fullscreen
)

$ErrorActionPreference = 'Stop'

$cs2  = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe'
$hlae = 'D:\Dev\cs2-vr-tools\hlae\HLAE.exe'
$hook = 'D:\Dev\cs2-vr-tools\hlae\x64\AfxHookSource2.dll'

foreach ($p in @($cs2, $hlae, $hook)) {
    if (-not (Test-Path $p)) { throw "Not found: $p" }
}

if (-not (Get-Process steam -ErrorAction SilentlyContinue)) {
    throw 'Steam is not running. Start it first.'
}

# Game arguments, passed through HLAE via -cmdLine.
$gameArgs = @(
    '-insecure'
    '-novid'
    '-condebug'                     # CS2 has no con_logfile; this writes game/csgo/console.log
    # Note: CS2 has no -netconport. The string is absent from engine2.dll, so there is
    # no remote console to drive the experiment through; key input is sent instead
    # (scripts/sweep-offset.ps1).
    '-allow_third_party_software'
    '+con_enable', '1'
)

if (-not $Fullscreen) { $gameArgs += @('-windowed', '-w', '1280', '-h', '720') }
if ($Vulkan)          { $gameArgs += '-vulkan' }
if ($Demo) {
    # A bare name is resolved by the engine relative to game/csgo, so only check
    # paths that actually look like paths.
    if ($Demo -match '[\\/]' -and -not (Test-Path $Demo)) { throw "Demo not found: $Demo" }
    $gameArgs += @('+playdemo', $Demo)
}

$gameCmdLine = $gameArgs -join ' '

$hlaeArgs = @(
    '-customLoader'
    '-noGui'
    '-autoStart'
    '-programPath', "`"$cs2`""
    '-hookDllPath', "`"$hook`""
    '-cmdLine',     "`"$gameCmdLine`""
)

Write-Host 'Launching CS2 through HLAE' -ForegroundColor Cyan
Write-Host "  game args: $gameCmdLine"
Write-Host ''
Write-Host 'In the console, FIRST of all:' -ForegroundColor Green
Write-Host '  mirv_cvar_unhide_all'
Write-Host '  mirv_cvar_unlock_sv_cheats'
Write-Host ''
Write-Host 'If mirv_cvar_unhide_all is not recognised, HLAE did not attach and every' -ForegroundColor Yellow
Write-Host 'convar result afterwards is meaningless. Stop and fix that first.'
Write-Host ''
Write-Host 'Then:'
Write-Host '  exec exp00_dump      -> version + full cvarlist to game/csgo/exp00_cvarlist.log'
Write-Host '  exec exp00_probe     -> checks the four convars, binds the F5-F9 sweep'

Start-Process -FilePath $hlae -ArgumentList $hlaeArgs
