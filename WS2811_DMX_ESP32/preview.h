#pragma once
#include <stdint.h>

/* ===========================================================================
 *  preview.h - render the look the satellites were told to produce
 *
 *  Fills `out` with TOTAL_PIXELS entries of packed 0xRRGGBB, master already
 *  applied, so the Preview tab can show the strip without one attached.
 *
 *  >>> THIS MIRRORS render() IN WS2811_Satellite.ino AND MUST TRACK IT. <<<
 *  The two are separate implementations of the same effects, which is exactly
 *  the drift hazard the README already flags for Protocol.h. Change an effect
 *  in one and the preview quietly starts lying. The proper fix is a shared
 *  Effects.h carried in both sketch folders and covered by
 *  tools/check-protocol-sync.ps1; until then, change both together.
 * ========================================================================= */
void previewRender(uint32_t *out);
