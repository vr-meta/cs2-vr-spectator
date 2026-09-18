# Writes an amplified difference image between two TGA frames, so a small numeric
# difference can be located on screen rather than only counted.
#
#   .\diff-image.ps1 -A <eyeL\00048.tga> -B <eyeR\00048.tga> -Out diff.png -Gain 8
#
# Output is greyscale: black where the frames agree, brighter where they differ, with
# the difference multiplied by -Gain and clamped. A structured bright region says the
# difference is content; scattered single pixels say it is dithering.

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Gain = 8
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$da = [IO.File]::ReadAllBytes($A)
$db = [IO.File]::ReadAllBytes($B)
if ($da.Length -ne $db.Length) { throw 'Frames differ in size.' }

$w = [BitConverter]::ToUInt16($da, 12)
$h = [BitConverter]::ToUInt16($da, 14)
$topDown = 0 -ne ($da[17] -band 0x20)

$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
$stride = $data.Stride
$row = New-Object byte[] $stride

for ($y = 0; $y -lt $h; $y++) {
    $srcRow = if ($topDown) { $y } else { $h - 1 - $y }
    $base = 18 + $srcRow * $w * 3
    for ($x = 0; $x -lt $w; $x++) {
        $i = $base + $x * 3
        $d0 = [Math]::Abs($da[$i]     - $db[$i])
        $d1 = [Math]::Abs($da[$i + 1] - $db[$i + 1])
        $d2 = [Math]::Abs($da[$i + 2] - $db[$i + 2])
        $m = [Math]::Max($d0, [Math]::Max($d1, $d2)) * $Gain
        if ($m -gt 255) { $m = 255 }
        $o = $x * 3
        $row[$o] = $m; $row[$o + 1] = $m; $row[$o + 2] = $m
    }
    [Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]($data.Scan0.ToInt64() + $y * $stride), $stride)
}

$bmp.UnlockBits($data)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "$Out  (gain x$Gain)" -ForegroundColor Green
