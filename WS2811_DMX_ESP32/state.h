#pragma once
#include <stdint.h>
#include "Protocol.h"
#include "config.h"

// The controller has no FastLED dependency - it never renders.
struct Rgb {
  uint8_t r, g, b;
  // Explicit constructors, not default member initialisers: a struct with
  // NSDMIs is not an aggregate under -std=gnu++11, which the ESP32 Arduino
  // 2.x core still uses, so brace-init would fail to compile there.
  constexpr Rgb() : r(0), g(0), b(0) {}
  constexpr Rgb(uint8_t R, uint8_t G, uint8_t B) : r(R), g(G), b(B) {}
};

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

// Pull cfg.address back into 1..(513 - footprint) for the current mode. Call
// after anything changes cfg.mode - switching 3ch -> Pixel Map can leave a
// perfectly good address 200 channels past the end of the universe.
void settingsClamp();

// ---- UI layer (ui.cpp) ---------------------------------------------------
void uiBegin();
void uiTask(uint32_t now);
