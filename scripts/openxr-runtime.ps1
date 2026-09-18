<#
.SYNOPSIS
    Shows, or changes, which OpenXR runtime this machine uses by default.

.DESCRIPTION
    Prefer not to use the -Set half of this.

    `launch-cs2-experiment.ps1 -MetaRuntime` selects a runtime for CS2 alone, through the
    XR_RUNTIME_JSON environment variable, which needs no elevation and changes nothing for
    anything else. This script exists for the case where something other than our launcher
    has to see a different runtime -- and because knowing what the machine is set to is
    worth a command.

    Changing the registry key affects EVERY VR application installed, until it is changed
    back. The previous value is written next to it as ActiveRuntime_PreviousValue so
    -Restore can undo it.

.EXAMPLE
    scripts\openxr-runtime.ps1
    scripts\openxr-runtime.ps1 -Set Meta        # needs an elevated shell
    scripts\openxr-runtime.ps1 -Restore
#>
[CmdletBinding()]
param(
    [ValidateSet('Meta', 'SteamVR')] [string] $Set,
    [switch] $Restore
)

$ErrorActionPreference = 'Stop'

$key = 'HKLM:\SOFTWARE\Khronos\OpenXR\1'

$known = @{
    Meta    = 'C:\Program Files\Meta Horizon\Support\oculus-runtime\oculus_openxr_64.json'
    SteamVR = 'D:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json'
}

function Get-Active {
    if (-not (Test-Path $key)) { return $null }
    (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).ActiveRuntime
}

function Show-State {
    $active = Get-Active
    Write-Host "active runtime : $(if ($active) { $active } else { '(none registered)' })"

    $previous = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).ActiveRuntime_PreviousValue
    if ($previous) { Write-Host "previous       : $previous" }

    Write-Host ''
    Write-Host 'installed runtimes:'
    foreach ($name in $known.Keys | Sort-Object) {
        $path = $known[$name]
        $mark = if ($active -and ($active -ieq $path)) { '*' } else { ' ' }
        $state = if (Test-Path $path) { 'present' } else { 'not installed' }
        Write-Host ("  {0} {1,-8} {2}  {3}" -f $mark, $name, $state, $path)
    }
    Write-Host ''
    Write-Host 'For CS2 alone, prefer:  launch-cs2-experiment.ps1 -MetaRuntime'
    Write-Host 'To see what a process would actually get:  tools/xr-probe'
}

function Assert-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Writing HKLM needs an elevated shell. Run this from an Administrator prompt.'
    }
}

if ($Set -and $Restore) { throw 'Pass -Set or -Restore, not both.' }

if ($Restore) {
    Assert-Elevated
    $previous = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).ActiveRuntime_PreviousValue
    if (-not $previous) { throw 'Nothing recorded to restore. This script did not make the last change.' }
    Set-ItemProperty -Path $key -Name ActiveRuntime -Value $previous
    Remove-ItemProperty -Path $key -Name ActiveRuntime_PreviousValue
    Write-Host "restored to $previous" -ForegroundColor Green
    Write-Host ''
    Show-State
    return
}

if ($Set) {
    Assert-Elevated
    $target = $known[$Set]
    if (-not (Test-Path $target)) { throw "$Set is not installed at $target" }

    $active = Get-Active
    if ($active -ieq $target) {
        Write-Host "already $Set." -ForegroundColor Green
        return
    }

    Write-Host 'This is a machine-wide setting and affects every VR application.' -ForegroundColor Yellow
    if ($active) { Set-ItemProperty -Path $key -Name ActiveRuntime_PreviousValue -Value $active }
    Set-ItemProperty -Path $key -Name ActiveRuntime -Value $target
    Write-Host "active runtime is now $Set" -ForegroundColor Green
    Write-Host '-Restore puts it back.'
    Write-Host ''
}

Show-State
