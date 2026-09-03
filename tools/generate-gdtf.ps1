<#
.SYNOPSIS
    Builds the GDTF fixture profile for the DMX512 -> WS2811 strip controller.

.DESCRIPTION
    Emits one FixtureType - Sonic Lighting / LED Strip - with a DMX mode per
    channel mode in the firmware:

        3ch RGB          Red Green Blue
        5ch Dim+RGB      Master RGB Strobe
        9ch Spectrum     Master, spectrum/intensity pairs, Mode Speed Size Strobe
        11ch RGB + BG    Master RGB, BG RGB, Mode Speed Size Strobe
        Pixel Map        Master then R,G,B per pixel

    Pixel count is parsed out of WS2811_DMX_ESP32/Protocol.h so the profile
    cannot drift from the firmware. Raise TOTAL_PIXELS there, re-run this, and
    the pixel-map mode follows.

    DELIBERATE CHOICES
    ------------------
    * Strobe homes to 0. Every strobe channel's first ChannelFunction is a
      plain "Open" spanning 0-7 with Default="0/1", so a freshly patched
      fixture sits still instead of hammering. A full-strobe default has cost
      real head-scratching on this rig before.
    * No GeometryReference anywhere. ZerOS does not build cells from one - a
      valid 100-cell reference imports as a single flat fixture with numbered
      parameters. The pixel-map mode therefore uses 100 ordinary child Beam
      geometries, which flattens to the same thing but without pretending
      otherwise. Real multicell needs a native profile from Zero 88.
    * Dimmer defaults to 0 in every mode that has one, and in those modes the
      foreground RGB defaults to 255 - so pushing the master fader alone gives
      white. 3ch has no master, so its RGB defaults to 0 to avoid a fixture
      that comes up full white the moment it is patched.

.PARAMETER OutDir
    Where to write. Defaults to the repo's gdtf/ folder.

.EXAMPLE
    pwsh tools/generate-gdtf.ps1
#>
[CmdletBinding()]
param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'gdtf' }

# Keep this stable across regenerations. Consoles key their patch on it, so a
# new GUID looks like a different fixture and orphans existing shows.
$FixtureTypeID = '6F2B1A44-9C3E-4D51-8A70-2E5C93B41D08'
$Manufacturer  = 'Sonic Lighting'
$ModelName     = 'LED Strip'
$ShortName     = 'LEDSTRIP'
$RevisionText  = 'Channel layout matching firmware modes 3/5/9/11/301.'

# --- pixel count comes from the firmware, not from here --------------------
$protocol = Join-Path $repo 'WS2811_DMX_ESP32\Protocol.h'
if (-not (Test-Path $protocol)) { Write-Error "Cannot find $protocol" }
$m = Select-String -Path $protocol -Pattern '^\s*#define\s+TOTAL_PIXELS\s+(\d+)'
if (-not $m) { Write-Error "No TOTAL_PIXELS #define in $protocol" }
$PIXELS = [int]$m.Matches[0].Groups[1].Value
Write-Host "TOTAL_PIXELS = $PIXELS (from Protocol.h)" -ForegroundColor Cyan

$IDENTITY = '{1.000000,0.000000,0.000000,0.000000}{0.000000,1.000000,0.000000,0.000000}{0.000000,0.000000,1.000000,0.000000}{0.000000,0.000000,0.000000,1.000000}'

# 5 m of tape, pixels evenly spaced along X and centred on the body origin.
function Get-PixelMatrix([int]$index) {
    $pitch = 5.0 / $PIXELS
    $x = ($index + 0.5) * $pitch - 2.5
    $xs = $x.ToString('F6', [System.Globalization.CultureInfo]::InvariantCulture)
    return "{1.000000,0.000000,0.000000,0.000000}{0.000000,1.000000,0.000000,0.000000}{0.000000,0.000000,1.000000,0.000000}{$xs,0.000000,0.000000,1.000000}"
}

$sb = [System.Text.StringBuilder]::new()
function W([string]$s) { [void]$sb.AppendLine($s) }

# ===========================================================================
#  Channel emitters
# ===========================================================================

# A plain linear 0..255 channel: one attribute, one function, one default.
function Add-Channel {
    param(
        [int]$Offset, [string]$Geometry, [string]$Attribute, [string]$FuncName,
        [int]$Default, [double]$PhysFrom = 0, [double]$PhysTo = 1,
        [string]$Snap = 'No', [string]$Master = 'None', [int]$Highlight = -1
    )
    $hl = if ($Highlight -ge 0) { " Highlight=`"$Highlight/1`"" } else { '' }
    $pf = $PhysFrom.ToString('F6', [System.Globalization.CultureInfo]::InvariantCulture)
    $pt = $PhysTo.ToString('F6',   [System.Globalization.CultureInfo]::InvariantCulture)
    W "          <DMXChannel DMXBreak=`"1`" Offset=`"$Offset`"$hl Geometry=`"$Geometry`">"
    W "            <LogicalChannel Attribute=`"$Attribute`" Snap=`"$Snap`" Master=`"$Master`" MibFade=`"0.000000`" DMXChangeTimeLimit=`"0.000000`">"
    W "              <ChannelFunction Name=`"$FuncName`" Attribute=`"$Attribute`" OriginalAttribute=`"`" DMXFrom=`"0/1`" Default=`"$Default/1`" PhysicalFrom=`"$pf`" PhysicalTo=`"$pt`" RealFade=`"0.000000`" RealAcceleration=`"0.000000`" />"
    W "            </LogicalChannel>"
    W "          </DMXChannel>"
}

# Strobe. Two functions in one logical channel: 0-7 is Open, 8-255 ramps the
# rate. Default sits in the Open function, so the fixture homes still.
# Rates match the firmware: period = 500 - (strobe-8)*460/247 ms, i.e. 2..25 Hz.
function Add-StrobeChannel {
    param([int]$Offset, [string]$Geometry)
    # InitialFunction is spelled out rather than left implicit. Without it the
    # home value is "whatever the importer decides the first function is", and
    # if it lands on Strobe instead of Open the fixture arrives at full strobe
    # the moment it is patched. That has cost an afternoon on this rig before.
    # The reference is <geometry>_<first logical channel attribute>.<logical
    # channel>.<channel function>.
    W "          <DMXChannel DMXBreak=`"1`" Offset=`"$Offset`" InitialFunction=`"${Geometry}_Shutter1.Shutter1.Open`" Geometry=`"$Geometry`">"
    W "            <LogicalChannel Attribute=`"Shutter1`" Snap=`"Yes`" Master=`"None`" MibFade=`"0.000000`" DMXChangeTimeLimit=`"0.000000`">"
    W "              <ChannelFunction Name=`"Open`" Attribute=`"Shutter1`" OriginalAttribute=`"`" DMXFrom=`"0/1`" Default=`"0/1`" PhysicalFrom=`"0.000000`" PhysicalTo=`"0.000000`" RealFade=`"0.000000`" RealAcceleration=`"0.000000`">"
    W "                <ChannelSet Name=`"Open`" DMXFrom=`"0/1`" PhysicalFrom=`"0.000000`" PhysicalTo=`"0.000000`" />"
    W "              </ChannelFunction>"
    W "              <ChannelFunction Name=`"Strobe`" Attribute=`"Shutter1Strobe`" OriginalAttribute=`"`" DMXFrom=`"8/1`" Default=`"8/1`" PhysicalFrom=`"2.000000`" PhysicalTo=`"25.000000`" RealFade=`"0.000000`" RealAcceleration=`"0.000000`">"
    W "                <ChannelSet Name=`"Slow (2 Hz)`" DMXFrom=`"8/1`" PhysicalFrom=`"2.000000`" PhysicalTo=`"2.000000`" />"
    W "                <ChannelSet Name=`"Strobe`" DMXFrom=`"9/1`" PhysicalFrom=`"2.100000`" PhysicalTo=`"24.900000`" />"
    W "                <ChannelSet Name=`"Fast (25 Hz)`" DMXFrom=`"255/1`" PhysicalFrom=`"25.000000`" PhysicalTo=`"25.000000`" />"
    W "              </ChannelFunction>"
    W "            </LogicalChannel>"
    W "          </DMXChannel>"
}

# Effect select. Six even bands of 43, matching (value * 6) >> 8 in the firmware.
function Add-ModeChannel {
    param([int]$Offset, [string]$Geometry)
    $bands = @(
        @{ n = 'Solid';   from = 0   },
        @{ n = 'Breathe'; from = 43  },
        @{ n = 'Rainbow'; from = 86  },
        @{ n = 'Chase';   from = 128 },
        @{ n = 'Comet';   from = 171 },
        @{ n = 'Sparkle'; from = 214 }
    )
    W "          <DMXChannel DMXBreak=`"1`" Offset=`"$Offset`" Geometry=`"$Geometry`">"
    W "            <LogicalChannel Attribute=`"Effects1`" Snap=`"Yes`" Master=`"None`" MibFade=`"0.000000`" DMXChangeTimeLimit=`"0.000000`">"
    W "              <ChannelFunction Name=`"Effect`" Attribute=`"Effects1`" OriginalAttribute=`"`" DMXFrom=`"0/1`" Default=`"0/1`" PhysicalFrom=`"0.000000`" PhysicalTo=`"5.000000`" RealFade=`"0.000000`" RealAcceleration=`"0.000000`">"
    foreach ($b in $bands) {
        $i = [array]::IndexOf($bands, $b)
        W "                <ChannelSet Name=`"$($b.n)`" DMXFrom=`"$($b.from)/1`" PhysicalFrom=`"$i.000000`" PhysicalTo=`"$i.000000`" />"
    }
    W "              </ChannelFunction>"
    W "            </LogicalChannel>"
    W "          </DMXChannel>"
}

# ===========================================================================
#  description.xml
# ===========================================================================
W '<?xml version="1.0" encoding="UTF-8" standalone="no"?>'
W '<GDTF DataVersion="1.1">'
W "  <FixtureType Name=`"$ModelName`" ShortName=`"$ShortName`" LongName=`"$ModelName`" Manufacturer=`"$Manufacturer`" Description=`"DMX512 to WS2811 pixel tape controller. 12 V tape, $PIXELS pixels per 5 m run.`" FixtureTypeID=`"$FixtureTypeID`" RefFT=`"`" CanHaveChildren=`"No`">"

# --- attribute definitions -------------------------------------------------
W '    <AttributeDefinitions>'
W '      <ActivationGroups>'
W '        <ActivationGroup Name="ColorRGB" />'
W '      </ActivationGroups>'
W '      <FeatureGroups>'
W '        <FeatureGroup Name="Dimmer" Pretty="Dimmer"><Feature Name="Dimmer" /></FeatureGroup>'
W '        <FeatureGroup Name="Color" Pretty="Colour"><Feature Name="RGB" /></FeatureGroup>'
W '        <FeatureGroup Name="Beam" Pretty="Beam"><Feature Name="Beam" /></FeatureGroup>'
W '        <FeatureGroup Name="Effects" Pretty="Effects"><Feature Name="Effects" /></FeatureGroup>'
W '      </FeatureGroups>'
W '      <Attributes>'
W '        <Attribute Name="Dimmer" Pretty="Dim" Feature="Dimmer.Dimmer" PhysicalUnit="LuminousIntensity" />'
W '        <Attribute Name="ColorAdd_R" Pretty="R" ActivationGroup="ColorRGB" Feature="Color.RGB" PhysicalUnit="ColorComponent" Color="0.735484,0.264516,21.870000" />'
W '        <Attribute Name="ColorAdd_G" Pretty="G" ActivationGroup="ColorRGB" Feature="Color.RGB" PhysicalUnit="ColorComponent" Color="0.197551,0.723502,72.690000" />'
W '        <Attribute Name="ColorAdd_B" Pretty="B" ActivationGroup="ColorRGB" Feature="Color.RGB" PhysicalUnit="ColorComponent" Color="0.166667,0.008900,5.440000" />'
# Custom, but declared - GDTF allows this and it beats the alternatives. Putting
# the background colour on a child geometry would make ZerOS show it as
# "Red 2 / Green 2 / Blue 2"; a named attribute shows "BG Red" on the desk.
W '        <Attribute Name="BGColorAdd_R" Pretty="BG R" Feature="Color.RGB" PhysicalUnit="ColorComponent" Color="0.735484,0.264516,21.870000" />'
W '        <Attribute Name="BGColorAdd_G" Pretty="BG G" Feature="Color.RGB" PhysicalUnit="ColorComponent" Color="0.197551,0.723502,72.690000" />'
W '        <Attribute Name="BGColorAdd_B" Pretty="BG B" Feature="Color.RGB" PhysicalUnit="ColorComponent" Color="0.166667,0.008900,5.440000" />'
# The 9ch spectrum interpolates seven stops - red, yellow, green, cyan, blue,
# magenta, white - with the last leg desaturating. It is a colour ramp rather
# than a wheel of discrete gels, so it is not ColorMacro and not CTO. It gets
# its own attribute.
W '        <Attribute Name="ColorSpectrum" Pretty="Spectrum" Feature="Color.RGB" PhysicalUnit="None" />'
W '        <Attribute Name="ColorIntensity" Pretty="Col Int" Feature="Color.RGB" PhysicalUnit="None" />'
W '        <Attribute Name="BGColorSpectrum" Pretty="BG Spectrum" Feature="Color.RGB" PhysicalUnit="None" />'
W '        <Attribute Name="BGColorIntensity" Pretty="BG Int" Feature="Color.RGB" PhysicalUnit="None" />'
W '        <Attribute Name="Shutter1" Pretty="Shutter" Feature="Beam.Beam" PhysicalUnit="None" />'
W '        <Attribute Name="Shutter1Strobe" Pretty="Strobe" MainAttribute="Shutter1" Feature="Beam.Beam" PhysicalUnit="Frequency" />'
W '        <Attribute Name="Effects1" Pretty="Mode" Feature="Effects.Effects" PhysicalUnit="None" />'
W '        <Attribute Name="Effects1Rate" Pretty="Speed" MainAttribute="Effects1" Feature="Effects.Effects" PhysicalUnit="None" />'
W '        <Attribute Name="Effects1Size" Pretty="Size" MainAttribute="Effects1" Feature="Effects.Effects" PhysicalUnit="None" />'
W '      </Attributes>'
W '    </AttributeDefinitions>'

W '    <Wheels />'
W '    <PhysicalDescriptions />'

# --- models ----------------------------------------------------------------
W '    <Models>'
W '      <Model Name="Body" Length="5.000000" Width="0.012000" Height="0.003000" PrimitiveType="Cube" />'
$pitch = (5.0 / $PIXELS).ToString('F6', [System.Globalization.CultureInfo]::InvariantCulture)
W "      <Model Name=`"Pixel`" Length=`"$pitch`" Width=`"0.012000`" Height=`"0.003000`" PrimitiveType=`"Cube`" />"
W '    </Models>'

# --- geometry --------------------------------------------------------------
#  Ordinary child Beams, one per pixel. Not GeometryReference: see the header.
W '    <Geometries>'
W "      <Geometry Name=`"Body`" Model=`"Body`" Position=`"$IDENTITY`">"
for ($i = 0; $i -lt $PIXELS; $i++) {
    $n = $i + 1
    $mtx = Get-PixelMatrix $i
    W "        <Beam Name=`"Pixel$n`" Model=`"Pixel`" Position=`"$mtx`" LampType=`"LED`" PowerConsumption=`"0.720000`" LuminousFlux=`"18.000000`" ColorTemperature=`"6500.000000`" BeamAngle=`"120.000000`" FieldAngle=`"120.000000`" BeamRadius=`"0.006000`" BeamType=`"Wash`" ColorRenderingIndex=`"80`" />"
}
W '      </Geometry>'
W '    </Geometries>'

# ===========================================================================
#  DMX modes
# ===========================================================================
W '    <DMXModes>'

# ---- 3ch: no master, so RGB homes at 0 or the rig comes up full white -----
W '      <DMXMode Name="3ch RGB" Geometry="Body">'
W '        <DMXChannels>'
Add-Channel -Offset 1 -Geometry 'Body' -Attribute 'ColorAdd_R' -FuncName 'Red'   -Default 0 -Highlight 255
Add-Channel -Offset 2 -Geometry 'Body' -Attribute 'ColorAdd_G' -FuncName 'Green' -Default 0 -Highlight 255
Add-Channel -Offset 3 -Geometry 'Body' -Attribute 'ColorAdd_B' -FuncName 'Blue'  -Default 0 -Highlight 255
W '        </DMXChannels>'
W '        <Relations /><FTMacros />'
W '      </DMXMode>'

# ---- 5ch ------------------------------------------------------------------
W '      <DMXMode Name="5ch Dim + RGB" Geometry="Body">'
W '        <DMXChannels>'
Add-Channel -Offset 1 -Geometry 'Body' -Attribute 'Dimmer' -FuncName 'Dimmer' -Default 0 -Master 'Grand' -Highlight 255
Add-Channel -Offset 2 -Geometry 'Body' -Attribute 'ColorAdd_R' -FuncName 'Red'   -Default 255
Add-Channel -Offset 3 -Geometry 'Body' -Attribute 'ColorAdd_G' -FuncName 'Green' -Default 255
Add-Channel -Offset 4 -Geometry 'Body' -Attribute 'ColorAdd_B' -FuncName 'Blue'  -Default 255
Add-StrobeChannel -Offset 5 -Geometry 'Body'
W '        </DMXChannels>'
W '        <Relations /><FTMacros />'
W '      </DMXMode>'

# ---- 9ch ------------------------------------------------------------------
W '      <DMXMode Name="9ch Spectrum" Geometry="Body">'
W '        <DMXChannels>'
Add-Channel -Offset 1 -Geometry 'Body' -Attribute 'Dimmer' -FuncName 'Dimmer' -Default 0 -Master 'Grand' -Highlight 255
Add-Channel -Offset 2 -Geometry 'Body' -Attribute 'ColorSpectrum'    -FuncName 'Spectrum'    -Default 0
Add-Channel -Offset 3 -Geometry 'Body' -Attribute 'ColorIntensity'   -FuncName 'Intensity'   -Default 255
Add-Channel -Offset 4 -Geometry 'Body' -Attribute 'BGColorSpectrum'  -FuncName 'BG Spectrum' -Default 0
Add-Channel -Offset 5 -Geometry 'Body' -Attribute 'BGColorIntensity' -FuncName 'BG Intensity' -Default 0
Add-ModeChannel -Offset 6 -Geometry 'Body'
Add-Channel -Offset 7 -Geometry 'Body' -Attribute 'Effects1Rate' -FuncName 'Speed' -Default 128
Add-Channel -Offset 8 -Geometry 'Body' -Attribute 'Effects1Size' -FuncName 'Size'  -Default 128
Add-StrobeChannel -Offset 9 -Geometry 'Body'
W '        </DMXChannels>'
W '        <Relations /><FTMacros />'
W '      </DMXMode>'

# ---- 11ch -----------------------------------------------------------------
W '      <DMXMode Name="11ch RGB + BG" Geometry="Body">'
W '        <DMXChannels>'
Add-Channel -Offset 1 -Geometry 'Body' -Attribute 'Dimmer' -FuncName 'Dimmer' -Default 0 -Master 'Grand' -Highlight 255
Add-Channel -Offset 2 -Geometry 'Body' -Attribute 'ColorAdd_R' -FuncName 'Red'   -Default 255
Add-Channel -Offset 3 -Geometry 'Body' -Attribute 'ColorAdd_G' -FuncName 'Green' -Default 255
Add-Channel -Offset 4 -Geometry 'Body' -Attribute 'ColorAdd_B' -FuncName 'Blue'  -Default 255
Add-Channel -Offset 5 -Geometry 'Body' -Attribute 'BGColorAdd_R' -FuncName 'BG Red'   -Default 0
Add-Channel -Offset 6 -Geometry 'Body' -Attribute 'BGColorAdd_G' -FuncName 'BG Green' -Default 0
Add-Channel -Offset 7 -Geometry 'Body' -Attribute 'BGColorAdd_B' -FuncName 'BG Blue'  -Default 0
Add-ModeChannel -Offset 8 -Geometry 'Body'
Add-Channel -Offset  9 -Geometry 'Body' -Attribute 'Effects1Rate' -FuncName 'Speed' -Default 128
Add-Channel -Offset 10 -Geometry 'Body' -Attribute 'Effects1Size' -FuncName 'Size'  -Default 128
Add-StrobeChannel -Offset 11 -Geometry 'Body'
W '        </DMXChannels>'
W '        <Relations /><FTMacros />'
W '      </DMXMode>'

# ---- pixel map ------------------------------------------------------------
$total = 1 + $PIXELS * 3
W "      <DMXMode Name=`"${total}ch Pixel Map`" Geometry=`"Body`">"
W '        <DMXChannels>'
Add-Channel -Offset 1 -Geometry 'Body' -Attribute 'Dimmer' -FuncName 'Dimmer' -Default 0 -Master 'Grand' -Highlight 255
for ($i = 0; $i -lt $PIXELS; $i++) {
    $n = $i + 1
    $o = 2 + $i * 3
    Add-Channel -Offset  $o      -Geometry "Pixel$n" -Attribute 'ColorAdd_R' -FuncName 'Red'   -Default 0
    Add-Channel -Offset ($o + 1) -Geometry "Pixel$n" -Attribute 'ColorAdd_G' -FuncName 'Green' -Default 0
    Add-Channel -Offset ($o + 2) -Geometry "Pixel$n" -Attribute 'ColorAdd_B' -FuncName 'Blue'  -Default 0
}
W '        </DMXChannels>'
W '        <Relations /><FTMacros />'
W '      </DMXMode>'

W '    </DMXModes>'

$stamp = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss')
W '    <Revisions>'
W "      <Revision Text=`"$RevisionText`" Date=`"$stamp`" UserID=`"1`" />"
W '    </Revisions>'
W '    <FTPresets />'
W '    <Protocols />'
W '  </FixtureType>'
W '</GDTF>'

# ===========================================================================
#  Write description.xml, then zip it into the .gdtf container
# ===========================================================================
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("gdtf_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$descPath = Join-Path $stage 'description.xml'
# UTF-8 with no BOM: a BOM ahead of the XML declaration upsets strict parsers.
[System.IO.File]::WriteAllText($descPath, $sb.ToString(),
    (New-Object System.Text.UTF8Encoding $false))

# Validate before packaging - a malformed profile is far more annoying to
# diagnose on the desk than here.
try {
    $xml = New-Object System.Xml.XmlDocument
    $xml.Load($descPath)
} catch {
    Write-Error "Generated description.xml is not well-formed XML: $_"
}

$modes = $xml.SelectNodes('//DMXMode')
Write-Host "DMX modes:" -ForegroundColor Cyan
foreach ($mode in $modes) {
    $n = $mode.SelectNodes('.//DMXChannel').Count
    Write-Host ("  {0,-20} {1,4} channels" -f $mode.Name, $n)
}

# Keep an unpacked copy next to the archive - it is the thing worth diffing.
Copy-Item $descPath (Join-Path $OutDir 'description.xml') -Force

$gdtf = Join-Path $OutDir "$Manufacturer@$ModelName@v1.gdtf"
if (Test-Path $gdtf) { Remove-Item $gdtf -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $gdtf,
    [System.IO.Compression.CompressionLevel]::Optimal, $false)

Remove-Item $stage -Recurse -Force

$kb = [math]::Round((Get-Item $gdtf).Length / 1KB, 1)
Write-Host ""
Write-Host "Wrote $gdtf ($kb KB)" -ForegroundColor Green
Write-Host "Wrote $(Join-Path $OutDir 'description.xml')" -ForegroundColor Green
