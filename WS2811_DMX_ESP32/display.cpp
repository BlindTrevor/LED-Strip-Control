/*
 *  display.cpp - panel / touch / LVGL plumbing
 *
 *  ---------------------------------------------------------------------------
 *  READ THIS FIRST
 *  ---------------------------------------------------------------------------
 *  The RGB panel timings, the GT911 reset sequence and the CH422G bit map for
 *  the ESP32-S3-Touch-LCD-4.3B are NOT written out below, on purpose. They vary
 *  between board revisions and between the 4.3 and 4.3B variants, and a wrong
 *  guess here costs bench time to unpick. They have to come from a bring-up
 *  that has actually run on your hardware.
 *
 *  What IS written out below is everything that is the same whichever stack you
 *  end up using: the LVGL draw buffers (PSRAM-backed), the display driver
 *  registration, the input device registration, and the 1 ms tick source. That
 *  is the fiddly part and it is done.
 *
 *  TO ENABLE THE SCREEN
 *  --------------------
 *    1. Get any one stack drawing a rectangle and reporting touch coordinates:
 *         - ESP32_Display_Panel  (Espressif; has a board preset for this panel)
 *         - Arduino_GFX + a GT911 library + hand-written CH422G writes
 *         - Waveshare's own ESP32-S3-Touch-LCD-4.3B Arduino demo
 *    2. Paste that init into panelInit(), its blit into panelFlush(), and its
 *       touch poll into panelTouch(), below.
 *    3. Set DISPLAY_BRINGUP_READY to 1.
 *    4. Install LVGL 8.3.x and copy its lv_conf.h next to the library folder
 *       with LV_COLOR_DEPTH 16 (see the README).
 *
 *  Nothing else in the firmware needs to change: ui.cpp is already written
 *  against LVGL only, and falls back to the serial status line until this
 *  returns true.
 *
 *  I2C NOTE: Wire is already begun in setup() for the satellite link, on the
 *  same GPIO8/9 bus as the GT911 and the CH422G. Do NOT call Wire.begin() again
 *  in panelInit() - reuse the existing bus, or the satellites drop off it.
 */

#include <Arduino.h>
#include "display.h"
// Deliberately NOT config.h - it defines the static SATS[] table, which would
// go unused in this file and warn in every build.

#define DISPLAY_BRINGUP_READY 0     // <-- set to 1 once the hooks below are real

static bool gReady = false;

bool displayReady() { return gReady; }

#if !UI_HAS_LVGL || !DISPLAY_BRINGUP_READY
// ---------------------------------------------------------------------------
//  Stub build. The firmware runs headless: DMX in, parameters out over I2C.
// ---------------------------------------------------------------------------
bool displayBegin()
{
#if !UI_HAS_LVGL
  Serial.println("[display] LVGL not installed - running headless.");
#else
  Serial.println("[display] bring-up not enabled - running headless.");
  Serial.println("[display] see the header comment in display.cpp.");
#endif
  return false;
}

void displaySetBacklight(uint8_t) {}

#else
// ---------------------------------------------------------------------------
//  Real build.
// ---------------------------------------------------------------------------
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

// ======================= HOOK 1 of 3 =======================================
//  Bring up the RGB panel, the backlight (CH422G) and the GT911. Return false
//  if anything fails. Do not call Wire.begin() here - see the note above.
static bool panelInit()
{
  // <<< paste your working panel + touch + backlight init here >>>
  return false;
}

// ======================= HOOK 2 of 3 =======================================
//  Blit a rectangle of RGB565 pixels. Inclusive coordinates, as LVGL supplies
//  them; most esp_lcd / Arduino_GFX draw calls want an exclusive x2/y2, so add
//  one when you forward it.
static void panelFlush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       const uint16_t *px)
{
  (void)x1; (void)y1; (void)x2; (void)y2; (void)px;
  // <<< paste your blit here >>>
}

// ======================= HOOK 3 of 3 =======================================
//  Poll the touch panel. Return true while a finger is down and write screen
//  coordinates to *x / *y. Return false when it is not.
static bool panelTouch(int16_t *x, int16_t *y)
{
  (void)x; (void)y;
  // <<< paste your touch read here >>>
  return false;
}

// ---------------------------------------------------------------------------
//  Everything below is stack-independent and already done.
// ---------------------------------------------------------------------------
static lv_disp_draw_buf_t  drawBuf;
static lv_disp_drv_t       dispDrv;
static lv_indev_drv_t      indevDrv;
static esp_timer_handle_t  tickTimer = nullptr;

// 40 lines is a good compromise on this panel: big enough that LVGL is not
// called back constantly, small enough that two of them fit in PSRAM without
// crowding out the DMX buffers.
static const uint32_t BUF_LINES = 40;

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
  panelFlush(area->x1, area->y1, area->x2, area->y2, (const uint16_t *)px);
  lv_disp_flush_ready(drv);
}

static void touchCb(lv_indev_drv_t *, lv_indev_data_t *data)
{
  int16_t x, y;
  if (panelTouch(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

#if !LV_TICK_CUSTOM
static void tickCb(void *) { lv_tick_inc(1); }
#endif

void displaySetBacklight(uint8_t percent)
{
  if (!gReady) return;
  (void)percent;
  // <<< optional: paste a backlight PWM/CH422G write here >>>
}

bool displayBegin()
{
  if (!panelInit()) {
    Serial.println("[display] panelInit() failed - running headless.");
    return false;
  }

  lv_init();

  size_t n = DISP_H_RES * BUF_LINES;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t),
                                                  MALLOC_CAP_SPIRAM);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t),
                                                  MALLOC_CAP_SPIRAM);
  if (!b1) {                                  // no PSRAM? take a smaller bite
    if (b2) { heap_caps_free(b2); b2 = nullptr; }
    n  = DISP_H_RES * 10;
    b1 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_DMA);
  }
  if (!b1) {
    Serial.println("[display] draw buffer alloc failed - running headless.");
    return false;
  }
  lv_disp_draw_buf_init(&drawBuf, b1, b2, n);

  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res  = DISP_H_RES;
  dispDrv.ver_res  = DISP_V_RES;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&indevDrv);
  indevDrv.type    = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchCb;
  lv_indev_drv_register(&indevDrv);

#if !LV_TICK_CUSTOM
  // LVGL needs a millisecond tick. loop() also runs settingsSave(), which
  // blocks on an NVS write, so drive the tick from a timer rather than from
  // elapsed-time bookkeeping in uiTask().
  esp_timer_create_args_t ta = {};
  ta.callback = tickCb;
  ta.arg      = nullptr;
  ta.name     = "lv_tick";
  esp_timer_create(&ta, &tickTimer);
  esp_timer_start_periodic(tickTimer, 1000);
#endif

  gReady = true;
  Serial.println("[display] up.");
  return true;
}
#endif
