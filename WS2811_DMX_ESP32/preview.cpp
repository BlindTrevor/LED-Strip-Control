/*
 *  preview.cpp - the Preview tab's renderer
 *
 *  FastLED is pulled in here for its MATHS ONLY - CRGB, sin8, fill_rainbow,
 *  blend, nscale8, qsub8. No addLeds(), no show(), so no RMT channel is
 *  claimed and no pin is touched; the controller still drives no strip. The
 *  point of using it rather than hand-rolling the arithmetic is exactness:
 *  sin8() and fill_rainbow() are specific approximations, and a preview drawn
 *  with merely similar curves would be a confident, plausible lie.
 *
 *  See the warning in preview.h about keeping this in step with the
 *  satellite's render().
 */

#include <Arduino.h>
#include <FastLED.h>
#include "state.h"
#include "preview.h"

extern ParamsMsg gSentParams;
extern bool      gSentPixels;
extern uint8_t   gSentPixelRgb[TOTAL_PIXELS * 3];

static CRGB    leds[TOTAL_PIXELS];
static uint8_t sparkleBuf[TOTAL_PIXELS];

void previewRender(uint32_t *out)
{
  ParamsMsg p = gSentParams;              // snapshot; a torn read costs a frame
  bool      pixelMode = gSentPixels;

  uint8_t decay = 8 + (p.speed >> 2);
  for (uint16_t i = 0; i < TOTAL_PIXELS; i++)
    sparkleBuf[i] = qsub8(sparkleBuf[i], decay);

  if (pixelMode) {
    for (uint16_t i = 0; i < TOTAL_PIXELS; i++)
      leds[i] = CRGB(gSentPixelRgb[i * 3],
                     gSentPixelRgb[i * 3 + 1],
                     gSentPixelRgb[i * 3 + 2]);
  }
  else {
    CRGB    fg(p.fgR, p.fgG, p.fgB);
    CRGB    bg(p.bgR, p.bgG, p.bgB);
    uint8_t pos8 = p.phase >> 8;

    switch (p.fx) {
      case FX_BREATHE:
        fill_solid(leds, TOTAL_PIXELS, fg);
        nscale8(leds, TOTAL_PIXELS, sin8(pos8));
        break;

      case FX_RAINBOW: {
        uint8_t d = 1 + (p.size >> 3);
        fill_rainbow(leds, TOTAL_PIXELS, pos8, d);
        break;
      }

      case FX_CHASE: {
        uint8_t gap = 2 + (p.size >> 5);
        fill_solid(leds, TOTAL_PIXELS, bg);
        for (uint16_t g = 0; g < TOTAL_PIXELS; g += gap)
          leds[(g + pos8) % TOTAL_PIXELS] = fg;
        break;
      }

      case FX_COMET: {
        uint16_t head = ((uint32_t)p.phase * TOTAL_PIXELS) >> 16;
        uint8_t  tail = 2 + (p.size >> 3);
        for (uint16_t i = 0; i < TOTAL_PIXELS; i++) {
          int16_t d = (int16_t)head - (int16_t)i;
          if (d < 0) d += TOTAL_PIXELS;
          uint8_t b = (d < tail) ? (uint8_t)(255 - ((uint16_t)d * 255) / tail) : 0;
          leds[i] = blend(bg, fg, b);
        }
        break;
      }

      case FX_SPARKLE:
        //  Sparkle is deliberately local to each satellite, so the preview can
        //  only ever show a representative twinkle, never the exact pixels the
        //  strips will light. Density and colour are right; placement is not.
        for (uint8_t i = 0; i <= (p.size >> 5); i++)
          sparkleBuf[random16(TOTAL_PIXELS)] = 255;
        for (uint16_t i = 0; i < TOTAL_PIXELS; i++)
          leds[i] = blend(bg, fg, sparkleBuf[i]);
        break;

      case FX_SOLID:
      default:
        fill_solid(leds, TOTAL_PIXELS, fg);
        break;
    }
  }

  //  The satellite applies master via FastLED.setBrightness() at show() time.
  //  Same arithmetic here, since MAX_BRIGHT is 255 on the satellite too.
  nscale8(leds, TOTAL_PIXELS, p.master);

  for (uint16_t i = 0; i < TOTAL_PIXELS; i++)
    out[i] = ((uint32_t)leds[i].r << 16) | ((uint32_t)leds[i].g << 8) | leds[i].b;
}
