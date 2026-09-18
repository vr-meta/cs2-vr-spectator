# Installs the build toolchain for this project.
#
# Run from an elevated PowerShell. Nothing here is project-specific beyond the
# component selection; it is safe to skip any package already installed by hand.
#
# Verify afterwards with scripts/check-toolchain.ps1

$ErrorActionPreference = 'Stop'

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget not found. Install App Installer from the Microsoft Store first.'
}

Write-Host 'Visual Studio 2022 Build Tools (C++ x64)...' -ForegroundColor Cyan
winget install --id Microsoft.VisualStudio.2022.BuildTools --accept-source-agreements --accept-package-agreements `
    --override '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended'

Write-Host 'CMake...' -ForegroundColor Cyan
winget install --id Kitware.CMake --accept-source-agreements --accept-package-agreements

Write-Host 'Ninja...' -ForegroundColor Cyan
winget install --id Ninja-build.Ninja --accept-source-agreements --accept-package-agreements

# Needed only if a Vulkan path is pursued. CS2 itself is D3D11, so this is optional
# for the CS2 side and relevant mainly for reading the portal2vr DXVK reference.
Write-Host 'Vulkan SDK (optional, for the portal2vr reference)...' -ForegroundColor Cyan
winget install --id KhronosGroup.VulkanSDK --accept-source-agreements --accept-package-agreements

Write-Host ''
Write-Host 'Done. Open a NEW shell so PATH changes take effect, then run:' -ForegroundColor Green
Write-Host '  powershell -ExecutionPolicy Bypass -File scripts\check-toolchain.ps1'
