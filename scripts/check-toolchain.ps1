# Reports what the machine has, so a failed build is diagnosed before it is run.
# Read-only. Safe to run at any time.

Write-Host '=== Build tools ===' -ForegroundColor Cyan

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vc = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vc) { Write-Host "  MSVC x64      : $vc" } else { Write-Host '  MSVC x64      : MISSING (workload not installed)' -ForegroundColor Red }
} else {
    Write-Host '  MSVC x64      : MISSING (no vswhere)' -ForegroundColor Red
}

foreach ($tool in 'cmake', 'ninja', 'git', 'gh') {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) { Write-Host ("  {0,-14}: {1}" -f $tool, $cmd.Source) }
    else      { Write-Host ("  {0,-14}: MISSING" -f $tool) -ForegroundColor Red }
}

if ($env:VULKAN_SDK) { Write-Host "  Vulkan SDK    : $env:VULKAN_SDK" }
else                 { Write-Host '  Vulkan SDK    : not set (optional for CS2, which is D3D11)' -ForegroundColor Yellow }

Write-Host ''
Write-Host '=== VR runtimes ===' -ForegroundColor Cyan

$xr = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction SilentlyContinue).ActiveRuntime
if ($xr) { Write-Host "  OpenXR runtime: $xr" } else { Write-Host '  OpenXR runtime: none registered' -ForegroundColor Red }

Get-Service OVRService, OVRLibraryService -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host ("  {0,-14}: {1}" -f $_.Name, $_.Status) }

Write-Host ''
Write-Host '=== GPU ===' -ForegroundColor Cyan
Get-CimInstance Win32_VideoController |
    ForEach-Object { Write-Host ("  {0} ({1})" -f $_.Name, $_.DriverVersion) }

Write-Host ''
Write-Host '=== CS2 ===' -ForegroundColor Cyan
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
if ($steam) {
    $libs = Join-Path $steam 'steamapps\libraryfolders.vdf'
    $paths = @()
    if (Test-Path $libs) {
        $paths = Select-String -Path $libs -Pattern '"path"\s+"(.+?)"' |
                 ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    $found = $false
    foreach ($p in $paths) {
        $manifest = Join-Path $p 'steamapps\appmanifest_730.acf'
        if (Test-Path $manifest) {
            $found = $true
            $txt = Get-Content $manifest -Raw
            $state = if ($txt -match '"StateFlags"\s+"(\d+)"') { $matches[1] } else { '?' }
            $done  = if ($txt -match '"BytesDownloaded"\s+"(\d+)"') { [int64]$matches[1] } else { 0 }
            $total = if ($txt -match '"BytesToDownload"\s+"(\d+)"') { [int64]$matches[1] } else { 0 }
            Write-Host "  manifest      : $manifest"
            Write-Host "  StateFlags    : $state  (4 = fully installed)"
            if ($total -gt 0 -and $state -ne '4') {
                $pct = [math]::Round(100 * $done / $total, 1)
                Write-Host "  downloaded    : $pct%  (manifest lags behind the Steam UI)" -ForegroundColor Yellow
            }
        }
    }
    if (-not $found) { Write-Host '  CS2           : no appmanifest_730.acf found' -ForegroundColor Red }
} else {
    Write-Host '  Steam         : not found' -ForegroundColor Red
}
