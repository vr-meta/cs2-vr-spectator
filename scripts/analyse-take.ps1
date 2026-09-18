# Phase C analysis of one HLAE take.
#
# Reports two things that answer different questions:
#
#   between eyes  - eyeL vs eyeR of the SAME frame. This is the stereo difference.
#   within eye    - frame N vs frame N+1 of ONE stream. This is how much the scene
#                   moves between frames, which says whether the demo was playing.
#
# The decisive phase C measurement is "between eyes" with separation 0 on a PLAYING
# demo: it must be 0.00%. Anything else means something advances between render passes,
# and the two eyes would see different moments of the same smoke.
#
#   .\analyse-take.ps1 -Take exp05_stereo\take0000

param(
    [Parameter(Mandatory = $true)][string]$Take,
    [string]$Root = 'D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\bin\win64',
    [string]$StreamA = 'eyeL',
    [string]$StreamB = 'eyeR',
    [int]$MaxFrames = 4
)

$ErrorActionPreference = 'Stop'

function Get-Diff([string]$pathA, [string]$pathB) {
    $a = [IO.File]::ReadAllBytes($pathA)
    $b = [IO.File]::ReadAllBytes($pathB)
    if ($a.Length -ne $b.Length) { return $null }

    $header = 18
    $pixels = ($a.Length - $header) / 3
    $differing = 0
    [long]$sumAbs = 0
    $maxAbs = 0

    for ($i = $header; $i -lt $a.Length; $i += 3) {
        $d0 = [Math]::Abs($a[$i]     - $b[$i])
        $d1 = [Math]::Abs($a[$i + 1] - $b[$i + 1])
        $d2 = [Math]::Abs($a[$i + 2] - $b[$i + 2])
        $m = [Math]::Max($d0, [Math]::Max($d1, $d2))
        if ($m -gt 0) { $differing++ }
        if ($m -gt $maxAbs) { $maxAbs = $m }
        $sumAbs += $d0 + $d1 + $d2
    }

    [pscustomobject]@{
        Pct    = 100.0 * $differing / $pixels
        Mean   = $sumAbs / ($pixels * 3.0)
        MaxAbs = $maxAbs
    }
}

$dirA = Join-Path $Root (Join-Path $Take $StreamA)
$dirB = Join-Path $Root (Join-Path $Take $StreamB)
foreach ($d in @($dirA, $dirB)) { if (-not (Test-Path $d)) { throw "Not found: $d" } }

$files = Get-ChildItem $dirA -Filter *.tga | Sort-Object Name | Select-Object -First $MaxFrames

Write-Host "$Take" -ForegroundColor Cyan
Write-Host "  between eyes ($StreamA vs $StreamB, same frame):"
foreach ($f in $files) {
    $other = Join-Path $dirB $f.Name
    if (-not (Test-Path $other)) { continue }
    $r = Get-Diff $f.FullName $other
    if ($null -eq $r) { Write-Host "    $($f.Name): size mismatch"; continue }
    "    {0}: {1,7:N2}% differ, mean {2,6:N2}, max {3,3}" -f $f.Name, $r.Pct, $r.Mean, $r.MaxAbs | Write-Host
}

Write-Host "  within $StreamA (frame N vs N+1, i.e. is the demo moving):"
for ($i = 0; $i -lt $files.Count - 1; $i++) {
    $r = Get-Diff $files[$i].FullName $files[$i + 1].FullName
    if ($null -eq $r) { continue }
    "    {0} -> {1}: {2,7:N2}% differ, mean {3,6:N2}, max {4,3}" -f `
        $files[$i].Name, $files[$i + 1].Name, $r.Pct, $r.Mean, $r.MaxAbs | Write-Host
}
