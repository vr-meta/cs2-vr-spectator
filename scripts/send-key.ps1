# Sends one function key to the CS2 window.
#
# CS2 has no remote console, so anything bound to a key has to be pressed. SendKeys
# does not reach games that read raw input; SendInput with the scancode flag does.
#
#   .\send-key.ps1 -Key F7
#
# Focus is the catch. A plain SetForegroundWindow is refused by Windows whenever the
# calling process is not itself in the foreground, and it fails *silently* - the key is
# then sent to whatever window actually has focus, and the experiment looks like the
# bind does nothing. So: attach to the foreground thread's input queue first, which
# lifts the restriction, and verify afterwards that the game really is in front.

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
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern ushort MapVirtualKey(uint uCode, uint uMapType);

    // Windows refuses a foreground steal unless the caller shares the foreground
    // thread's input queue, or unless input is "active". Two workarounds, tried in
    // order, because neither is reliable on its own.
    public static bool Focus(IntPtr target) {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (GetForegroundWindow() == target) return true;

            // 1. Share the foreground thread's input queue.
            uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
            uint myThread = GetCurrentThreadId();
            AttachThreadInput(myThread, fgThread, true);
            ShowWindow(target, 9 /* SW_RESTORE */);
            BringWindowToTop(target);
            SetForegroundWindow(target);
            AttachThreadInput(myThread, fgThread, false);
            System.Threading.Thread.Sleep(200);
            if (GetForegroundWindow() == target) return true;

            // 2. A synthetic ALT tap makes Windows treat the caller as active, which
            //    lifts the same restriction. Ugly, documented, and it works.
            Tap(0x12 /* VK_MENU */);
            SetForegroundWindow(target);
            System.Threading.Thread.Sleep(200);
            if (GetForegroundWindow() == target) return true;

            System.Threading.Thread.Sleep(300);
        }
        return false;
    }

    public static void Tap(ushort vk) {
        ushort scan = MapVirtualKey(vk, 0);
        // Home, End, Insert and Delete live on the extended part of the keyboard and are
        // not seen without this flag.
        uint ext = (vk == 0x24 || vk == 0x23 || vk == 0x2D || vk == 0x2E) ? 0x0001u : 0u;
        INPUT[] inp = new INPUT[2];
        inp[0].type = 1; inp[0].ki.wVk = vk; inp[0].ki.wScan = scan; inp[0].ki.dwFlags = 0x0008 | ext;
        inp[1].type = 1; inp[1].ki.wVk = vk; inp[1].ki.wScan = scan; inp[1].ki.dwFlags = 0x0008 | 0x0002 | ext;
        SendInput(2, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$vk = @{ F1 = 0x70; F2 = 0x71; F3 = 0x72; F4 = 0x73; F5 = 0x74
         F6 = 0x75; F7 = 0x76; F8 = 0x77; F9 = 0x78; F10 = 0x79
         # VK_OEM_3 is the key left of "1" - the console toggle. Its scancode is the
         # same whatever the keyboard layout, which a Russian layout's "e" is not.
         TILDE = 0xC0; CONSOLE = 0xC0
         HOME = 0x24; END = 0x23; INS = 0x2D; DEL = 0x2E }[$Key.ToUpper()]

if (-not $vk) { throw "Unsupported key: $Key" }

$proc = Get-Process cs2 -ErrorAction SilentlyContinue
if (-not $proc) { throw 'cs2.exe is not running.' }

if (-not [SendKeyOne]::Focus($proc.MainWindowHandle)) {
    throw "Could not bring the CS2 window to the foreground; the key would have gone somewhere else. Click the game window once and retry."
}

[SendKeyOne]::Tap([System.UInt16]$vk)
Start-Sleep -Milliseconds $SettleMs

Write-Host "Sent $Key to cs2 (pid $($proc.Id))" -ForegroundColor Green
