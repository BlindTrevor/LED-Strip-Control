<#
.SYNOPSIS
    Structural checks on the generated GDTF profile.

.DESCRIPTION
    Not a schema validation - it checks the things that actually go wrong when
    a console imports a hand-built profile, and the things this rig has been
    bitten by before:

      1. description.xml is well-formed and the archive holds it at the root.
      2. Every Geometry a DMXChannel points at exists in the geometry tree.
      3. Every Attribute a channel references is declared, and every
         MainAttribute and Feature reference resolves.
      4. Within a mode, channel offsets are contiguous from 1 with no gaps,
         duplicates or overlaps.
      5. Each mode's channel count matches CM_FOOTPRINT in Protocol.h.
      6. Every ChannelFunction Default lies inside that function's own DMX
         range - a default outside its range is silently clamped by some
         importers and honoured by others.
      7. EVERY strobe channel homes to 0. This is the one that has cost real
         bench time, so it is checked explicitly and loudly.

.EXAMPLE
    pwsh tools/validate-gdtf.ps1
#>
[CmdletBinding()]
param(
    [string]$GdtfPath
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$gdtfDir = Join-Path $repo 'gdtf'
if (-not $GdtfPath) {
    $GdtfPath = Get-ChildItem -Path $gdtfDir -Filter '*.gdtf' |
                Select-Object -First 1 -ExpandProperty FullName
}
if (-not $GdtfPath -or -not (Test-Path $GdtfPath)) {
    Write-Error "No .gdtf found. Run tools/generate-gdtf.ps1 first."
}

$fail = 0
function Fail([string]$m) { $script:fail++; Write-Host "  FAIL  $m" -ForegroundColor Red }
function Pass([string]$m) { Write-Host "  ok    $m" -ForegroundColor Green }

Write-Host "Validating $GdtfPath" -ForegroundColor Cyan
Write-Host ""

# --- 1. archive + well-formedness -----------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($GdtfPath)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -eq 'description.xml' }
    if (-not $entry) {
        Fail "archive has no description.xml at its root"
        $zip.Dispose(); exit 1
    }
    Pass "archive contains description.xml ($($entry.Length) bytes)"
    $reader = New-Object System.IO.StreamReader($entry.Open())
    $xmlText = $reader.ReadToEnd()
    $reader.Dispose()
} finally { $zip.Dispose() }

$xml = New-Object System.Xml.XmlDocument
try { $xml.LoadXml($xmlText); Pass "description.xml is well-formed" }
catch { Fail "description.xml is not well-formed: $_"; exit 1 }

$ft = $xml.SelectSingleNode('/GDTF/FixtureType')
Pass "fixture: $($ft.Manufacturer) / $($ft.Name)  (GDTF $($xml.GDTF.DataVersion))"

# --- 2/3. reference integrity ---------------------------------------------
$geoNames  = @{}
foreach ($g in $xml.SelectNodes('//Geometries//*')) {
    if ($g.Name) { $geoNames[$g.GetAttribute('Name')] = $true }
}
$attrNames = @{}
foreach ($a in $xml.SelectNodes('//AttributeDefinitions/Attributes/Attribute')) {
    $attrNames[$a.GetAttribute('Name')] = $true
}
$featNames = @{}
foreach ($fg in $xml.SelectNodes('//FeatureGroups/FeatureGroup')) {
    foreach ($f in $fg.SelectNodes('Feature')) {
        $featNames["$($fg.GetAttribute('Name')).$($f.GetAttribute('Name'))"] = $true
    }
}
Pass "declared: $($geoNames.Count) geometries, $($attrNames.Count) attributes, $($featNames.Count) features"

foreach ($a in $xml.SelectNodes('//AttributeDefinitions/Attributes/Attribute')) {
    $n = $a.GetAttribute('Name')
    $f = $a.GetAttribute('Feature')
    if ($f -and -not $featNames.ContainsKey($f)) { Fail "attribute '$n' references undeclared feature '$f'" }
    $ma = $a.GetAttribute('MainAttribute')
    if ($ma -and -not $attrNames.ContainsKey($ma)) { Fail "attribute '$n' has undeclared MainAttribute '$ma'" }
}

$badGeo = 0; $badAttr = 0
foreach ($c in $xml.SelectNodes('//DMXChannel')) {
    $g = $c.GetAttribute('Geometry')
    if (-not $geoNames.ContainsKey($g)) { $badGeo++; Fail "channel points at unknown geometry '$g'" }
    foreach ($lc in $c.SelectNodes('LogicalChannel')) {
        foreach ($cf in $lc.SelectNodes('ChannelFunction')) {
            $at = $cf.GetAttribute('Attribute')
            if (-not $attrNames.ContainsKey($at)) { $badAttr++; Fail "channel function references undeclared attribute '$at'" }
        }
    }
}
if ($badGeo  -eq 0) { Pass "every DMXChannel Geometry resolves" }
if ($badAttr -eq 0) { Pass "every ChannelFunction Attribute is declared" }

# --- 5. footprints must match the firmware --------------------------------
$protocol = Join-Path $repo 'WS2811_DMX_ESP32\Protocol.h'
$pm = Select-String -Path $protocol -Pattern '^\s*#define\s+TOTAL_PIXELS\s+(\d+)'
$pixels = [int]$pm.Matches[0].Groups[1].Value
$expected = @(3, 5, 9, 11, (1 + $pixels * 3))

Write-Host ""
$modes = @($xml.SelectNodes('//DMXMode'))
if ($modes.Count -ne $expected.Count) {
    Fail "expected $($expected.Count) DMX modes, found $($modes.Count)"
}

for ($i = 0; $i -lt $modes.Count; $i++) {
    $mode = $modes[$i]
    $name = $mode.GetAttribute('Name')
    $chans = @($mode.SelectNodes('.//DMXChannel'))

    # --- 4. offsets contiguous from 1, no duplicates ---
    $offsets = @()
    foreach ($c in $chans) {
        foreach ($o in ($c.GetAttribute('Offset') -split ',')) { $offsets += [int]$o }
    }
    $sorted = $offsets | Sort-Object
    $dupes  = ($offsets | Group-Object | Where-Object Count -gt 1)
    $okSeq  = $true
    for ($k = 0; $k -lt $sorted.Count; $k++) { if ($sorted[$k] -ne ($k + 1)) { $okSeq = $false; break } }

    $expN = if ($i -lt $expected.Count) { $expected[$i] } else { -1 }
    $okN  = ($chans.Count -eq $expN)

    $line = "  {0,-18} {1,4} channels" -f $name, $chans.Count
    if ($okN -and $okSeq -and -not $dupes) {
        Write-Host "$line  offsets 1..$($sorted[-1]) contiguous" -ForegroundColor Green
    } else {
        Write-Host $line -ForegroundColor Red
        if (-not $okN)   { Fail "$name has $($chans.Count) channels, firmware footprint says $expN" }
        if ($dupes)      { Fail "$name has duplicate offsets: $($dupes.Name -join ', ')" }
        if (-not $okSeq) { Fail "$name offsets are not contiguous from 1" }
    }

    # --- 6/7. defaults ---
    $strobeChans = 0
    foreach ($c in $chans) {
        foreach ($lc in $c.SelectNodes('LogicalChannel')) {
            $isStrobe = ($lc.GetAttribute('Attribute') -eq 'Shutter1')
            if ($isStrobe) { $strobeChans++ }

            $cfs = @($lc.SelectNodes('ChannelFunction'))
            for ($j = 0; $j -lt $cfs.Count; $j++) {
                $cf   = $cfs[$j]
                $from = [int](($cf.GetAttribute('DMXFrom') -split '/')[0])
                # A function runs until the next one starts, or to 255.
                $to = if ($j + 1 -lt $cfs.Count) {
                          [int](($cfs[$j+1].GetAttribute('DMXFrom') -split '/')[0]) - 1
                      } else { 255 }
                $defRaw = $cf.GetAttribute('Default')
                if ($defRaw) {
                    $def = [int](($defRaw -split '/')[0])
                    if ($def -lt $from -or $def -gt $to) {
                        Fail "$name / $($cf.GetAttribute('Name')): Default $def outside its range $from..$to"
                    }
                }
            }

            # THE important one: a patched fixture must sit still.
            if ($isStrobe) {
                $first = $cfs[0]
                $def   = [int](($first.GetAttribute('Default') -split '/')[0])
                $init  = $c.GetAttribute('InitialFunction')
                if ($def -ne 0) {
                    Fail "$name strobe homes to $def, not 0"
                } elseif (-not $init) {
                    Fail "$name strobe has no explicit InitialFunction - home value is importer-dependent"
                } elseif (-not $init.EndsWith('.Open')) {
                    Fail "$name strobe InitialFunction is '$init', expected to end .Open"
                } else {
                    Write-Host "        strobe homes to 0 (InitialFunction=$init)" -ForegroundColor DarkGray
                }
            }
        }
    }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "PASS - no structural problems found." -ForegroundColor Green
    exit 0
}
Write-Host "$fail problem(s) found." -ForegroundColor Red
exit 1
