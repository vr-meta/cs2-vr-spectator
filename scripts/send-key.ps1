# Sends one function key to the CS2 window.
#
# CS2 has no remote console, so anything bound to a key has to be pressed. SendKeys
# does not reach games that read raw input; SendInput with the scancode flag does.
# Extracted from sweep-offset.ps1 so single presses do not need the whole sweep.
#
#   .\send-key.ps1 -Key F7

param(
    [Parameter(Mandatory = $true)][string]$Key,
    [int]$SettleMs = 600
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class SendKeyOne {
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
        inp[0].type = 1; inp[0].ki.wVk = vk; inp[0].ki.wScan = scan; inp[0].ki.dwFlags = 0x0008;
        inp[1].type = 1; inp[1].ki.wVk = vk; inp[1].ki.wScan = scan; inp[1].ki.dwFlags = 0x0008 | 0x0002;
        SendInput(2, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$vk = @{ F1 = 0x70; F2 = 0x71; F3 = 0x72; F4 = 0x73; F5 = 0x74
         F6 = 0x75; F7 = 0x76; F8 = 0x77; F9 = 0x78; F10 = 0x79 }[$Key.ToUpper()]

if (-not $vk) { throw "Unsupported key: $Key" }

$proc = Get-Process cs2 -ErrorAction SilentlyContinue
if (-not $proc) { throw 'cs2.exe is not running.' }

[SendKeyOne]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400
[SendKeyOne]::Tap([System.UInt16]$vk)
Start-Sleep -Milliseconds $SettleMs

Write-Host "Sent $Key to cs2 (pid $($proc.Id))" -ForegroundColor Green
