#pragma once
#include <stdint.h>
#include "Protocol.h"

// ===========================================================================
//  ESP32-S3-Touch-LCD-4.3B  -  DMX receiver, touchscreen UI, and I2C master
//
//  This board has NO free GPIO for pixel data - the 4.3" RGB LCD consumes
//  almost every pin and the board needs a CH422G expander just for backlight.
//  So it never touches a strip. It decodes DMX, owns the animation clock, and
//  pushes parameters to D1 Mini satellites which do the rendering.
// ===========================================================================

// ----------------------------------------------------------------- DMX ----
//  UART1 with pins remapped to the on-board RS485 transceiver.
//  UART0's default pins ARE those same pins, so do not print to UART0 - set
//  "USB CDC On Boot: Enabled" and Serial goes out native USB instead.
#define DMX_UART_NUM   1
#define DMX_RX_PIN     43
#define DMX_TX_PIN     44
#define DMX_RTS_PIN    -1       // board switches direction automatically
#define DMX_TIMEOUT_MS 3000     // silence before falling back to manual

// ------------------------------------------------- I2C link to satellites ----
//  The board's own I2C bus, already broken out to the 3.5 mm terminal block.
//  GPIO8/9 are shared with the GT911 touch controller and the CH422G expander;
//  the satellites simply join as extra slaves at 0x30..0x32.
//
//  Two rules that matter:
//    - The minis must NOT fit their own pull-ups. This board already has them,
//      and stacking more drops the effective resistance until the bus fails.
//    - A satellite that hangs holds SDA low and takes the touchscreen with it,
//      because they share this bus. If the UI dies, suspect a mini first.
#define I2C_SDA        8
#define I2C_SCL        9
#define I2C_HZ         400000

#define SEND_HZ        100      // parameter modes: 14 bytes, ~0.4 ms a frame
#define SEND_HZ_PIXELS 40       // pixel map: ~390 bytes a frame, ~10 ms

// --------------------------------------------------------- satellite map ----
//  Must agree with SAT_PIXELS / SAT_OFFSET compiled into each mini, and the
//  offsets must sum to TOTAL_PIXELS in Protocol.h. Uncomment as you add units;
//  the 301-channel footprint follows TOTAL_PIXELS automatically.
struct SatSeg { uint8_t addr; uint8_t offset; uint8_t pixels; };
static const SatSeg SATS[] = {
  { SAT_I2C_BASE + 0,   0, 100 },
//{ SAT_I2C_BASE + 1, 100, 100 },
//{ SAT_I2C_BASE + 2, 200, 100 },
};
static const uint8_t SAT_COUNT = sizeof(SATS) / sizeof(SATS[0]);

// -------------------------------------------------------------- UI text ----
static const char *CM_NAME[CM_COUNT] = {
  "3ch RGB", "5ch Dim+RGB", "9ch Spectrum", "11ch RGB + BG", "Pixel Map"
};
static const char *FX_NAME[FX_COUNT] = {
  "Solid", "Breathe", "Rainbow", "Chase", "Comet", "Sparkle"
};
