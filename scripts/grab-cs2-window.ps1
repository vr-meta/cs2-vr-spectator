# Captures the CS2 window to a PNG.
#
# Used instead of an in-game screenshot command so the capture is identical across
# the offset sweep: same window, same crop, no engine-side processing in between.
#
#   .\grab-cs2-window.ps1 -Name offset_0
#
# Writes to docs/experiments/screenshots/<name>.png by default.

param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$OutDir = 'D:\Dev\cs2-vr-spectator\docs\experiments\screenshots'
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win32Grab {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@

$proc = Get-Process cs2 -ErrorAction SilentlyContinue
if (-not $proc) { throw 'cs2.exe is not running.' }

$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw 'CS2 has no main window handle.' }

# Bring it forward, or the capture grabs whatever is on top.
[Win32Grab]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400

$rect = New-Object Win32Grab+RECT
[Win32Grab]::GetClientRect($hwnd, [ref]$rect) | Out-Null

$origin = New-Object Win32Grab+POINT
[Win32Grab]::ClientToScreen($hwnd, [ref]$origin) | Out-Null

$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "Bad client rect: ${w}x${h}" }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$path = Join-Path $OutDir "$Name.png"

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bmp.Size)
$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose()
$bmp.Dispose()

Write-Host "saved: $path  (${w}x${h})" -ForegroundColor Green
