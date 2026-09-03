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

// --- is LVGL even installed? ----------------------------------------------
//  Guarded so the sketch still compiles on a machine without the LVGL library.
//  With LVGL absent, ui.cpp degrades to exactly the status line it printed
//  before the UI existed, which keeps the DMX/I2C bench path buildable.
#if defined(__has_include)
#  if __has_include(<lvgl.h>)
#    define UI_HAS_LVGL 1
#  endif
#endif
#ifndef UI_HAS_LVGL
#  define UI_HAS_LVGL 0
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
