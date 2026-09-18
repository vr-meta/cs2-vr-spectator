# Converts HLAE's uncompressed 24-bit TGA output to PNG so it can be looked at.
#
# HLAE writes: 18-byte header, then BGR. Width and height are little endian at offsets
# 12 and 14. Row order comes from bit 5 of the image descriptor at offset 17: HLAE sets
# it (0x20), so these files are TOP-DOWN, not the bottom-up that TGA defaults to.
#
#   .\tga-to-png.ps1 -In <file.tga> -Out <file.png>

param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$b = [IO.File]::ReadAllBytes($In)

$w = [BitConverter]::ToUInt16($b, 12)
$h = [BitConverter]::ToUInt16($b, 14)
$bpp = $b[16]
if ($bpp -ne 24) { throw "Expected 24 bpp, got $bpp" }

$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)

$stride = $data.Stride
$row = New-Object byte[] $stride

$topDown = 0 -ne ($b[17] -band 0x20)

for ($y = 0; $y -lt $h; $y++) {
    # GDI+ rows are top-down; flip only if the file is not.
    $srcRow = if ($topDown) { $y } else { $h - 1 - $y }
    $src = 18 + $srcRow * $w * 3
    [Array]::Copy($b, $src, $row, 0, $w * 3)
    [Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]($data.Scan0.ToInt64() + $y * $stride), $stride)
}

$bmp.UnlockBits($data)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Host "$Out  ($w x $h)" -ForegroundColor Green
