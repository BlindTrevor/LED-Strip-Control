<#
.SYNOPSIS
    Fails if the two copies of Protocol.h have drifted apart.

.DESCRIPTION
    Protocol.h defines the wire format between the ESP32-S3 controller and the
    D1 Mini satellites. Arduino IDE 2.x will only compile headers that sit in
    the sketch folder, so the file is duplicated - and a one-byte difference
    between the two copies is a silent, ugly bug: the structs stop agreeing,
    the satellites decode garbage, and nothing reports an error because I2C
    writes still succeed.

    This compares the files byte for byte and exits non-zero if they differ.

.PARAMETER Fix
    Copy the controller's copy over the satellite's, making them identical.

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

$repo      = Split-Path -Parent $PSScriptRoot
$reference = Join-Path $repo 'WS2811_DMX_ESP32\Protocol.h'
$copy      = Join-Path $repo 'WS2811_Satellite\Protocol.h'

if ($InstallHook) {
    $hookDir = Join-Path $repo '.git\hooks'
    if (-not (Test-Path $hookDir)) {
        Write-Error "No .git/hooks directory at $hookDir - is this a git checkout?"
    }
    $hook = Join-Path $hookDir 'pre-commit'
    @'
#!/bin/sh
# Refuse a commit in which the two copies of Protocol.h differ.
exec pwsh -NoProfile -File "$(git rev-parse --show-toplevel)/tools/check-protocol-sync.ps1"
'@ | Set-Content -Path $hook -Encoding ascii -NoNewline
    Write-Host "Installed pre-commit hook at $hook" -ForegroundColor Green
    exit 0
}

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
    Write-Host "Protocol.h: in sync ($($a.Length) bytes)." -ForegroundColor Green
    exit 0
}

if ($Fix) {
    Copy-Item -Path $reference -Destination $copy -Force
    Write-Host "Protocol.h: copied controller -> satellite. Now in sync." -ForegroundColor Yellow
    Write-Host "Rebuild and reflash BOTH boards - the wire format may have changed."
    exit 0
}

Write-Host "Protocol.h: OUT OF SYNC." -ForegroundColor Red
Write-Host "  $reference"
Write-Host "  $copy"
Write-Host ""

# git diff renders this far better than anything hand-rolled, and it is already
# on the machine if this is a checkout.
$gitDiff = & git --no-pager diff --no-index -- $reference $copy 2>$null
if ($LASTEXITCODE -ne 0 -and $gitDiff) { $gitDiff | Write-Host }

Write-Host ""
Write-Host "Run with -Fix to copy the controller's copy over the satellite's," -ForegroundColor Yellow
Write-Host "then rebuild and reflash BOTH boards." -ForegroundColor Yellow
exit 1
