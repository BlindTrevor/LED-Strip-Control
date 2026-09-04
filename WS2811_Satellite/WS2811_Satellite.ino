/*
 *  WS2811 satellite - ESP32 D1 Mini
 *
 *  I2C slave. Receives parameters from the ESP32-S3 controller and renders its
 *  own segment of the strip. Flash one mini per strip, changing SAT_UNIT (and
 *  the segment settings) each time.
 *
 *  Wiring: SDA/SCL/GND to the controller board's I2C terminal.
 *          LED_PIN -> strip data.  Strip 12 V from the PSU, grounds common.
 *
 *  Libraries: FastLED >= 3.7
 */

#include <FastLED.h>
#include <Wire.h>
#include "Protocol.h"
#include "Effects.h"

// ================== PER-UNIT CONFIG - CHANGE FOR EACH MINI ==================
#define SAT_UNIT     0      // 0, 1 or 2  -> I2C address 0x40, 0x41, 0x42
#define SAT_PIXELS   100    // pixels on THIS satellite's strip
#define SAT_OFFSET   0      // this segment's first index in the global strip
                            //   unit 0: offset 0    unit 1: offset 100  etc.
// ============================================================================

#define LED_PIN      4      // any free GPIO on the mini; 4 is always safe
#define LED_TYPE     WS2811 // try WS2811_400 if output is garbled
#define COLOR_ORDER  BRG    // verified for HD-12V-WS2811-60L-B-IP30
#define MAX_BRIGHT   255
#define FRAME_HZ     100

#define I2C_SDA      21
#define I2C_SCL      22

CRGB    leds[SAT_PIXELS];
uint8_t sparkleBuf[SAT_PIXELS];
uint8_t pixelBuf[SAT_PIXELS * 3];

static volatile ParamsMsg rxParams;
static volatile bool      haveParams = false;
static volatile bool      pixelMode  = false;
static uint8_t            rxBuf[sizeof(PixelMsg)];

// ------------------------------------------------------------ I2C slave ----
// Kept deliberately short: copy and flag, never render in here.
void onI2cReceive(int len)
{
  if (len <= 0 || len > (int)sizeof(rxBuf)) {
    while (Wire.available()) Wire.read();
    return;
  }
  for (int i = 0; i < len; i++) rxBuf[i] = Wire.read();

  if (rxBuf[0] == PROTO_MAGIC_PARAMS && len >= (int)sizeof(ParamsMsg)) {
    memcpy((void *)&rxParams, rxBuf, sizeof(ParamsMsg));
    pixelMode  = false;
    haveParams = true;
  }
  else if (rxBuf[0] == PROTO_MAGIC_PIXELS && len >= 4) {
    PixelMsg *m = (PixelMsg *)rxBuf;
    uint8_t n = m->count;
    if (n <= PIXELS_PER_BLOCK &&
        (uint16_t)m->start + n <= SAT_PIXELS &&
        len >= (int)PIXELMSG_LEN(n)) {
      memcpy(&pixelBuf[(uint16_t)m->start * 3], m->rgb, (uint16_t)n * 3);
    }
    pixelMode  = true;
    haveParams = true;
  }
}

// --------------------------------------------------------------- render ----
// All motion comes from the controller's `phase`, and every position is
// computed in GLOBAL pixel space then offset into this segment. That is what
// makes a chase or comet flow continuously from one strip to the next.
static void render()
{
  ParamsMsg p;
  memcpy(&p, (const void *)&rxParams, sizeof(p));

  //  The effects themselves live in Effects.h, shared byte-identically with
  //  the controller so its Preview tab cannot show a different look from the
  //  one this renders. Global pixel space, written into this satellite's own
  //  segment - that is what keeps a chase continuous across all the strips.
  fxRender(leds, sparkleBuf, SAT_PIXELS, SAT_OFFSET, TOTAL_PIXELS, p,
           pixelMode ? pixelBuf : nullptr);

  FastLED.setBrightness(scale8(MAX_BRIGHT, p.master));
  FastLED.show();
}

// -------------------------------------------------------------- arduino ----
void setup()
{
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, SAT_PIXELS);
  FastLED.setBrightness(MAX_BRIGHT);
  FastLED.setDither(DISABLE_DITHER);
  FastLED.clear(true);

  Wire.begin((uint8_t)(SAT_I2C_BASE + SAT_UNIT), I2C_SDA, I2C_SCL, 400000);
  Wire.onReceive(onI2cReceive);
}

void loop()
{
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < (1000 / FRAME_HZ)) return;
  lastFrame = now;

  if (haveParams) render();   // hold the last look if the link goes quiet
}
