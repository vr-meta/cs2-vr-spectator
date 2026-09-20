# Reads the rectangles CS2 is actually using, from outside the process.
#
# Written for experiment 22, where the menu panel showed a frame the operator could not
# reconcile with where their ray went. Three rectangles are in play at the menu and the hook
# logs none of them: the client rect the pointer maps onto (MoveMouseToSheet), the window
# rect Windows actually granted, and the desktop display mode - which CS2 CHANGES under us
# when setting.fullscreen is 1, snapping an impossible resolution to the nearest legal one.
# That is how 2048x1536 appeared out of a request for 1000x1400.
#
# It has to be external. The hook cannot report the panel's source at a desk because that
# code path needs an XR session, and the whole point of these measurements is that they do
# not need a headset. So this reads what Windows knows and console.log supplies the rest.
#
#   .\probe-cs2-window.ps1 -Label 'fullscreen=1, asked 1000x1400'
#
# Prints and changes nothing. Safe while a session is running: no input is sent, no window
# is brought forward. grab-cs2-window.ps1 is the one that steals focus.

param(
    [int]$WaitSeconds = 60,          # how long to wait for the window to exist
    [int]$SettleSeconds = 6,         # CS2 resizes its swap chain more than once on the way up
    [string]$Label = 'run'
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Cs2WindowProbe {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int  GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
Add-Type -AssemblyName System.Windows.Forms

# Poll rather than assume: HLAE creates the process and CS2 takes several seconds to put a
# window up, so a probe run straight after the launch script finds a process with no handle.
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$proc = $null
while ((Get-Date) -lt $deadline) {
    $proc = Get-Process cs2 -ErrorAction SilentlyContinue
    if ($proc -and $proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $proc -or $proc.MainWindowHandle -eq [IntPtr]::Zero) {
    throw 'cs2 has no window yet. Did the launch actually reach Start-Process?'
}

# A single early sample reports a rectangle nobody ever saw: the mode change and the swap
# chain resize both land after the window first appears.
Start-Sleep -Seconds $SettleSeconds

$h = $proc.MainWindowHandle

$client = New-Object Cs2WindowProbe+RECT
[Cs2WindowProbe]::GetClientRect($h, [ref]$client) | Out-Null

$window = New-Object Cs2WindowProbe+RECT
[Cs2WindowProbe]::GetWindowRect($h, [ref]$window) | Out-Null

$origin = New-Object Cs2WindowProbe+POINT
[Cs2WindowProbe]::ClientToScreen($h, [ref]$origin) | Out-Null

$style   = [Cs2WindowProbe]::GetWindowLong($h, -16)   # GWL_STYLE
$exStyle = [Cs2WindowProbe]::GetWindowLong($h, -20)   # GWL_EXSTYLE
$screen  = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

"--- $Label ---"
"client      {0}x{1}" -f ($client.R - $client.L), ($client.B - $client.T)
"window      {0}x{1}  at {2},{3}" -f ($window.R - $window.L), ($window.B - $window.T), $window.L, $window.T
"clientorg   {0},{1}" -f $origin.X, $origin.Y
"display     {0}x{1}" -f $screen.Width, $screen.Height
"style       0x{0:X8}   exstyle 0x{1:X8}" -f $style, $exStyle
"WS_POPUP    {0}" -f (($style -band 0x80000000) -ne 0)
"WS_CAPTION  {0}" -f (($style -band 0x00C00000) -eq 0x00C00000)

# The engine's own idea of its size is the fourth quantity, and it is the one that disagrees.
# It is not readable from here - it is in console.log as "RenderPipelineCsgo] RT WxH" and
# "m_DisplayMode / m_nWidth". Read both, or the table in experiment 22 is only half filled.
""
"Now read the engine's side from <CS2>\game\csgo\console.log:"
"  RenderPipelineCsgo] RT <w>x<h>     what the engine renders and lays out for"
"  m_DisplayMode / m_nWidth           what the engine thinks the mode is"
"  | AfxHookSource2 (<date>) |        that a hook attached at all - NOT 'AFXVR', which"
"                                     upstream's hook never prints even when working"
