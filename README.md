# DMX512 → WS2811 pixel controller

A two-board DMX fixture for 12 V WS2811 pixel tape, driven from a Zero 88 FLX.

An **ESP32-S3 touchscreen board** receives DMX and runs the UI; **ESP32 D1 Mini
satellites** render the pixels. The split exists for a hard reason: the S3
board's 4.3" RGB parallel LCD consumes essentially every usable GPIO, leaving
nothing that can drive a WS2811 line.

```
FLX ──DMX/RS485──► ESP32-S3-Touch-LCD-4.3B ──I2C──┬──► D1 Mini ──► strip 1
                   (DMX decode, UI, clock)        ├──► D1 Mini ──► strip 2
                                                  └──► D1 Mini ──► strip 3
```

## Hardware

| Item | Detail |
|---|---|
| Strip | [BTF-LIGHTING WS2811 12 V, 5 m, 60 LED/m](https://www.amazon.co.uk/BTF-LIGHTING-300LEDs-Addressable-Flexible-Non-waterproof/dp/B01CNL6K52) — marked `HD-12V-WS2811-60L-B-IP30` |
| Pixels | **100 per 5 m** — 12 V WS2811 drives 3 LEDs per IC, so 60 LED/m = 20 pixels/m |
| Colour order | **BRG** (verified on the bench; `RGB` gives swapped colours) |
| Controller | [Waveshare ESP32-S3-Touch-LCD-4.3B](https://www.amazon.co.uk/dp/B0DCNSRT31) — 800×480, GT911 touch, CH422G expander, 16 MB flash, 8 MB OPI PSRAM |
| Satellites | [ESP32 D1 Mini, 3-pack](https://www.amazon.co.uk/dp/B0CJNMRG37) — CH9102F USB bridge |
| PSU | 12 V, ≥8 A (5 m draws ~6 A at full white). Not included above. |

### Controller GPIO allocation

Almost everything is taken. This is why the satellites exist.

| Function | GPIO |
|---|---|
| RGB LCD | 0, 1, 2, 3, 5, 7, 10, 14, 17, 18, 21, 38–42, 45–48 |
| Touch | 4 (INT), 8 (SDA), 9 (SCL) |
| microSD | 11, 12, 13 |
| USB | 19, 20 |
| CAN | 15, 16 |
| **RS485 (DMX)** | **43 (RX), 44 (TX)** |
| Flash / octal PSRAM | 26–37 |

GPIO6 is the only unallocated pin, and likely reaches no pad.

> The strip is advertised as "300LEDs 100pixels". Both numbers are correct and
> mean different things: 300 physical LEDs, wired three per WS2811 IC, giving
> **100 addressable pixels**. Every count in this project is pixels.

### Wiring

```
12 V PSU (+) ──┬──► strip +12 V
               └──► S3 terminal VIN
12 V PSU (−) ──┬──► strip GND
               └──► S3 terminal GND

DMX XLR: pin 3 → A,  pin 2 → B,  pin 1 → GND terminal

S3 terminal SDA ──► mini GPIO21
S3 terminal SCL ──► mini GPIO22
S3 terminal GND ──► mini GND          (essential)

mini GPIO4 ──► strip DATA
```

The minis drive the strips **directly from 3.3 V** — no level shifter, by
design decision. A 12 V WS2811 wants ~3.5 V for a logic high, so this is
marginal; a 74AHCT125 on 5 V is the fix if it proves unreliable.

## DMX personalities

Set the address and mode from the touchscreen; both persist to NVS.

**3ch** — `1` Red · `2` Green · `3` Blue

**5ch** — `1` Master · `2` Red · `3` Green · `4` Blue · `5` Strobe

**9ch** — `1` Master · `2` Colour spectrum · `3` FG intensity ·
`4` BG spectrum · `5` BG intensity · `6` Mode · `7` Speed · `8` Size · `9` Strobe

**11ch** — `1` Master · `2–4` RGB · `5–7` BG RGB · `8` Mode · `9` Speed ·
`10` Size · `11` Strobe

**Pixel map** — `1` Master, then R,G,B per pixel. 301 channels for one
satellite; scales with `TOTAL_PIXELS`.

### Mode channel

Divides 0–255 evenly across six effects:

| Value | Effect | Uses BG? |
|---|---|---|
| 0–42 | Solid | – |
| 43–85 | Breathe | – |
| 86–127 | Rainbow | – |
| 128–170 | Chase | yes |
| 171–213 | Comet | yes |
| 214–255 | Sparkle | yes |

### Colour spectrum channel (9ch only)

Channels `2` (foreground) and `4` (background) select a colour from a single
value, smoothly interpolated between seven evenly spaced stops:

| DMX | Colour |
|---:|---|
| 0 | Red |
| 43 | Yellow |
| 85 | Green |
| 128 | Cyan |
| 170 | Blue |
| 213 | Magenta |
| 255 | **White** |

Stops fall every 255 ÷ 6 = 42.5, so the values above are ±1. Anything between
two stops is a smooth blend, which means fades through the spectrum work
properly rather than jumping.

The final leg desaturates from magenta to white, which is what brings white
within reach of one channel. So **white sparkle over a blue background works in
9ch**: `2` = 255 (white), `3` = 255, `4` = 170 (blue), `5` = 40, `6` = 230.

What a single channel can't give you is white *and* a chosen hue independently —
for that, `11ch` sets both colours as explicit RGB.

## Build

Arduino IDE 2.x with the **esp32 by Espressif Systems** core, pinned to
**2.0.17**. Libraries: **FastLED** (satellite), **esp_dmx** ≥ 4.1 and
**LVGL 8.3.x** (controller).

> **Do not use core 3.x.** esp_dmx 4.1.0 is the newest release and supports
> Arduino-ESP32 2.0.3+ / ESP-IDF 4.4.1+ only. On core 3.x, ESP-IDF 5 has
> removed `module` from `uart_signal_conn_t` and `dmx/hal/uart.c` fails to
> compile. 2.0.17 is the last of the 2.x line.

LVGL is optional, via `UI_HAS_LVGL` in `display.h`. It defaults to `1`; set it
to `0` to build on a machine without the library and the firmware still builds
and runs, just headless. So you can bring up DMX and the strips without
touching the display at all.

This used to be automatic and silently was not. `__has_include(<lvgl.h>)`
cannot work under the Arduino builder: libraries are discovered by scanning
sources for `#include` directives, and their include paths are added only
afterwards, so `<lvgl.h>` is unreachable at the moment `__has_include` is
evaluated. It always answered "no", and the controller built headless with LVGL
correctly installed — a green build in which not one line of `ui.cpp` had ever
been compiled. If the controller comes out around 315 KB rather than ~453 KB,
that is what has happened.

With LVGL installed, copy `lv_conf_template.h` out of the library folder to
`lv_conf.h` **beside** it (not inside), set `#define LV_CONF_INCLUDE_SIMPLE 1`
at the top, and:

| Setting | Value | Why |
|---|---|---|
| `LV_COLOR_DEPTH` | `16` | RGB565, what the panel wants |
| `LV_FONT_MONTSERRAT_22` | `1` | tab labels |
| `LV_FONT_MONTSERRAT_28` | `1` | the big address / source readouts |
| `LV_MEM_CUSTOM` | `1` | let it use the 8 MB PSRAM |

The two fonts are optional — without them the UI still lays out, just smaller.

Flash the satellite first, so there is something listening.

### Satellite — `WS2811_Satellite`

| Setting | Value |
|---|---|
| Board | WEMOS D1 MINI ESP32 |
| Flash Size | 4 MB |
| PSRAM | Disabled |

### Controller — `WS2811_DMX_ESP32`

> **Unplug the DMX cable before uploading.** The flashing UART is GPIO43/44 —
> the same pins as the RS485 transceiver.

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** |
| Flash Size | 16 MB |
| PSRAM | OPI PSRAM |
| Partition Scheme | Huge APP (3 MB No OTA) |

`USB CDC On Boot: Enabled` routes `Serial` out the native USB peripheral rather
than UART0. This matters: UART0's pins *are* the RS485 transceiver, so printing
to UART0 injects console text onto the DMX bus. If the monitor shows nothing,
this board's USB-C is on the CH343P — set it Disabled and only monitor with DMX
unplugged.

## Adding a satellite

1. In the satellite sketch set `SAT_UNIT` (→ I²C address `0x30 + unit`),
   `SAT_PIXELS` and `SAT_OFFSET` (its first index in the global strip).
2. In the controller's `config.h`, uncomment the matching `SATS[]` row.
3. Raise `TOTAL_PIXELS` in **both** copies of `Protocol.h` to the sum.

## Touchscreen UI

Three tabs on the 800×480 panel:

| Tab | Does |
|---|---|
| **Setup** | DMX start address (−10/−1/+1/+10, with the occupied channel range shown), channel mode, and an *Ignore DMX* switch for standalone use. **Save** writes to NVS. |
| **Manual** | What standalone mode plays: master, effect, speed, size, strobe, and foreground / background RGB with live swatches. Writes straight into `manual`. |
| **Status** | Read-only: DMX live or not, frames received, `i2cErr`, the patch, and the look actually going out. |

Changing the channel mode re-clamps the address — going from 3ch to Pixel Map
would otherwise leave a perfectly good address 290 channels past the end of the
universe.

### Bringing the display up

`ui.cpp` is complete and contains **no hardware knowledge**. Everything about
the RGB panel, the GT911 and the CH422G sits behind three hooks in
`display.cpp`:

```
panelInit()    bring up panel + backlight + touch
panelFlush()   blit an RGB565 rectangle
panelTouch()   return true and a coordinate while a finger is down
```

The LVGL side of it — PSRAM draw buffers, display and input driver
registration, the 1 ms tick — is already written. To light the screen up:

1. Get any one stack drawing and reporting touch: `ESP32_Display_Panel`,
   `Arduino_GFX` + LVGL, or Waveshare's own demo.
2. Paste that into the three hooks.
3. Set `DISPLAY_BRINGUP_READY` to `1` in `display.cpp`.

Until then `displayBegin()` returns false and `uiTask()` prints the serial
status line instead, so the DMX and I2C paths never depend on the screen.

> **`panelInit()` must not call `Wire.begin()`.** `setup()` has already opened
> GPIO8/9 for the satellites; re-opening it resets the bus and drops them.

## Validating the I²C link

ESP32 Arduino **I²C slave** mode is the least proven part of this design, so
prove it before trusting it. `i2cErr` counts `Wire.endTransmission()` failures
on the controller — every one is a parameter frame the satellite never saw.

Do this headless, before the display is in the picture, so there is only one
variable:

1. One mini powered, flashed, SDA/SCL/**GND** to the terminal block. Watch the
   controller's serial line: `i2cErr` must stay at **0** while `frames` climbs.
2. Leave it running for ten minutes. A slave that works for thirty seconds and
   then wedges is the classic failure — a hung mini holds SDA low, so the
   symptom is `i2cErr` climbing by ~100/second and never recovering.
3. Switch to **Pixel Map** and repeat. This is the hard case: ~390 bytes per
   frame in four blocks instead of 14 bytes, and the satellite is inside
   `FastLED.show()` for part of each frame. If errors appear only here, the
   send rate is outrunning the slave — drop `SEND_HZ_PIXELS`.
4. Only then bring the display up, and check `i2cErr` again. The GT911 is
   polled on the same bus from core 1 while the link task drives it at 100 Hz
   from core 0; errors that appear *only* after the screen works are that
   contention, not the slave.

If step 1 or 2 fails, the slave implementation is the problem and no amount of
tuning the controller will fix it.

## Fixture profile

`gdtf/` holds the GDTF profile — **Sonic Lighting / LED Strip**, one fixture
type with a DMX mode per channel mode above.

```
pwsh tools/generate-gdtf.ps1     # rebuild it
pwsh tools/validate-gdtf.ps1     # check it before it reaches the desk
```

The generator reads `TOTAL_PIXELS` out of `Protocol.h`, so the pixel-map mode
cannot drift from the firmware — raise the pixel count and re-run.

Two things are deliberate:

- **Strobe homes to 0.** Every strobe channel's first function is a plain
  *Open* spanning 0–7 with `Default="0/1"`, and `InitialFunction` is spelled
  out rather than left for the importer to guess. A full-strobe default has
  cost an afternoon on this rig before.
- **No `GeometryReference`.** ZerOS does not build cells from one — a valid
  100-cell reference imports as a single flat fixture with numbered parameters
  (`Red 2`, `Red 3`, …). The pixel-map mode uses 100 ordinary child `Beam`
  geometries, which flattens the same way without pretending otherwise. Real
  multicell needs a native profile from Zero 88.

> `.zfix` is a `Z88C` binary container whose payload resisted deflate, zlib,
> gzip and brotli at every offset. Don't try to author or decode one.

## Design notes

**The controller owns the animation clock.** It sends a global `phase` in every
message rather than letting each satellite keep its own, which is what makes a
chase or comet run *continuously across all strips* instead of as several
independent animations. Satellites compute positions in global pixel space then
offset into their own segment.

**Parameters, not pixels.** `ParamsMsg` is 14 bytes, so a dropped message is
invisible — the satellite just holds its last look. Pixel-map mode is the
exception at ~390 bytes per frame, throttled to 40 Hz.

**Strobe is central**, folded into `master` before sending so all strips blink
together. Resolution is bounded by the 100 Hz send rate: crisp to ~10 Hz, coarse
above. Sparkle is deliberately local, since independent twinkling per strip is
what you want.

**Core pinning.** The link task runs on core 0 so an 800×480 LVGL redraw on
core 1 can never stall the output.

## Known issues

- **The display has never been brought up.** `ui.cpp` is written and complete,
  but the three hooks in `display.cpp` are empty and `DISPLAY_BRINGUP_READY` is
  `0`, so the firmware runs headless. See *Bringing the display up* above.
- **ESP32 Arduino I²C *slave* mode is the weak link.** Validate it early: the
  Status tab and the serial line both report `i2cErr`, which should stay at 0.
- **The satellites share the I²C bus with the touch controller and CH422G.**
  A mini that hangs holds SDA low and takes the touchscreen with it. If the UI
  dies, suspect a mini first. Once the panel is up, the GT911 is also being
  polled on that bus from core 1 while the link task drives it at 100 Hz from
  core 0 — if `i2cErr` only starts climbing after the screen works, that
  contention is the first place to look.
- **`Protocol.h` is duplicated** in both sketch folders and must stay
  byte-identical — Arduino IDE 2.x only compiles headers inside the sketch
  folder. Drift is silent and nasty: the structs stop agreeing, the satellites
  decode garbage, and the I²C writes still succeed so nothing reports an error.

  ```
  pwsh tools/check-protocol-sync.ps1                # compare
  pwsh tools/check-protocol-sync.ps1 -Fix           # controller -> satellite
  pwsh tools/check-protocol-sync.ps1 -InstallHook   # block commits that drift
  ```

  CI runs the same check on any push that touches either copy. Converting to
  PlatformIO (two environments plus `lib/Protocol/`) would remove the
  duplication properly, and is still worth doing once the display is working —
  doing both at once means a failed build could be either.