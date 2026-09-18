<#
.SYNOPSIS
    Drags the mouse across the CS2 window, for testing the demo timeline slider.

.DESCRIPTION
    The demo timeline is a Panorama control and the only way to exercise it is to
    actually drag it. Dragging it crashed the game with an access violation on the first
    day of this project, and nothing since then established whether that was the hook's
    fault or the game's -- which matters, because the VR menu (issue #2) has to decide
    between reproducing that slider and offering fixed jumps instead.

    Coordinates are in window client pixels, origin top left.

.EXAMPLE
    # Drag the timeline scrubber of a 1280x720 window from a tenth in to two thirds in.
    scripts\drag-in-cs2.ps1 -FromX 128 -FromY 685 -ToX 850 -ToY 685
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int] $FromX,
    [Parameter(Mandatory = $true)][int] $FromY,
    [Parameter(Mandatory = $true)][int] $ToX,
    [Parameter(Mandatory = $true)][int] $ToY,
    [int] $Steps = 25,
    [int] $StepDelayMs = 20,
    [string] $ProcessName = 'cs2'
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class Drag {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);

    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP   = 0x0004;
}
'@

$proc = Get-Process $ProcessName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No $ProcessName process with a window." }
$hwnd = $proc.MainWindowHandle

# SetForegroundWindow refuses unless the calling thread owns the foreground window or is
# attached to the one that does. Without this the drag lands wherever the mouse happens to
# be, which looks exactly like the game ignoring it. Same trick as send-key.ps1.
$fg = [Drag]::GetForegroundWindow()
$fgThread = [Drag]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$me = [Drag]::GetCurrentThreadId()
[void][Drag]::AttachThreadInput($me, $fgThread, $true)
[void][Drag]::SetForegroundWindow($hwnd)
[void][Drag]::AttachThreadInput($me, $fgThread, $false)
Start-Sleep -Milliseconds 300

function To-Screen([int] $x, [int] $y) {
    $p = New-Object Drag+POINT
    $p.X = $x; $p.Y = $y
    [void][Drag]::ClientToScreen($hwnd, [ref] $p)
    return $p
}

$start = To-Screen $FromX $FromY
[void][Drag]::SetCursorPos($start.X, $start.Y)
Start-Sleep -Milliseconds 150
[Drag]::mouse_event([Drag]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)

for ($i = 1; $i -le $Steps; $i++) {
    $x = $FromX + [int](($ToX - $FromX) * $i / $Steps)
    $y = $FromY + [int](($ToY - $FromY) * $i / $Steps)
    $p = To-Screen $x $y
    [void][Drag]::SetCursorPos($p.X, $p.Y)
    Start-Sleep -Milliseconds $StepDelayMs
}

[Drag]::mouse_event([Drag]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
Write-Host "dragged ($FromX,$FromY) -> ($ToX,$ToY) in $ProcessName (pid $($proc.Id))"
