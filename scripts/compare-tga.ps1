# Compares the two stream folders of one HLAE take, frame by frame.
#
# HLAE writes uncompressed 24-bit TGA: 18-byte header, then BGR rows bottom-up. Raw
# byte comparison is enough to answer "are these the same image", so no decoder is
# needed - see docs/workflow.md.
#
#   .\compare-tga.ps1 -Take exp04_probe\take0001
#
# Reports, per frame, the percentage of differing pixels and the mean absolute
# difference. A control take where both streams render the same camera must come out
# at 0.00% - a test that cannot reproduce its own baseline is not measuring anything.

param(
    [Parameter(Mandatory = $true)][string]$Take,
    [string]$Root = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64',
    [string]$StreamA = 'eyeL',
    [string]$StreamB = 'eyeR',
    [int]$MaxFrames = 5
)

$ErrorActionPreference = 'Stop'

$dirA = Join-Path $Root (Join-Path $Take $StreamA)
$dirB = Join-Path $Root (Join-Path $Take $StreamB)

foreach ($d in @($dirA, $dirB)) {
    if (-not (Test-Path $d)) { throw "Not found: $d" }
}

$filesA = Get-ChildItem $dirA -Filter *.tga | Sort-Object Name | Select-Object -First $MaxFrames

Write-Host "$Take : $StreamA vs $StreamB" -ForegroundColor Cyan

foreach ($fa in $filesA) {
    $fb = Join-Path $dirB $fa.Name
    if (-not (Test-Path $fb)) { Write-Host "  $($fa.Name): missing in $StreamB" -ForegroundColor Yellow; continue }

    $a = [IO.File]::ReadAllBytes($fa.FullName)
    $b = [IO.File]::ReadAllBytes($fb)

    if ($a.Length -ne $b.Length) {
        Write-Host "  $($fa.Name): different size ($($a.Length) vs $($b.Length))" -ForegroundColor Yellow
        continue
    }

    $header = 18
    $pixels  = ($a.Length - $header) / 3
    $differing = 0
    [long]$sumAbs = 0

    for ($i = $header; $i -lt $a.Length; $i += 3) {
        $d0 = [Math]::Abs($a[$i]     - $b[$i])
        $d1 = [Math]::Abs($a[$i + 1] - $b[$i + 1])
        $d2 = [Math]::Abs($a[$i + 2] - $b[$i + 2])
        if ($d0 -or $d1 -or $d2) { $differing++ }
        $sumAbs += $d0 + $d1 + $d2
    }

    $pct  = 100.0 * $differing / $pixels
    $mean = $sumAbs / ($pixels * 3.0)

    "  {0}: {1,7:N2}% pixels differ, mean abs diff {2,6:N2}" -f $fa.Name, $pct, $mean | Write-Host
}
