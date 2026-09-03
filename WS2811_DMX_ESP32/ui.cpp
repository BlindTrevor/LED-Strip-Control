/*
 *  ui.cpp - LVGL 8 touchscreen UI
 *
 *  Three pages on the 800x480 panel:
 *
 *    Setup   DMX start address and channel mode, plus a standalone switch for
 *            driving the rig with no desk attached. Persisted to NVS on Save.
 *    Manual  What standalone mode plays: master, effect, speed, size, strobe,
 *            and foreground / background colour. Writes straight into `manual`.
 *    Status  What is actually going out - DMX health, frame and error counts,
 *            the live look. Read-only.
 *
 *  This file contains no hardware knowledge at all. Everything about the RGB
 *  panel, the GT911 and the CH422G is in display.cpp behind three hooks. If
 *  displayBegin() returns false - because the bring-up is not enabled yet, or
 *  LVGL is not installed - uiTask() falls back to the serial status line the
 *  headless placeholder used to print, and the DMX and I2C paths carry on
 *  untouched. That fallback is the point: the bench path never depends on the
 *  screen working.
 *
 *  Threading: every LVGL call happens here, on core 1, from loop(). The link
 *  task on core 0 only ever reads `cfg` and `manual`. LVGL is not thread-safe
 *  and nothing here may be called from the link task.
 *
 *  Debug printing needs "USB CDC On Boot: Enabled" - UART0's pins ARE the
 *  RS485 transceiver, so printing to UART0 injects console text onto DMX.
 */

#include <Arduino.h>
#include "state.h"
#include "display.h"

// ===========================================================================
//  Fallback status line - also used whenever the panel is not up.
// ===========================================================================
static void serialStatus(uint32_t now)
{
  static uint32_t last = 0;
  if (now - last < 1000) { delay(5); return; }
  last = now;

  Serial.printf("[%6lus] %s addr %-3u %-14s dmx=%-3s frames=%-7lu "
                "i2cErr=%-5lu master=%-3u fx=%s\n",
                now / 1000,
                cfg.standalone ? "MANUAL" : "DMX   ",
                cfg.address,
                CM_NAME[cfg.mode < CM_COUNT ? cfg.mode : 0],
                dmxAlive ? "yes" : "no",
                dmxFrameCount,
                i2cErrors,
                live.master,
                FX_NAME[live.fx < FX_COUNT ? live.fx : 0]);
}

static void manualDefaults()
{
  // Something visible with no DMX and no screen: white sparkle over a dim blue
  // background, which is the look that needs 11ch on the desk.
  manual.master = 180;
  manual.fg     = { 255, 255, 255 };
  manual.bg     = {   0,   0,  40 };
  manual.fx     = FX_SPARKLE;
  manual.speed  = 120;
  manual.size   = 96;
  manual.strobe = 0;
}

#if !UI_HAS_LVGL
// ---------------------------------------------------------------------------
//  LVGL absent: compile to exactly the old headless behaviour.
// ---------------------------------------------------------------------------
void uiBegin() { Serial.begin(115200); manualDefaults(); displayBegin(); }
void uiTask(uint32_t now) { serialStatus(now); }

#else
#include <lvgl.h>

// Only Montserrat 14 is guaranteed by a stock lv_conf.h. Enable 22 and 28
// there for the big readouts; without them the UI still lays out, just smaller.
#if LV_FONT_MONTSERRAT_28
#  define FONT_BIG (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_22
#  define FONT_BIG (&lv_font_montserrat_22)
#else
#  define FONT_BIG LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_22
#  define FONT_MED (&lv_font_montserrat_22)
#else
#  define FONT_MED LV_FONT_DEFAULT
#endif

#define COL_BG      lv_color_hex(0x12141a)
#define COL_PANEL   lv_color_hex(0x1e222b)
#define COL_TEXT    lv_color_hex(0xe6e9ef)
#define COL_MUTED   lv_color_hex(0x8b93a7)
#define COL_OK      lv_color_hex(0x3ecf7a)
#define COL_WARN    lv_color_hex(0xe4b243)
#define COL_ERR     lv_color_hex(0xe2564d)
#define COL_RED     lv_color_hex(0xe2564d)
#define COL_GREEN   lv_color_hex(0x3ecf7a)
#define COL_BLUE    lv_color_hex(0x5aa9e6)

// ---- widgets refreshed after creation -------------------------------------
static lv_obj_t *lblAddr  = NULL, *lblRange = NULL, *ddMode = NULL;
static lv_obj_t *swAlone  = NULL, *lblSaved = NULL, *ddFx   = NULL;
static lv_obj_t *swFg     = NULL, *swBg     = NULL;
static lv_obj_t *stSource = NULL, *stDmx    = NULL, *stFrames = NULL;
static lv_obj_t *stErr    = NULL, *stAddr   = NULL, *stLook   = NULL;
static lv_obj_t *stFgSw   = NULL, *stBgSw   = NULL;

static uint32_t savedAtMs = 0;

// A slider bound to one byte of `manual`, with its own value label.
struct Bind { uint8_t *target; lv_obj_t *lbl; };
static Bind    binds[12];
static uint8_t bindN = 0;

static char modeOpts[160];
static char fxOpts[96];

// ===========================================================================
//  Small helpers
// ===========================================================================
static lv_obj_t *panel(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_set_size(p, w, h);
  lv_obj_set_style_bg_color(p, COL_PANEL, 0);
  lv_obj_set_style_border_width(p, 0, 0);
  lv_obj_set_style_radius(p, 8, 0);
  lv_obj_set_style_pad_all(p, 10, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  return p;
}

// A transparent, non-scrolling container - used wherever a row needs to hold
// several widgets without drawing a box of its own.
static lv_obj_t *bare(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *txt)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, COL_MUTED, 0);
  return l;
}

static lv_obj_t *swatch(lv_obj_t *parent, Rgb c)
{
  lv_obj_t *s = lv_obj_create(parent);
  lv_obj_set_size(s, 54, 30);
  lv_obj_set_style_radius(s, 6, 0);
  lv_obj_set_style_border_width(s, 1, 0);
  lv_obj_set_style_border_color(s, COL_MUTED, 0);
  lv_obj_set_style_bg_color(s, lv_color_make(c.r, c.g, c.b), 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static void sliderCb(lv_event_t *e)
{
  Bind   *b = (Bind *)lv_event_get_user_data(e);
  uint8_t v = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
  *b->target = v;
  lv_label_set_text_fmt(b->lbl, "%u", v);

  if (swFg) lv_obj_set_style_bg_color(swFg,
              lv_color_make(manual.fg.r, manual.fg.g, manual.fg.b), 0);
  if (swBg) lv_obj_set_style_bg_color(swBg,
              lv_color_make(manual.bg.r, manual.bg.g, manual.bg.b), 0);
}

// A labelled 0..255 slider wired directly to a byte of `manual`.
static void sliderRow(lv_obj_t *parent, const char *name, uint8_t *target,
                      lv_color_t accent)
{
  lv_obj_t *row = bare(parent, LV_PCT(100), 48);

  lv_obj_t *nm = caption(row, name);
  lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *val = lv_label_create(row);
  lv_obj_set_style_text_color(val, COL_TEXT, 0);
  lv_label_set_text_fmt(val, "%u", *target);
  lv_obj_align(val, LV_ALIGN_TOP_RIGHT, 0, 0);

  lv_obj_t *s = lv_slider_create(row);
  lv_slider_set_range(s, 0, 255);
  lv_slider_set_value(s, *target, LV_ANIM_OFF);
  lv_obj_set_size(s, LV_PCT(100), 14);
  lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s, accent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, accent, LV_PART_KNOB);
  // Fat knob: this is a finger on a 4.3" panel, not a mouse.
  lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);

  binds[bindN] = { target, val };
  lv_obj_add_event_cb(s, sliderCb, LV_EVENT_VALUE_CHANGED, &binds[bindN]);
  bindN++;
}

// ===========================================================================
//  Setup page
// ===========================================================================
static void refreshAddr()
{
  lv_label_set_text_fmt(lblAddr, "%03u", cfg.address);
  uint16_t fp = CM_FOOTPRINT[cfg.mode];
  lv_label_set_text_fmt(lblRange, "channels %u - %u  (%u wide)",
                        cfg.address, (unsigned)(cfg.address + fp - 1), fp);
}

static void addrCb(lv_event_t *e)
{
  int32_t delta = (int32_t)(intptr_t)lv_event_get_user_data(e);
  int32_t a     = (int32_t)cfg.address + delta;
  int32_t hi    = 513 - (int32_t)CM_FOOTPRINT[cfg.mode];
  if (a < 1)  a = 1;
  if (a > hi) a = hi;
  cfg.address = (uint16_t)a;
  refreshAddr();
}

static void modeCb(lv_event_t *)
{
  uint16_t sel = lv_dropdown_get_selected(ddMode);
  cfg.mode = (uint8_t)(sel < CM_COUNT ? sel : 0);
  settingsClamp();      // a wider mode can push the address past 512
  refreshAddr();
}

static void aloneCb(lv_event_t *)
{
  cfg.standalone = lv_obj_has_state(swAlone, LV_STATE_CHECKED);
}

static void saveCb(lv_event_t *)
{
  settingsSave();
  savedAtMs = millis();
  lv_label_set_text(lblSaved, LV_SYMBOL_OK "  Saved to NVS");
  lv_obj_set_style_text_color(lblSaved, COL_OK, 0);
}

static void addrBtn(lv_obj_t *parent, const char *txt, int32_t delta)
{
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 82, 62);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, addrCb, LV_EVENT_CLICKED, (void *)(intptr_t)delta);
}

static void buildSetup(lv_obj_t *page)
{
  lv_obj_set_style_pad_all(page, 12, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  // ---- address ----
  lv_obj_t *p = panel(page, LV_PCT(100), 172);
  lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *ct = caption(p, "DMX start address");
  lv_obj_align(ct, LV_ALIGN_TOP_LEFT, 4, 0);

  lblAddr = lv_label_create(p);
  lv_obj_set_style_text_font(lblAddr, FONT_BIG, 0);
  lv_obj_set_style_text_color(lblAddr, COL_TEXT, 0);
  lv_obj_align(lblAddr, LV_ALIGN_LEFT_MID, 6, 0);

  lblRange = lv_label_create(p);
  lv_obj_set_style_text_color(lblRange, COL_MUTED, 0);
  lv_obj_align(lblRange, LV_ALIGN_BOTTOM_LEFT, 6, 0);

  lv_obj_t *btns = bare(p, 364, 66);
  lv_obj_align(btns, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(btns, 8, 0);
  addrBtn(btns, "-10", -10);
  addrBtn(btns, "-1",   -1);
  addrBtn(btns, "+1",   +1);
  addrBtn(btns, "+10", +10);

  // ---- mode + standalone ----
  lv_obj_t *q = panel(page, LV_PCT(100), 150);
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 184);

  lv_obj_t *mt = caption(q, "Channel mode");
  lv_obj_align(mt, LV_ALIGN_TOP_LEFT, 4, 0);

  ddMode = lv_dropdown_create(q);
  lv_dropdown_set_options_static(ddMode, modeOpts);
  lv_dropdown_set_selected(ddMode, cfg.mode);
  lv_obj_set_size(ddMode, 320, 54);
  lv_obj_align(ddMode, LV_ALIGN_LEFT_MID, 6, 10);
  lv_obj_add_event_cb(ddMode, modeCb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *at = caption(q, "Ignore DMX (manual only)");
  lv_obj_align(at, LV_ALIGN_TOP_RIGHT, -4, 0);

  swAlone = lv_switch_create(q);
  lv_obj_set_size(swAlone, 90, 46);
  lv_obj_align(swAlone, LV_ALIGN_RIGHT_MID, -6, 10);
  if (cfg.standalone) lv_obj_add_state(swAlone, LV_STATE_CHECKED);
  lv_obj_add_event_cb(swAlone, aloneCb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- save ----
  lv_obj_t *save = lv_btn_create(page);
  lv_obj_set_size(save, 210, 64);
  lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_t *sv = lv_label_create(save);
  lv_label_set_text(sv, LV_SYMBOL_SAVE "  Save");
  lv_obj_center(sv);
  lv_obj_add_event_cb(save, saveCb, LV_EVENT_CLICKED, NULL);

  lblSaved = lv_label_create(page);
  lv_label_set_text(lblSaved, "");
  lv_obj_align(lblSaved, LV_ALIGN_BOTTOM_LEFT, 4, -22);

  refreshAddr();
}

// ===========================================================================
//  Manual page
// ===========================================================================
static void fxCb(lv_event_t *)
{
  uint16_t sel = lv_dropdown_get_selected(ddFx);
  manual.fx = (uint8_t)(sel < FX_COUNT ? sel : 0);
}

static void colourHeader(lv_obj_t *parent, const char *txt, Rgb c,
                         lv_obj_t **outSwatch)
{
  lv_obj_t *h = bare(parent, LV_PCT(100), 34);
  lv_obj_t *t = caption(h, txt);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
  *outSwatch = swatch(h, c);
  lv_obj_align(*outSwatch, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void buildManual(lv_obj_t *page)
{
  lv_obj_set_style_pad_all(page, 12, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  // ---- left: level and movement ----
  lv_obj_t *l = panel(page, 368, 380);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(l, 4, 0);

  sliderRow(l, "Master", &manual.master, COL_TEXT);

  caption(l, "Effect");
  ddFx = lv_dropdown_create(l);
  lv_dropdown_set_options_static(ddFx, fxOpts);
  lv_dropdown_set_selected(ddFx, manual.fx);
  lv_obj_set_size(ddFx, LV_PCT(100), 50);
  lv_obj_add_event_cb(ddFx, fxCb, LV_EVENT_VALUE_CHANGED, NULL);

  sliderRow(l, "Speed",  &manual.speed,  COL_BLUE);
  sliderRow(l, "Size",   &manual.size,   COL_BLUE);
  sliderRow(l, "Strobe", &manual.strobe, COL_WARN);

  // ---- right: colour ----
  lv_obj_t *r = panel(page, 368, 380);
  lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(r, 2, 0);

  colourHeader(r, "Foreground", manual.fg, &swFg);
  sliderRow(r, "Red",   &manual.fg.r, COL_RED);
  sliderRow(r, "Green", &manual.fg.g, COL_GREEN);
  sliderRow(r, "Blue",  &manual.fg.b, COL_BLUE);

  colourHeader(r, "Background  (Chase / Comet / Sparkle)", manual.bg, &swBg);
  sliderRow(r, "Red",   &manual.bg.r, COL_RED);
  sliderRow(r, "Green", &manual.bg.g, COL_GREEN);
  sliderRow(r, "Blue",  &manual.bg.b, COL_BLUE);

  // Strobe is folded into master by the controller before sending, so all
  // satellites blink together. Say so, since 0 meaning "off" is not obvious.
  lv_obj_t *note = caption(page, "Strobe 0 = off. Sparkle twinkles per strip; "
                                 "chase and comet run across all of them.");
  lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

// ===========================================================================
//  Status page
// ===========================================================================
static lv_obj_t *statRow(lv_obj_t *parent, const char *name, lv_coord_t y)
{
  lv_obj_t *n = caption(parent, name);
  lv_obj_align(n, LV_ALIGN_TOP_LEFT, 6, y);
  lv_obj_t *v = lv_label_create(parent);
  lv_obj_set_style_text_color(v, COL_TEXT, 0);
  lv_label_set_text(v, "-");
  lv_obj_align(v, LV_ALIGN_TOP_LEFT, 210, y);
  return v;
}

static void buildStatus(lv_obj_t *page)
{
  lv_obj_set_style_pad_all(page, 12, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *p = panel(page, LV_PCT(100), 92);
  lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 0);

  stSource = lv_label_create(p);
  lv_obj_set_style_text_font(stSource, FONT_BIG, 0);
  lv_obj_set_style_text_color(stSource, COL_TEXT, 0);
  lv_obj_align(stSource, LV_ALIGN_LEFT_MID, 6, 0);

  lv_obj_t *sl = caption(p, "fg / bg");
  lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -130, 0);
  stFgSw = swatch(p, live.fg);
  lv_obj_align(stFgSw, LV_ALIGN_RIGHT_MID, -66, 0);
  stBgSw = swatch(p, live.bg);
  lv_obj_align(stBgSw, LV_ALIGN_RIGHT_MID, -4, 0);

  lv_obj_t *q = panel(page, LV_PCT(100), 288);
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 102);
  stDmx    = statRow(q, "DMX input",        8);
  stFrames = statRow(q, "Frames received", 50);
  stErr    = statRow(q, "I2C errors",      92);
  stAddr   = statRow(q, "Patch",          134);
  stLook   = statRow(q, "Look",           176);

  lv_obj_t *sats = caption(q, "");
  lv_label_set_text_fmt(sats, "%u satellite%s, %u pixels total",
                        SAT_COUNT, SAT_COUNT == 1 ? "" : "s", TOTAL_PIXELS);
  lv_obj_align(sats, LV_ALIGN_BOTTOM_LEFT, 6, 0);
}

// Read-only mirror of what the link task is doing. `live` is written on core 0
// without a lock, which is fine here: a torn read shows one stale byte for one
// refresh, and this is a status display, not a control path.
static void refreshStatus(lv_timer_t *)
{
  bool fromDmx = dmxAlive && !cfg.standalone;
  lv_label_set_text(stSource, fromDmx ? "DMX" : "MANUAL");

  lv_label_set_text(stDmx, dmxAlive ? "live" : "no signal");
  lv_obj_set_style_text_color(stDmx, dmxAlive ? COL_OK : COL_WARN, 0);

  lv_label_set_text_fmt(stFrames, "%lu", (unsigned long)dmxFrameCount);

  lv_label_set_text_fmt(stErr, "%lu", (unsigned long)i2cErrors);
  lv_obj_set_style_text_color(stErr, i2cErrors ? COL_ERR : COL_OK, 0);

  lv_label_set_text_fmt(stAddr, "%u   %s", cfg.address,
                        CM_NAME[cfg.mode < CM_COUNT ? cfg.mode : 0]);
  lv_label_set_text_fmt(stLook, "%s   master %u",
                        FX_NAME[live.fx < FX_COUNT ? live.fx : 0], live.master);

  lv_obj_set_style_bg_color(stFgSw,
      lv_color_make(live.fg.r, live.fg.g, live.fg.b), 0);
  lv_obj_set_style_bg_color(stBgSw,
      lv_color_make(live.bg.r, live.bg.g, live.bg.b), 0);

  if (lblSaved && savedAtMs && millis() - savedAtMs > 2000) {
    lv_label_set_text(lblSaved, "");
    savedAtMs = 0;
  }
}

// ===========================================================================
//  Entry points
// ===========================================================================
static void buildOptionStrings()
{
  // lv_dropdown_set_options_static() keeps the pointer, so these must be
  // static storage that outlives the widget.
  modeOpts[0] = 0;
  for (uint8_t i = 0; i < CM_COUNT; i++) {
    if (i) strncat(modeOpts, "\n", sizeof(modeOpts) - strlen(modeOpts) - 1);
    strncat(modeOpts, CM_NAME[i], sizeof(modeOpts) - strlen(modeOpts) - 1);
  }
  fxOpts[0] = 0;
  for (uint8_t i = 0; i < FX_COUNT; i++) {
    if (i) strncat(fxOpts, "\n", sizeof(fxOpts) - strlen(fxOpts) - 1);
    strncat(fxOpts, FX_NAME[i], sizeof(fxOpts) - strlen(fxOpts) - 1);
  }
}

void uiBegin()
{
  Serial.begin(115200);
  manualDefaults();

  if (!displayBegin()) return;      // headless: serialStatus() takes over

  buildOptionStrings();

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);

  lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 58);
  lv_obj_set_style_bg_color(tv, COL_BG, 0);
  lv_obj_set_style_text_font(lv_tabview_get_tab_btns(tv), FONT_MED, 0);

  buildSetup (lv_tabview_add_tab(tv, "Setup"));
  buildManual(lv_tabview_add_tab(tv, "Manual"));
  buildStatus(lv_tabview_add_tab(tv, "Status"));

  lv_timer_create(refreshStatus, 250, NULL);
  displaySetBacklight(100);
}

void uiTask(uint32_t now)
{
  if (!displayReady()) { serialStatus(now); return; }
  (void)now;
  lv_timer_handler();
  delay(5);
}
#endif  // UI_HAS_LVGL
