# Phase C analysis of one HLAE take.
#
# Reports two things that answer different questions:
#
#   between eyes  - eyeL vs eyeR of the SAME frame. This is the stereo difference.
#   within eye    - frame N vs frame N+1 of ONE stream. This is how much the scene
#                   moves between frames, which says whether the demo was playing.
#
# The decisive phase C measurement is "between eyes" with separation 0 on a PLAYING
# demo: it must stay at the noise floor. Anything else means something advances between
# render passes, and the two eyes would see different moments of the same smoke.
#
#   .\analyse-take.ps1 -Take exp05_stereo\take0002 -MaxFrames 5
#   .\analyse-take.ps1 -Take exp05_stereo\take0003 -AllFrames -Stride 20 -WorstOnly
#
# The pixel loop is C# rather than PowerShell: at 921600 pixels a PowerShell loop takes
# about ten seconds per pair, which makes scanning a whole take impractical.

param(
    [Parameter(Mandatory = $true)][string]$Take,
    [string]$Root = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64',
    [string]$StreamA = 'eyeL',
    [string]$StreamB = 'eyeR',
    [int]$MaxFrames = 4,
    [switch]$AllFrames,              # every frame in the take, subject to -Stride
    [int]$Stride = 1,          # look at every Nth frame
    [switch]$WorstOnly         # print only the worst frame, for scans
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.IO;
public class TgaDiff {
    // 18-byte header, then BGR. Returns {percent differing, mean abs, max abs}.
    public static double[] Compare(string pathA, string pathB) {
        byte[] a = File.ReadAllBytes(pathA);
        byte[] b = File.ReadAllBytes(pathB);
        if (a.Length != b.Length) return null;

        const int header = 18;
        int pixels = (a.Length - header) / 3;
        int differing = 0;
        long sumAbs = 0;
        int maxAbs = 0;

        for (int i = header; i + 2 < a.Length; i += 3) {
            int d0 = a[i]     > b[i]     ? a[i]     - b[i]     : b[i]     - a[i];
            int d1 = a[i + 1] > b[i + 1] ? a[i + 1] - b[i + 1] : b[i + 1] - a[i + 1];
            int d2 = a[i + 2] > b[i + 2] ? a[i + 2] - b[i + 2] : b[i + 2] - a[i + 2];
            int m = d0 > d1 ? d0 : d1;
            if (d2 > m) m = d2;
            if (m > 0) differing++;
            if (m > maxAbs) maxAbs = m;
            sumAbs += d0 + d1 + d2;
        }

        return new double[] {
            100.0 * differing / pixels,
            (double)sumAbs / (pixels * 3.0),
            maxAbs
        };
    }
}
'@

$dirA = Join-Path $Root (Join-Path $Take $StreamA)
$dirB = Join-Path $Root (Join-Path $Take $StreamB)
foreach ($d in @($dirA, $dirB)) { if (-not (Test-Path $d)) { throw "Not found: $d" } }

$frames = Get-ChildItem $dirA -Filter *.tga | Sort-Object Name
if (-not $AllFrames) { $frames = $frames | Select-Object -First $MaxFrames }
if ($Stride -gt 1) { $frames = $frames | Where-Object { ([int]($_.BaseName)) % $Stride -eq 0 } }

Write-Host "$Take  ($($frames.Count) frames examined)" -ForegroundColor Cyan

$worst = $null
$worstName = ''

Write-Host "  between eyes ($StreamA vs $StreamB, same frame):"
foreach ($f in $frames) {
    $other = Join-Path $dirB $f.Name
    if (-not (Test-Path $other)) { continue }
    $r = [TgaDiff]::Compare($f.FullName, $other)
    if ($null -eq $r) { Write-Host "    $($f.Name): size mismatch"; continue }
    if ($null -eq $worst -or $r[2] -gt $worst[2] -or ($r[2] -eq $worst[2] -and $r[0] -gt $worst[0])) {
        $worst = $r; $worstName = $f.Name
    }
    if (-not $WorstOnly) {
        "    {0}: {1,7:N2}% differ, mean {2,6:N2}, max {3,3}" -f $f.Name, $r[0], $r[1], $r[2] | Write-Host
    }
}
if ($worst) {
    "    WORST {0}: {1,7:N2}% differ, mean {2,6:N2}, max {3,3}" -f $worstName, $worst[0], $worst[1], $worst[2] |
        Write-Host -ForegroundColor Yellow
}

if (-not $WorstOnly) {
    Write-Host "  within $StreamA (frame N vs N+1, i.e. is the demo moving):"
    for ($i = 0; $i -lt $frames.Count - 1; $i++) {
        $r = [TgaDiff]::Compare($frames[$i].FullName, $frames[$i + 1].FullName)
        if ($null -eq $r) { continue }
        "    {0} -> {1}: {2,7:N2}% differ, mean {3,6:N2}, max {4,3}" -f `
            $frames[$i].Name, $frames[$i + 1].Name, $r[0], $r[1], $r[2] | Write-Host
    }
}
