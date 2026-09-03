#pragma once
#include <stdint.h>
#include "Protocol.h"
#include "config.h"

// The controller has no FastLED dependency - it never renders.
struct Rgb { uint8_t r = 0, g = 0, b = 0; };

struct Params {
  uint8_t master = 0;
  Rgb     fg, bg;
  uint8_t fx     = FX_SOLID;
  uint8_t speed  = 128;
  uint8_t size   = 128;
  uint8_t strobe = 0;
};

struct Config {
  uint16_t address    = 1;
  uint8_t  mode       = CM_11CH;
  bool     standalone = false;   // true = ignore DMX, drive from the screen
};

// ---- shared state --------------------------------------------------------
//  Written by the UI on core 1, read by the link task on core 0. Unguarded on
//  purpose: every field is a byte, and a torn read lasts one 10 ms frame. A
//  mutex here could stall the link task behind an 800x480 LVGL redraw.
extern Config   cfg;
extern Params   manual;         // what the touchscreen is driving
extern Params   live;           // what is actually being sent
extern bool     dmxAlive;
extern uint32_t dmxFrameCount;
extern uint32_t i2cErrors;

void settingsLoad();
void settingsSave();

// ---- UI layer (ui.cpp) ---------------------------------------------------
void uiBegin();
void uiTask(uint32_t now);
