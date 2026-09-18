# Drives the experiment 00 offset sweep: presses the bound key, captures the window.
#
# CS2 has no remote console (no -netconport in engine2.dll), so the sweep is driven by
# synthesised key input. SendKeys does not reach games reading raw input, so this uses
# SendInput, which does.
#
# Requires, in the game console beforehand:
#   mirv_cvar_unhide_all
#   exec exp00_probe      (binds F5=0, F6=-1.25, F7=+1.25, F8=+10)
#   exec exp00_play       (plays the demo; F4 pauses)
# ...with the demo PAUSED on a frame showing near and far geometry.
#
#   .\sweep-offset.ps1

param(
    [string]$OutDir = 'D:\Dev\cs2-vr-spectator\docs\experiments\screenshots',
    [int]$SettleMs  = 900
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Keys {
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Explicit, Size = 40)] public struct INPUT {
        [FieldOffset(0)]  public uint type;
        [FieldOffset(8)]  public KEYBDINPUT ki;
    }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern ushort MapVirtualKey(uint uCode, uint uMapType);

    public static void Tap(ushort vk) {
        ushort scan = MapVirtualKey(vk, 0);
        INPUT[] inp = new INPUT[2];
        inp[0].type = 1; inp[0].ki.wVk = vk; inp[0].ki.wScan = scan; inp[0].ki.dwFlags = 0x0008 | 0x0000;
        inp[1].type = 1; inp[1].ki.wVk = vk; inp[1].ki.wScan = scan; inp[1].ki.dwFlags = 0x0008 | 0x0002;
        // 0x0008 = SCANCODE, 0x0002 = KEYUP. Scancode flag matters: raw-input games
        // commonly ignore virtual-key-only synthetic events.
        SendInput(2, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$proc = Get-Process cs2 -ErrorAction SilentlyContinue
if (-not $proc) { throw 'cs2.exe is not running.' }
[Keys]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 600

# F5..F8 = 0x74..0x77
$steps = @(
    @{ Key = 0x74; Name = 'offset_0';      Desc = 'baseline, offset 0' }
    @{ Key = 0x75; Name = 'offset_left';   Desc = 'offset -1.25 (left eye)' }
    @{ Key = 0x76; Name = 'offset_right';  Desc = 'offset +1.25 (right eye)' }
    @{ Key = 0x77; Name = 'offset_exag';   Desc = 'offset +10 (exaggerated)' }
)

foreach ($s in $steps) {
    Write-Host "-> $($s.Desc)" -ForegroundColor Cyan
    [Keys]::Tap([ushort]$s.Key)
    Start-Sleep -Milliseconds $SettleMs
    & "$PSScriptRoot\grab-cs2-window.ps1" -Name $s.Name -OutDir $OutDir
}

Write-Host ''
Write-Host "Done. Four captures in $OutDir" -ForegroundColor Green
