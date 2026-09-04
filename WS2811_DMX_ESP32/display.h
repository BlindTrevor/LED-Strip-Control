#pragma once
#include <stdint.h>

/* ===========================================================================
 *  display.h - the ONLY file that knows the panel exists
 *
 *  Everything hardware-specific about the 4.3" RGB LCD, the GT911 touch
 *  controller and the CH422G expander lives in display.cpp. ui.cpp is pure
 *  LVGL and never touches a register, so swapping display stacks - or getting
 *  one working for the first time - is a one-file job.
 *
 *  Nothing here is guessed. display.cpp ships with the bring-up DISABLED and
 *  three clearly marked hooks for you to paste your working init into; see the
 *  header comment there. Until then displayBegin() returns false, uiTask()
 *  falls back to the serial status line, and the DMX and I2C paths are
 *  completely unaffected.
 * ========================================================================= */

// --- build the LVGL UI, or not? -------------------------------------------
//  A deliberate switch, NOT auto-detection. `__has_include(<lvgl.h>)` cannot
//  work here and was silently doing the wrong thing: the Arduino builder finds
//  libraries by scanning sources for #include directives and only then adds
//  their include paths, so <lvgl.h> is unreachable at the moment __has_include
//  is evaluated. It therefore always answered "no", and the firmware built
//  headless even with LVGL installed and lv_conf.h correct - a green build
//  that had never compiled a line of ui.cpp.
//
//  Set this to 0 to build on a machine without the LVGL library. ui.cpp then
//  degrades to exactly the status line it printed before the UI existed, which
//  keeps the DMX/I2C bench path buildable.
#ifndef UI_HAS_LVGL
#  define UI_HAS_LVGL 1
#endif

#define DISP_H_RES 800
#define DISP_V_RES 480

// Bring up the panel, touch and LVGL. Returns false if the display stack is
// not available or not yet implemented - callers must cope with that.
bool displayBegin();

// True once displayBegin() has succeeded.
bool displayReady();

// Backlight, 0..100. Silently ignored when the panel is not up.
void displaySetBacklight(uint8_t percent);
