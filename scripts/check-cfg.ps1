# Fails if a key is bound twice inside one CS2 config, or if a bound key is missing from
# that file's own echo lines.
#
# A bind is last-one-wins and CS2 says nothing about it. vr.cfg grew a block of stereo
# diagnostics at the bottom that silently took eleven keys off the bindings above, while
# the echo lines at the top went on advertising the old layout - so F8 read "free look"
# and set an OpenXR latency mode. The person pressing these has a headset on and cannot
# see the console, which is exactly the situation in which a wrong key map is expensive.
#
#   .\check-cfg.ps1                 # every cfg in scripts\cs2
#   .\check-cfg.ps1 -Path a.cfg     # just this one
#
# Exits non-zero on the first problem, so CI can run it.

param(
    [string[]]$Path
)

$ErrorActionPreference = 'Stop'

if (-not $Path) {
    $root = Join-Path (Split-Path -Parent $PSCommandPath) 'cs2'
    $Path = Get-ChildItem -Path $root -Filter *.cfg | ForEach-Object { $_.FullName }
}

$problems = 0

foreach ($file in $Path) {
    $name = Split-Path -Leaf $file
    $lines = Get-Content -LiteralPath $file

    $announcesItsKeys = $name -like "vr*.cfg"

    $bound = [ordered]@{}
    $echoed = New-Object System.Collections.Generic.HashSet[string]

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i].Trim()
        if ($line -match '^//') { continue }

        if ($line -match '^bind\s+"([^"]+)"\s+"(.*)"\s*$') {
            $key = $matches[1].ToUpper()
            $cmd = $matches[2]
            if ($bound.Contains($key)) {
                Write-Host "$name : $key is bound twice - line $($bound[$key].Line) says '$($bound[$key].Cmd)', line $($i + 1) says '$cmd'" -ForegroundColor Red
                $problems++
            }
            $bound[$key] = [pscustomobject]@{ Line = $i + 1; Cmd = $cmd }
        }
        elseif ($line -match '^echo\s') {
            # Keys are named in the echo text as bare words: "F9/F5 headset", "PGDN ...".
            foreach ($m in [regex]::Matches($line.ToUpper(), '\b(F1[0-2]|F[1-9]|HOME|END|INS|DEL|PGUP|PGDN|LEFTARROW|RIGHTARROW|UPARROW|DOWNARROW)\b')) {
                [void]$echoed.Add($m.Groups[1].Value)
            }
        }
    }

    foreach ($key in $bound.Keys) {
        if ($announcesItsKeys -and -not $echoed.Contains($key)) {
            Write-Host "$name : $key is bound to '$($bound[$key].Cmd)' but no echo line mentions it" -ForegroundColor Red
            $problems++
        }
    }

    foreach ($key in $echoed) {
        if ($announcesItsKeys -and -not $bound.Contains($key)) {
            Write-Host "$name : an echo line advertises $key, which this file does not bind" -ForegroundColor Red
            $problems++
        }
    }

    if ($bound.Count -gt 0) {
        $suffix = if ($announcesItsKeys) { "keys, each bound once and each announced" } else { "keys, each bound once" }
        Write-Host "$name : $($bound.Count) $suffix" -ForegroundColor Green
    }
}

if ($problems -gt 0) {
    Write-Host "$problems problem(s)." -ForegroundColor Red
    exit 1
}

Write-Host 'All configs are consistent.' -ForegroundColor Green
