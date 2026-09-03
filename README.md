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

`0` = red, `128` = green, `255` = blue — a two-leg ramp, **not** a hue wheel.
There is deliberately no white or magenta in this space, so white-sparkle-over-
blue needs **11ch**.

## Build

Arduino IDE 2.x with the **esp32 by Espressif Systems** core.
Libraries: **FastLED** (satellite) and **esp_dmx** ≥ 4.1 (controller).

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

- **`ui.cpp` is a headless placeholder.** The LVGL touchscreen layer is not
  written yet — the display stack is still to be chosen.
- **ESP32 Arduino I²C *slave* mode is the weak link.** Validate it early: the
  controller's status line reports `i2cErr`, which should stay at 0.
- **The satellites share the I²C bus with the touch controller and CH422G.**
  A mini that hangs holds SDA low and takes the touchscreen with it. If the UI
  dies, suspect a mini first.
- **`Protocol.h` is duplicated** in both sketch folders and must stay
  byte-identical.