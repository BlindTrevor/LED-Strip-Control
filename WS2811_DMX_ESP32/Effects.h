#pragma once
#include <FastLED.h>
#include "Protocol.h"

/* ===========================================================================
 *  Effects.h - the single definition of what each effect looks like
 *
 *  DUPLICATED, LIKE Protocol.h, AND FOR THE SAME REASON: Arduino IDE 2.x only
 *  compiles headers that sit inside the sketch folder. The two copies must
 *  stay byte-identical.
 *
 *      pwsh tools/check-protocol-sync.ps1                # compare
 *      pwsh tools/check-protocol-sync.ps1 -Fix           # controller -> satellite
 *      pwsh tools/check-protocol-sync.ps1 -InstallHook   # block commits that drift
 *
 *  Both the satellite's renderer and the controller's Preview tab call
 *  fxRender(), so the screen cannot show one thing while the tape does
 *  another. Before this existed they were two hand-written implementations of
 *  the same six effects, and keeping them in step was a matter of remembering.
 *
 *  SEGMENTS. Every effect is computed in GLOBAL pixel space and then written
 *  into the caller's own segment, which is what makes a chase or a comet run
 *  continuously across several strips instead of restarting on each one:
 *
 *      satellite   offset = SAT_OFFSET, count = SAT_PIXELS
 *      preview     offset = 0,          count = TOTAL_PIXELS
 *
 *  FastLED is required here for its arithmetic - sin8(), fill_rainbow(),
 *  blend(), nscale8(), qsub8(). The controller links it for that alone: no
 *  addLeds(), no show(), so no RMT channel is claimed and no pin is driven.
 *  Those curves are specific approximations and reimplementing them
 *  "equivalently" would make the preview a confident lie.
 *
 *  NOT here, on purpose:
 *    - master. The satellite applies it through FastLED.setBrightness() at
 *      show() time; the preview has no show() and scales the buffer instead.
 *      Same result, different mechanism, so each caller does its own.
 *    - sparkle placement. Deliberately local to each renderer - independent
 *      twinkling per strip is the point - so the preview can only ever match
 *      its density, never its exact pixels.
 * ========================================================================= */

//  leds        - the caller's segment, `count` entries
//  sparkleBuf  - the caller's own sparkle state, `count` entries
//  pixelRgb    - packed RGB for this segment when in pixel-map mode, indexed
//                from 0 within the segment, or nullptr for parameter modes
static inline void fxRender(CRGB *leds, uint8_t *sparkleBuf,
                            uint16_t count, uint16_t offset, uint16_t total,
                            const ParamsMsg &p, const uint8_t *pixelRgb)
{
  //  Decay every frame whatever the mode, so switching into Sparkle never
  //  shows stale full-brightness pixels left from a previous visit.
  uint8_t decay = 8 + (p.speed >> 2);
  for (uint16_t i = 0; i < count; i++)
    sparkleBuf[i] = qsub8(sparkleBuf[i], decay);

  if (pixelRgb) {
    for (uint16_t i = 0; i < count; i++)
      leds[i] = CRGB(pixelRgb[i * 3], pixelRgb[i * 3 + 1], pixelRgb[i * 3 + 2]);
    return;
  }

  CRGB    fg(p.fgR, p.fgG, p.fgB);
  CRGB    bg(p.bgR, p.bgG, p.bgB);
  uint8_t pos8 = p.phase >> 8;

  switch (p.fx) {
    case FX_BREATHE:
      fill_solid(leds, count, fg);
      nscale8(leds, count, sin8(pos8));
      break;

    case FX_RAINBOW: {
      uint8_t d = 1 + (p.size >> 3);
      fill_rainbow(leds, count, pos8 + (uint8_t)(offset * d), d);
      break;
    }

    case FX_CHASE: {
      uint8_t gap = 2 + (p.size >> 5);            // 2..9 px spacing
      fill_solid(leds, count, bg);
      for (uint16_t g = 0; g < total; g += gap) {
        uint16_t gi = (g + pos8) % total;
        if (gi >= offset && gi < offset + count) leds[gi - offset] = fg;
      }
      break;
    }

    case FX_COMET: {
      uint16_t head = ((uint32_t)p.phase * total) >> 16;
      uint8_t  tail = 2 + (p.size >> 3);          // 2..33 px of trail
      for (uint16_t i = 0; i < count; i++) {
        int16_t d = (int16_t)head - (int16_t)(i + offset);
        if (d < 0) d += total;
        uint8_t b = (d < tail) ? (uint8_t)(255 - ((uint16_t)d * 255) / tail) : 0;
        leds[i] = blend(bg, fg, b);
      }
      break;
    }

    case FX_SPARKLE:
      for (uint8_t i = 0; i <= (p.size >> 5); i++)
        sparkleBuf[random16(count)] = 255;
      for (uint16_t i = 0; i < count; i++)
        leds[i] = blend(bg, fg, sparkleBuf[i]);
      break;

    case FX_SOLID:
    default:
      fill_solid(leds, count, fg);
      break;
  }
}
