#pragma once
#include <stdint.h>

/* ===========================================================================
 *  preview.h - render the look the satellites were told to produce
 *
 *  Fills `out` with TOTAL_PIXELS entries of packed 0xRRGGBB, master already
 *  applied, so the Preview tab can show the strip without one attached.
 *
 *  The effects come from Effects.h, which is shared byte-identically with the
 *  satellite sketch and guarded by tools/check-protocol-sync.ps1 - so this
 *  cannot drift from what the tape actually does.
 *
 *  One thing it still cannot match exactly, by design rather than neglect:
 *  sparkle placement is local to each satellite, so the strips twinkle
 *  independently. Its density and colour are right; the individual pixels
 *  are representative.
 * ========================================================================= */
void previewRender(uint32_t *out);
