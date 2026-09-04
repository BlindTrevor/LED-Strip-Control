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

#define DISPLAY_BRINGUP_READY 1     // <-- set to 1 once the hooks below are real
#define DISPLAY_TOUCH_DEBUG   0     // 1 = per-press and raw-register tracing

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
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

// ---------------------------------------------------------------------------
//  Waveshare ESP32-S3-Touch-LCD-4.3B pin map
//
//  Cross-checked against the GPIO allocation table in the README: the pins
//  below are exactly 0,1,2,3,5,7,10,14,17,18,21,38-42,45-48 for the panel and
//  4/8/9 for touch, with nothing left over. If you are holding the non-B 4.3,
//  stop - the map differs and a wrong one gives a dark or scrambled panel.
// ---------------------------------------------------------------------------
static const int PIN_HSYNC = 46;
static const int PIN_VSYNC =  3;
static const int PIN_DE    =  5;
static const int PIN_PCLK  =  7;

//  RGB565 data lines in esp_lcd order, which is LSB first: B3..B7, G2..G7,
//  R3..R7. The panel's low-order bits are simply not wired - that is what
//  makes it 565 rather than 888.
static const int PIN_RGB[16] = {
  14, 38, 18, 17, 10,          // B3 B4 B5 B6 B7
  39,  0, 45, 48, 47, 21,      // G2 G3 G4 G5 G6 G7
   1,  2, 42, 41, 40           // R3 R4 R5 R6 R7
};

static const int PIN_TP_INT = 4;   // GT911 interrupt, and address select at reset

// --- CH422G IO expander ----------------------------------------------------
//  Unusual part: it has no register-pointer byte. Each "register" is its own
//  I2C address that you write a single byte to. None of these collide with the
//  satellites at 0x30..0x32 or with the GT911 at 0x5D.
static const uint8_t CH422G_MODE = 0x24;   // system/mode
static const uint8_t CH422G_OUT  = 0x38;   // EXIO0..EXIO7 output states
static const uint8_t CH422G_PUSH_PULL = 0x01;

//  EXIO0 = DI0, EXIO5 = DI1 are brought out to the header and left alone.
static const uint8_t EXIO_TP_RST  = 1 << 1; // EXIO1 - GT911 reset
static const uint8_t EXIO_DISP    = 1 << 2; // EXIO2 - backlight LED driver AND
                                            //         the panel's DISP line, so
                                            //         it is on/off only: PWM here
                                            //         drops DISP too and puts the
                                            //         panel into standby.
static const uint8_t EXIO_LCD_RST = 1 << 3; // EXIO3 - panel reset. Leave this low
                                            //         and the panel initialises
                                            //         perfectly and stays dark.
static const uint8_t EXIO_SD_CS   = 1 << 4; // EXIO4 - TF card, active low

static uint8_t exioState = EXIO_SD_CS;     // SD deselected from the start

static bool exioFlush()
{
  Wire.beginTransmission(CH422G_OUT);
  Wire.write(exioState);
  return Wire.endTransmission() == 0;
}

static bool exioSet(uint8_t mask, bool on)
{
  if (on) exioState |= mask; else exioState &= (uint8_t)~mask;
  return exioFlush();
}

// --- GT911 capacitive touch ------------------------------------------------
//  The GT911 picks its address from the INT level as reset is released: low
//  gives 0x5D, high gives 0x14. INT is an input for the rest of the time, so
//  if the chip ever re-initialises on its own it can land on either one. Track
//  it rather than assuming, and re-probe if it stops answering.
static const uint8_t  GT911_ADDR_A  = 0x5D;
static const uint8_t  GT911_ADDR_B  = 0x14;
static uint8_t        gtAddr        = GT911_ADDR_A;

static const uint16_t GT911_COMMAND = 0x8040;   // 0 = read coordinates normally
static const uint16_t GT911_PRODUCT = 0x8140;
static const uint16_t GT911_STATUS  = 0x814E;
static const uint16_t GT911_POINT1  = 0x8150;

static bool gtRead(uint16_t reg, uint8_t *buf, size_t n)
{
  Wire.beginTransmission(gtAddr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;      // repeated start
  if (Wire.requestFrom((int)gtAddr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool gtWrite8(uint16_t reg, uint8_t v)
{
  Wire.beginTransmission(gtAddr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

//  Leaves gtAddr pointing at whichever address actually answered.
static bool gtProbe()
{
  const uint8_t cand[2] = { GT911_ADDR_A, GT911_ADDR_B };
  for (uint8_t i = 0; i < 2; i++) {
    gtAddr = cand[i];
    uint8_t id[4] = {0};
    if (gtRead(GT911_PRODUCT, id, 4) &&
        id[0] == '9' && id[1] == '1' && id[2] == '1') return true;
  }
  return false;
}

static esp_lcd_panel_handle_t panel = nullptr;

// ======================= HOOK 1 of 3 =======================================
//  Bring up the RGB panel, the backlight (CH422G) and the GT911. Return false
//  if anything fails. Do not call Wire.begin() here - see the note above.
static bool panelInit()
{
  // -- CH422G into push-pull output mode, backlight and touch held off ------
  Wire.beginTransmission(CH422G_MODE);
  Wire.write(CH422G_PUSH_PULL);
  if (Wire.endTransmission() != 0) {
    Serial.println("[display] CH422G not responding at 0x24.");
    return false;
  }
  exioState = EXIO_SD_CS;                 // both resets asserted, backlight off,
  if (!exioFlush()) return false;         // SD deselected

  // -- Release both resets together. INT is held low across the GT911's
  //    release, which is what selects its 0x5D address rather than 0x14.
  pinMode(PIN_TP_INT, OUTPUT);
  digitalWrite(PIN_TP_INT, LOW);
  delay(10);
  exioState |= EXIO_TP_RST | EXIO_LCD_RST;
  if (!exioFlush()) return false;

  //  INT must stay low for ~50 ms AFTER reset is released, not just across the
  //  release. Let go too early and the GT911 still latches its address and
  //  still answers its product ID - it just never starts scanning, so every
  //  status read comes back 0x00 and the panel looks dead while appearing
  //  perfectly healthy on the bus. That is a slow one to find.
  delay(55);
  pinMode(PIN_TP_INT, INPUT);             // release to interrupt duty
  delay(50);                              // settle before the first transaction

  if (!gtProbe()) {
    Serial.println("[display] GT911 not found at 0x5D or 0x14.");
    return false;
  }
  Serial.printf("[display] GT911 at 0x%02X\n", gtAddr);
  gtWrite8(GT911_COMMAND, 0x00);          // normal "read coordinates" mode
  gtWrite8(GT911_STATUS,  0x00);          // start from a clean status byte

#if DISPLAY_TOUCH_DEBUG
  //  Config block starts at 0x8047: version, X res, Y res, max points. A
  //  version of 0x00/0xFF or a nonsense resolution means the chip has no valid
  //  config and will answer I2C all day without ever scanning.
  uint8_t c[6] = {0};
  if (gtRead(0x8047, c, sizeof(c))) {
    Serial.printf("[display] GT911 cfgVer=0x%02X res=%ux%u maxPts=%u\n",
                  c[0], (unsigned)(c[1] | (c[2] << 8)),
                  (unsigned)(c[3] | (c[4] << 8)), (unsigned)(c[5] & 0x0F));
  } else {
    Serial.println("[display] GT911 config read failed.");
  }
#endif

  // -- RGB panel -----------------------------------------------------------
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src                    = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz            = 16 * 1000 * 1000;
  cfg.timings.h_res              = DISP_H_RES;
  cfg.timings.v_res              = DISP_V_RES;
  cfg.timings.hsync_pulse_width  = 48;
  cfg.timings.hsync_back_porch   = 88;
  cfg.timings.hsync_front_porch  = 40;
  cfg.timings.vsync_pulse_width  = 3;
  cfg.timings.vsync_back_porch   = 32;
  cfg.timings.vsync_front_porch  = 13;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.data_width                 = 16;
  cfg.sram_trans_align           = 4;
  cfg.psram_trans_align          = 64;
  cfg.hsync_gpio_num             = PIN_HSYNC;
  cfg.vsync_gpio_num             = PIN_VSYNC;
  cfg.de_gpio_num                = PIN_DE;
  cfg.pclk_gpio_num              = PIN_PCLK;
  memcpy(cfg.data_gpio_nums, PIN_RGB, sizeof(PIN_RGB));
  cfg.disp_gpio_num              = -1;    // DISP is on the CH422G, not a GPIO
  cfg.flags.fb_in_psram          = 1;     // 800*480*2 = 768 KB, needs the OPI PSRAM

  esp_err_t e = esp_lcd_new_rgb_panel(&cfg, &panel);
  if (e != ESP_OK) {
    Serial.printf("[display] esp_lcd_new_rgb_panel failed: %s\n", esp_err_to_name(e));
    return false;
  }
  if ((e = esp_lcd_panel_reset(panel)) != ESP_OK ||
      (e = esp_lcd_panel_init(panel))  != ESP_OK) {
    Serial.printf("[display] panel init failed: %s\n", esp_err_to_name(e));
    return false;
  }

  exioSet(EXIO_DISP, true);               // backlight on, last, so no flash of noise
  return true;
}

// ======================= HOOK 2 of 3 =======================================
//  Blit a rectangle of RGB565 pixels. Inclusive coordinates, as LVGL supplies
//  them; most esp_lcd / Arduino_GFX draw calls want an exclusive x2/y2, so add
//  one when you forward it.
static void panelFlush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       const uint16_t *px)
{
  if (!panel) return;
  esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px);
}

// ======================= HOOK 3 of 3 =======================================
//  Poll the touch panel. Return true while a finger is down and write screen
//  coordinates to *x / *y. Return false when it is not.
//  The GT911 raises bit 7 only when it has something NEW to say, and it says
//  the lift as well as the press. So "no new data" means "nothing has changed"
//  and the last state still stands - it does not mean the finger is up. LVGL
//  polls far faster than the panel produces events, so reporting released on
//  every quiet poll turns every press into a burst of press/release pairs and
//  no tap ever lands. Latch the state instead and only change it when the
//  GT911 actually reports something.
static bool panelTouch(int16_t *x, int16_t *y)
{
  static bool    held  = false;
  static int16_t lastX = 0, lastY = 0;

  uint8_t status = 0;
  bool    ok     = gtRead(GT911_STATUS, &status, 1);

  //  If it has stopped answering, it may have re-initialised onto its other
  //  address. Re-probe occasionally rather than giving up for good.
  if (!ok) {
    static uint32_t lastProbe = 0;
    if (millis() - lastProbe > 500) {
      lastProbe = millis();
      if (gtProbe()) {
        Serial.printf("[touch] GT911 reappeared at 0x%02X\n", gtAddr);
        ok = gtRead(GT911_STATUS, &status, 1);
      }
    }
  }

#if DISPLAY_TOUCH_DEBUG
  // Heartbeat: proves this is being polled at all, and shows what the GT911
  // is actually saying. Rate limited so it does not drown the status line.
  static uint32_t lastDbg = 0;
  static uint32_t polls   = 0;
  polls++;
  if (millis() - lastDbg > 1000) {
    lastDbg = millis();
    uint8_t id[4] = {0};
    bool idOk = gtRead(GT911_PRODUCT, id, 4);
    Serial.printf("[touch dbg] addr=0x%02X polls=%lu read=%d status=0x%02X id=%d:%c%c%c\n",
                  gtAddr, polls, (int)ok, status, (int)idOk,
                  id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?');
    polls = 0;
  }
#endif

  if (ok && (status & 0x80)) {
    uint8_t points = status & 0x0F;
    if (points >= 1) {
      uint8_t p[8];
      bool pok = gtRead(GT911_POINT1, p, sizeof(p));
#if DISPLAY_TOUCH_DEBUG
      Serial.printf("[touch raw] pts=%u ok=%d %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    points, (int)pok, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
#endif
      if (pok) {
        //  0x8150 is the X low byte. The track id lives at 0x814F, BEFORE this
        //  block - so there is no id byte to skip here. Getting that wrong
        //  shifts every read by one and turns X into garbage that fails the
        //  range check below, which looks exactly like a dead touch panel.
        int16_t nx = (int16_t)(p[0] | (p[1] << 8));
        int16_t ny = (int16_t)(p[2] | (p[3] << 8));
        if (nx >= 0 && nx < DISP_H_RES && ny >= 0 && ny < DISP_V_RES) {
          lastX = nx; lastY = ny;
#if DISPLAY_TOUCH_DEBUG
          if (!held) Serial.printf("[touch] %d,%d\n", nx, ny);
#endif
          held = true;
        }
      }
    } else {
      held = false;                     // GT911 reports the release too
    }
    gtWrite8(GT911_STATUS, 0);          // must clear, or it never reports again
  }

  *x = lastX;
  *y = lastY;
  return held;
}

// ---------------------------------------------------------------------------
//  Everything below is stack-independent and already done.
// ---------------------------------------------------------------------------
static lv_disp_draw_buf_t  drawBuf;
static lv_disp_drv_t       dispDrv;
static lv_indev_drv_t      indevDrv;
static esp_timer_handle_t  tickTimer = nullptr;

// 16 lines x 800 x 2 bytes = 25 KB per buffer, 50 KB for the pair, which fits
// internal SRAM alongside the DMX and I2C buffers. Fewer lines than the PSRAM
// version below, but the bandwidth it frees is worth far more than the extra
// flush callbacks cost.
static const uint32_t BUF_LINES_INT = 8;

// Only used if internal SRAM cannot supply the pair. Bigger, because in PSRAM
// the callback overhead matters more than the memory does.
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
  // EXIO2 is a plain on/off line, not a PWM pin - there is no dimming to be
  // had here without cutting the backlight trace. Treat it as a threshold.
  exioSet(EXIO_DISP, percent >= 50);
}

bool displayBegin()
{
  if (!panelInit()) {
    Serial.println("[display] panelInit() failed - running headless.");
    return false;
  }

  lv_init();

  //  Draw buffers go in INTERNAL SRAM, not PSRAM, and that is a deliberate
  //  trade of size for bandwidth.
  //
  //  The RGB peripheral streams the whole framebuffer out of PSRAM continuously
  //  - 800x480x2 at ~31 Hz is about 24 MB/s that must never be late. Put the
  //  LVGL draw buffers in PSRAM too and rendering reads and writes PSRAM, then
  //  the flush copies PSRAM to PSRAM, all competing with that stream. When the
  //  LCD's FIFO loses the race it starts a line late and the entire image jumps
  //  a few pixels sideways.
  //
  //  Internal SRAM is smaller, so fewer lines per buffer - but the render
  //  traffic leaves PSRAM entirely and the flush becomes a one-way copy.
  size_t n = DISP_H_RES * BUF_LINES_INT;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(
      n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(
      n * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  if (!b1) {                                  // internal SRAM too tight
    if (b2) { heap_caps_free(b2); b2 = nullptr; }
    Serial.println("[display] internal draw buffers failed - falling back to PSRAM.");
    n  = DISP_H_RES * BUF_LINES;
    b1 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    b2 = (lv_color_t *)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
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
