# Types into the console of a game that has none.
#
#   .\send-command.ps1 "mirv_vr_panel spread 1.2"
#   .\send-command.ps1 "mirv_vr_views" "mirv_vr_fov"       # several, in order
#   "mirv_vr_reset" | .\send-command.ps1
#
# The window in a worn launch is 2528x2780 clamped onto a 2560x1600 display, and Panorama
# lays the console out for the full 2780 rows, so its input line is below the bottom of the
# screen. Until now the only ways in were a key bind and a config re-read with PgDn/PgUp,
# which is why every adjustable number had to be given a key first.
#
# The hook opens \\.\pipe\cs2vr for this user (mirv_vr_pipe 0 closes it). A line written
# here is queued and dispatched on the engine thread, in order, with everything else.
#
# The reply is in the game's console.log, not here: this is a console with no echo back.
# Pass -Tail to print what the log gained afterwards.

param(
    [Parameter(ValueFromRemainingArguments = $true, ValueFromPipeline = $true)]
    [string[]]$Command,

    [switch]$Tail,
    [string]$Log = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo\console.log'
)

$ErrorActionPreference = 'Stop'

if (-not $Command -or 0 -eq $Command.Count) {
    throw 'Nothing to send. Pass one or more console lines.'
}

$before = 0
if ($Tail -and (Test-Path $Log)) { $before = (Get-Item $Log).Length }

$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'cs2vr', [System.IO.Pipes.PipeDirection]::Out)
try {
    # Short: either the hook is in this game or it is not. A long wait here just delays
    # finding out that the session was never launched with the pipe open.
    $pipe.Connect(2000)
} catch {
    Write-Host 'No pipe at \\.\pipe\cs2vr.' -ForegroundColor Red
    Write-Host '  The game is not running, or was launched with AFXVR_PIPE=0, or mirv_vr_pipe 0 closed it.'
    exit 1
}

$writer = New-Object System.IO.StreamWriter($pipe)
$writer.AutoFlush = $true
foreach ($line in $Command) {
    $writer.WriteLine($line)
    Write-Host "-> $line" -ForegroundColor Cyan
}
$writer.Dispose()
$pipe.Dispose()

if ($Tail) {
    Start-Sleep -Milliseconds 500
    if (Test-Path $Log) {
        $stream = [System.IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
        $stream.Seek($before, 'Begin') | Out-Null
        $reader = New-Object System.IO.StreamReader($stream)
        $reader.ReadToEnd().TrimEnd()
        $reader.Dispose()
    }
}
