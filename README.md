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

> **A level shifter is required. This is not optional.** The minis were
> designed to drive the strips directly from 3.3 V; on the bench that does not
> work. A 12 V WS2811 wants ~3.5 V for a logic high and 3.3 V sits just under
> it, so the chips never latch a frame and hold their power-up state — the
> strip lights **solid white and ignores everything**. Fit a **74AHCT125** (or
> 74HCT245 / 74HCT04) on 5 V: the HCT input reads 3.3 V as a valid high and
> re-transmits at 5 V.
>
> ```
> mini GPIO4 ──[ 470R ]──► 74AHCT125 1A (pin 2)
>                          74AHCT125 1Y (pin 3) ──► strip DATA
>                          /1OE (pin 1) ──► GND
>                          VCC  (pin 14) ──► 5 V
>                          GND  (pin 7)  ──► common ground
> ```
>
> Do not substitute a BSS138-style bidirectional module — too slow for 800 kHz.
> A single transistor is also wrong: one stage inverts, and inverted WS2811
> data is noise.
>
> The diagnosis is not a guess. The same strip, at the same `WS2811` / `BRG` /
> 100-pixel settings, works correctly from an Arduino Mega — which drives 5 V.
> Driven from a mini the GPIO measures a healthy 0.4 V average under continuous
> refresh, three separate GPIOs behave identically, grounds are bonded and the
> strip has its 12 V. Logic level is the only variable left.

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

1. In the satellite sketch set `SAT_UNIT` (→ I²C address `0x40 + unit`),
   `SAT_PIXELS` and `SAT_OFFSET` (its first index in the global strip).
2. In the controller's `config.h`, uncomment the matching `SATS[]` row.
3. Raise `TOTAL_PIXELS` in **both** copies of `Protocol.h` to the sum.

## Touchscreen UI

Four tabs on the 800×480 panel:

| Tab | Does |
|---|---|
| **Setup** | DMX start address (−10/−1/+1/+10, with the occupied channel range shown), channel mode, and an *Ignore DMX* switch for standalone use. **Save** writes to NVS. |
| **Manual** | What standalone mode plays: master, effect, speed, size, strobe, and foreground / background RGB with live swatches. Writes straight into `manual`. |
| **Preview** | The look the tape would be showing, with no tape attached. |
| **Status** | Read-only: DMX live or not, frames received, `i2cErr`, the patch, and the look actually going out. |

Preview renders from the exact `ParamsMsg` last put on the wire — captured in
`sendParams()`, not re-derived — so it shows what the satellites were actually
told, strobe included. Pixel Map mode uses the real per-pixel DMX values.

**It cannot drift from the tape.** Both the satellite's renderer and this tab
call `fxRender()` in `Effects.h`, which is shared byte-identically between the
sketch folders and guarded by the same check as `Protocol.h`. The satellite
passes its own segment (`SAT_OFFSET`, `SAT_PIXELS`); the preview passes the
whole strip (offset 0, `TOTAL_PIXELS`). Every effect is computed in global
pixel space either way, which is also what makes a chase run continuously
across several strips instead of restarting on each.

One thing it still cannot match exactly, by design: sparkle placement is local
to each satellite so the strips twinkle independently. Its density and colour
are right; the individual pixels are representative.

The controller links FastLED for this, but only for its arithmetic — `sin8`,
`fill_rainbow`, `blend`, `nscale8`, `qsub8`. No `addLeds()`, no `show()`, no
RMT channel, no pin. Those curves are specific approximations, and a preview
drawn with merely similar ones would be a confident lie.

`master` is deliberately outside `fxRender()`: the satellite applies it through
`FastLED.setBrightness()` at `show()` time, and the preview — which has no
`show()` — scales the buffer instead. Same result, different mechanism.

Changing the channel mode re-clamps the address — going from 3ch to Pixel Map
would otherwise leave a perfectly good address 290 channels past the end of the
universe.

### The display

Brought up and working: panel, backlight and GT911 touch. `ui.cpp` still
contains **no hardware knowledge** — everything about the RGB panel, the GT911
and the CH422G lives behind three hooks in `display.cpp`, driven straight from
ESP-IDF's `esp_lcd_panel_rgb` with hand-written CH422G and GT911 code. No
display library is needed.

```
panelInit()    bring up panel + backlight + touch
panelFlush()   blit an RGB565 rectangle
panelTouch()   return true and a coordinate while a finger is down
```

`DISPLAY_BRINGUP_READY` is `1`. Set it to `0` to force the headless build back,
in which `displayBegin()` returns false and `uiTask()` prints the serial status
line instead. `DISPLAY_TOUCH_DEBUG 1` adds per-press and raw-register tracing.

Panel timings are 800×480, hfp 40 / hpw 48 / hbp 88, vfp 13 / vpw 3 / vbp 32,
16 MHz pixel clock, `pclk_active_neg`, framebuffer in PSRAM.

> **`panelInit()` must not call `Wire.begin()`.** `setup()` has already opened
> GPIO8/9 for the satellites; re-opening it resets the bus and drops them.

Four traps on this board, all of which cost bench time:

- **EXIO3 is the panel reset.** Leave it low and the panel initialises
  perfectly, reports no error, and stays completely dark.
- **EXIO2 is backlight *and* the panel's DISP line.** It is on/off only. PWM
  there drops DISP too and puts the panel into standby.
- **The GT911's point registers start at `0x8150` with the X low byte.** The
  track id is at `0x814F`, *before* that block — there is no id byte to skip.
  Skipping one anyway shifts every read and turns X into garbage that fails a
  range check, which looks exactly like a dead touch panel.
- **The GT911 raises its status bit only when something changes**, and it
  reports the lift as well as the press. "No new data" means "unchanged", not
  "released". Report released on every quiet poll and no tap ever lands.

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

- **The strip needs a 5 V level shifter and does not work without one.** See
  *Wiring* above. Until it is fitted the strip sits solid white.
- **The RGB panel has no bandwidth to spare, and the UI is tuned around that.**
  The peripheral streams the whole framebuffer out of PSRAM continuously —
  ~24 MB/s that can never be late. Anything else touching PSRAM competes, and
  when the LCD's FIFO loses the race the image jumps sideways or vertically.
  This core cannot fix that properly: bounce buffers, which stage lines in
  internal SRAM, are an ESP-IDF 5 feature and so need core 3.x, which
  `esp_dmx` rules out. So it is managed instead, and all four of these matter:

  | Measure | Why |
  |---|---|
  | LVGL draw buffers in **internal SRAM**, not PSRAM | rendering leaves PSRAM entirely; the flush becomes a one-way copy |
  | `BUF_LINES_INT` = **8** | each flush's cache writeback holds the PSRAM bus half as long. This was the one that actually fixed it |
  | Preview canvas in **internal SRAM**, and only rendered while its tab is showing | a tabview scrolls inactive pages out of view rather than hiding them, so a `LV_OBJ_FLAG_HIDDEN` check silently never fires |
  | Update only what changed | `lv_label_set_text()` invalidates whether or not the text differs |

  Raising the draw-buffer size, adding another animating widget, or moving a
  frequently-written buffer to PSRAM will bring the jumping straight back. If
  it ever needs more headroom, dropping `pclk_hz` from 16 MHz costs refresh
  rate (~31 Hz at 16 MHz) but reduces the DMA's demand directly.
- **The CH422G answers to 24 I²C addresses**, not one: the whole of `0x20-0x27`
  and `0x30-0x3F`. Verified by a bus scan on the hardware. This is why
  `SAT_I2C_BASE` is `0x40` and must stay clear of that block. With satellites
  at `0x30..0x32` every parameter frame was latched by the expander as well as
  by the mini, clobbering the register that drives backlight, DISP and both
  reset lines — the screen came up and went dark on the first frame. Worse,
  the expander ACKs those addresses whether or not a mini is present, so
  `i2cErr` read as healthy and proved nothing at all.
- **ESP32 Arduino I²C *slave* mode is the weak link.** Validate it early: the
  Status tab and the serial line both report `i2cErr`, which should stay at 0.
  That figure is only meaningful now the satellites are outside the CH422G's
  address block.
- **A satellite losing power looks exactly like the controller dying.** An
  unpowered ESP32 still has ESD diodes from every GPIO to a rail now sitting at
  0 V, so the bus pull-ups push current through the mini's SDA/SCL pins and
  clamp both lines at roughly 0.6 V. The whole bus reads low. The GT911 is on
  those same two wires and is polled from core 1 every frame, so every poll
  costs the full `Wire` timeout and the UI starves — the screen goes
  unresponsive and the S3 looks dead, when what actually happened is that a
  mini lost power.

  Toggling SCL to recover the bus does not help: nothing is *driving* the line
  low, a diode is clamping it. So the firmware backs off instead — `Wire`
  timeout cut to 15 ms, and the GT911 poll drops to twice a second after four
  consecutive failures. The screen keeps drawing, `i2cErr` keeps climbing on
  the Status tab, and touch returns by itself when power does.

  The real fix is electrical, and it is already in *Wiring*: run both boards
  from the 12 V PSU so neither can be up while the other is down. This mostly
  bites on the bench, with the mini USB-powered. To harden it against a
  satellite genuinely failing on a rig, an I²C buffer such as a PCA9517 between
  the S3's bus and the satellite terminal would isolate the segments so a dead
  mini cannot pull the controller's bus down.
- **The satellites share the I²C bus with the touch controller and CH422G.**
  A mini that hangs holds SDA low and takes the touchscreen with it. If the UI
  dies, suspect a mini first. Once the panel is up, the GT911 is also being
  polled on that bus from core 1 while the link task drives it at 100 Hz from
  core 0 — if `i2cErr` only starts climbing after the screen works, that
  contention is the first place to look.
- **`Protocol.h` and `Effects.h` are duplicated** in both sketch folders and
  must stay byte-identical — Arduino IDE 2.x only compiles headers inside the
  sketch folder. Drift in either is silent and nasty. `Protocol.h`: the structs
  stop agreeing, the satellites decode garbage, and the I²C writes still succeed
  so nothing reports an error. `Effects.h`: the Preview tab confidently shows a
  different look from the one the tape renders.

  ```
  pwsh tools/check-protocol-sync.ps1                # compare
  pwsh tools/check-protocol-sync.ps1 -Fix           # controller -> satellite
  pwsh tools/check-protocol-sync.ps1 -InstallHook   # block commits that drift
  ```

  CI runs the same check on any push that touches any of the four copies.
  Converting to PlatformIO (two environments plus a shared `lib/` folder) would
  remove the duplication properly. The display is working now, so that argument
  for deferring it has gone — but note the level-shifter work is still open, and
  doing both at once means a failed build could be either.