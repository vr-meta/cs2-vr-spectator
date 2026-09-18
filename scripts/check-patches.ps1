<#
.SYNOPSIS
    Checks that this project's sources and patches still reconstruct a buildable advancedfx tree.

.DESCRIPTION
    Takes a clean checkout of advancedfx at the pinned tag in a throwaway worktree, copies in
    src/AfxHookSource2/, applies both patches, and reports the result. Nothing is built: this
    answers "do the patches still apply", which is the question that breaks silently when
    upstream moves or when a patch is regenerated carelessly.

    The two patches overlapped once — 002 was generated from a tree that already had 001
    applied, so it carried 001's hunk as well and the pair could not be applied in sequence.
    That went unnoticed for a while because the one machine with a toolchain never applied
    them from clean. This script is how that stays noticed.

.PARAMETER AdvancedfxPath
    An existing advancedfx clone to take the worktree from. Avoids a fresh clone.

.PARAMETER Ref
    Upstream revision to check against. Defaults to the pinned tag; pass 'main' to see
    whether the patches have rotted against upstream.

.EXAMPLE
    scripts\check-patches.ps1
    scripts\check-patches.ps1 -Ref main
#>
[CmdletBinding()]
param(
    [string] $AdvancedfxPath = 'D:\Dev\cs2-vr-tools\advancedfx',
    [string] $Ref = 'v2.192.2',
    [string] $WorkDir
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 turns a native command's stderr into an ErrorRecord, and with
# ErrorActionPreference = Stop that aborts the script on git's ordinary progress chatter.
# Every git call here goes through this, which collects both streams and reports only the
# exit code.
function Invoke-Git {
    param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Arguments)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & git @Arguments 2>&1 | ForEach-Object { "$_" }
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    } finally {
        $ErrorActionPreference = $prev
    }
}

$repo = Split-Path -Parent $PSScriptRoot
$patches = @(
    Join-Path $repo 'docs\patches\001-vswhere-products.patch'
    Join-Path $repo 'docs\patches\002-per-pass-camera.patch'
)
foreach ($p in $patches) {
    if (-not (Test-Path $p)) { throw "Patch not found: $p" }
}

if (-not $WorkDir) {
    $WorkDir = Join-Path ([IO.Path]::GetTempPath()) ("afx-patchcheck-" + [Guid]::NewGuid().ToString('n').Substring(0, 8))
}

if (-not (Test-Path $AdvancedfxPath)) {
    throw "No advancedfx clone at $AdvancedfxPath. Clone it (see docs/install.md) or pass -AdvancedfxPath."
}

Write-Host "advancedfx : $AdvancedfxPath"
Write-Host "ref        : $Ref"
Write-Host "worktree   : $WorkDir"
Write-Host ''

$created = $false
try {
    Push-Location $AdvancedfxPath
    $add = Invoke-Git worktree add --detach $WorkDir $Ref
    if ($add.ExitCode -ne 0) {
        $add.Output | ForEach-Object { Write-Host "        $_" }
        throw "git worktree add failed for ref '$Ref'"
    }
    $created = $true
    Pop-Location

    Push-Location $WorkDir

    Copy-Item (Join-Path $repo 'src\AfxHookSource2\*') 'AfxHookSource2\' -Force
    Write-Host 'sources copied in.'

    $failed = @()
    foreach ($p in $patches) {
        $name = Split-Path -Leaf $p
        # --verbose so a rejection names the hunk; git apply preserves the tree's CRLF,
        # which is why patches are applied this way and never with sed.
        $apply = Invoke-Git apply --verbose $p
        if ($apply.ExitCode -eq 0) {
            Write-Host "OK      $name"
        } else {
            Write-Host "FAILED  $name" -ForegroundColor Red
            $apply.Output | Where-Object { $_ -match '^error' } | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
            $failed += $name
        }
    }

    Pop-Location

    Write-Host ''
    if ($failed.Count -gt 0) {
        throw "Patches that did not apply against ${Ref}: $($failed -join ', ')"
    }
    Write-Host "All patches apply cleanly against $Ref." -ForegroundColor Green
}
finally {
    if ((Get-Location).Path -eq $WorkDir) { Pop-Location }
    if ($created) {
        Push-Location $AdvancedfxPath
        Invoke-Git worktree remove --force $WorkDir | Out-Null
        Pop-Location
    }
}
