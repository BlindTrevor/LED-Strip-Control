<#
.SYNOPSIS
    Fails if the shared headers have drifted between the two sketch folders.

.DESCRIPTION
    Arduino IDE 2.x only compiles headers that sit inside the sketch folder, so
    anything both sketches need is duplicated. Two files are shared:

      Protocol.h   the wire format between controller and satellites. A one-byte
                   difference is a silent, ugly bug: the structs stop agreeing,
                   the satellites decode garbage, and nothing reports an error
                   because the I2C writes still succeed.

      Effects.h    what each effect looks like. Both the satellite's renderer
                   and the controller's Preview tab call fxRender(), so drift
                   here means the screen confidently shows one thing while the
                   tape does another.

    This compares each pair byte for byte and exits non-zero if any differ.

.PARAMETER Fix
    Copy the controller's copies over the satellite's, making them identical.

.PARAMETER InstallHook
    Install a git pre-commit hook that runs this check automatically.

.EXAMPLE
    pwsh tools/check-protocol-sync.ps1
    pwsh tools/check-protocol-sync.ps1 -Fix
    pwsh tools/check-protocol-sync.ps1 -InstallHook
#>
[CmdletBinding()]
param(
    [switch]$Fix,
    [switch]$InstallHook
)

$ErrorActionPreference = 'Stop'

$repo   = Split-Path -Parent $PSScriptRoot
$shared = @('Protocol.h', 'Effects.h')

if ($InstallHook) {
    $hookDir = Join-Path $repo '.git\hooks'
    if (-not (Test-Path $hookDir)) {
        Write-Error "No .git/hooks directory at $hookDir - is this a git checkout?"
    }
    $hook = Join-Path $hookDir 'pre-commit'
    @'
#!/bin/sh
# Refuse a commit in which the shared headers differ between sketch folders.
exec pwsh -NoProfile -File "$(git rev-parse --show-toplevel)/tools/check-protocol-sync.ps1"
'@ | Set-Content -Path $hook -Encoding ascii -NoNewline
    Write-Host "Installed pre-commit hook at $hook" -ForegroundColor Green
    exit 0
}

$drifted = @()

foreach ($name in $shared) {
    $reference = Join-Path $repo "WS2811_DMX_ESP32\$name"
    $copy      = Join-Path $repo "WS2811_Satellite\$name"

    foreach ($f in @($reference, $copy)) {
        if (-not (Test-Path $f)) { Write-Error "Missing: $f" }
    }

    $a = [System.IO.File]::ReadAllBytes($reference)
    $b = [System.IO.File]::ReadAllBytes($copy)

    $identical = ($a.Length -eq $b.Length)
    if ($identical) {
        for ($i = 0; $i -lt $a.Length; $i++) {
            if ($a[$i] -ne $b[$i]) { $identical = $false; break }
        }
    }

    if ($identical) {
        Write-Host "${name}: in sync ($($a.Length) bytes)." -ForegroundColor Green
        continue
    }

    if ($Fix) {
        Copy-Item -Path $reference -Destination $copy -Force
        Write-Host "${name}: copied controller -> satellite. Now in sync." -ForegroundColor Yellow
        continue
    }

    $drifted += [pscustomobject]@{ Name = $name; Reference = $reference; Copy = $copy }
}

if ($Fix) {
    Write-Host "Rebuild and reflash BOTH boards - the wire format or the effects may have changed."
    exit 0
}

if ($drifted.Count -eq 0) { exit 0 }

foreach ($d in $drifted) {
    Write-Host "$($d.Name): OUT OF SYNC." -ForegroundColor Red
    Write-Host "  $($d.Reference)"
    Write-Host "  $($d.Copy)"
    Write-Host ""

    # git diff renders this far better than anything hand-rolled, and it is
    # already on the machine if this is a checkout.
    $gitDiff = & git --no-pager diff --no-index -- $d.Reference $d.Copy 2>$null
    if ($LASTEXITCODE -ne 0 -and $gitDiff) { $gitDiff | Write-Host }
    Write-Host ""
}

Write-Host "Run with -Fix to copy the controller's copies over the satellite's," -ForegroundColor Yellow
Write-Host "then rebuild and reflash BOTH boards." -ForegroundColor Yellow
exit 1
