/*
 *  preview.cpp - the Preview tab's renderer
 *
 *  The effects are NOT here. They live in Effects.h, shared byte-identically
 *  with the satellite sketch, so this tab cannot show one look while the tape
 *  renders another. This file only supplies the whole strip as a single
 *  segment - offset 0, TOTAL_PIXELS wide - and applies master.
 *
 *  FastLED comes in via Effects.h for its arithmetic only: no addLeds(), no
 *  show(), so no RMT channel is claimed and no pin is driven. The controller
 *  still drives no strip.
 */

#include <Arduino.h>
#include "state.h"
#include "Effects.h"
#include "preview.h"

extern ParamsMsg gSentParams;
extern bool      gSentPixels;
extern uint8_t   gSentPixelRgb[TOTAL_PIXELS * 3];

static CRGB    leds[TOTAL_PIXELS];
static uint8_t sparkleBuf[TOTAL_PIXELS];

void previewRender(uint32_t *out)
{
  ParamsMsg p         = gSentParams;   // snapshot; a torn read costs one frame
  bool      pixelMode = gSentPixels;

  fxRender(leds, sparkleBuf, TOTAL_PIXELS, 0, TOTAL_PIXELS, p,
           pixelMode ? gSentPixelRgb : nullptr);

  //  The satellite applies master through FastLED.setBrightness() at show()
  //  time. There is no show() here, so scale the buffer instead - same result,
  //  and the reason master is not inside fxRender().
  nscale8(leds, TOTAL_PIXELS, p.master);

  for (uint16_t i = 0; i < TOTAL_PIXELS; i++)
    out[i] = ((uint32_t)leds[i].r << 16) | ((uint32_t)leds[i].g << 8) | leds[i].b;
}
