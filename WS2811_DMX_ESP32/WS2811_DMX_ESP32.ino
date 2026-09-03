/*
 *  DMX512 -> I2C -> WS2811 satellites
 *  Waveshare ESP32-S3-Touch-LCD-4.3B  (controller / UI / clock)
 *
 *  Decodes DMX, owns the animation phase, and pushes parameters to the D1 Mini
 *  satellites over the board's I2C terminal. Never drives a strip itself.
 *
 *  The link task is pinned to core 0 so an 800x480 LVGL redraw on core 1 can
 *  never stall the output.
 *
 *  Libraries:  esp_dmx >= 4.1 (someweisguy)   Preferences (bundled)
 */

#include <Wire.h>
#include <Preferences.h>
#include <esp_dmx.h>
#include "Protocol.h"
#include "config.h"
#include "state.h"

// ------------------------------------------------------------- globals ----
Config   cfg;
Params   manual;
Params   live;
bool     dmxAlive      = false;
uint32_t dmxFrameCount = 0;
uint32_t i2cErrors     = 0;

static uint8_t     dmxData[DMX_PACKET_SIZE];   // [0] is the start code
static uint32_t    lastDmxMs = 0;
static Preferences prefs;
static uint16_t    gPhase = 0;
static uint8_t     gSeq   = 0;

// ------------------------------------------------------------ settings ----
void settingsClamp()
{
  if (cfg.mode >= CM_COUNT) cfg.mode = CM_11CH;
  uint16_t maxAddr = 513 - CM_FOOTPRINT[cfg.mode];
  if (cfg.address < 1)       cfg.address = 1;
  if (cfg.address > maxAddr) cfg.address = maxAddr;
}

void settingsLoad()
{
  prefs.begin("ledctl", true);
  cfg.address    = prefs.getUShort("addr", 1);
  cfg.mode       = prefs.getUChar("mode", CM_11CH);
  cfg.standalone = prefs.getBool("alone", false);
  prefs.end();

  settingsClamp();
}

void settingsSave()
{
  prefs.begin("ledctl", false);
  prefs.putUShort("addr", cfg.address);
  prefs.putUChar("mode", cfg.mode);
  prefs.putBool("alone", cfg.standalone);
  prefs.end();
}

// ----------------------------------------------------------- DMX decode ----
static inline uint8_t dch(uint16_t offset)     // offset 0 == start address
{
  uint16_t c = cfg.address + offset;
  return (c <= 512) ? dmxData[c] : 0;
}

// Seven evenly spaced stops with smooth interpolation between them:
//
//     0 Red   ·   43 Yellow  ·   85 Green  ·  128 Cyan
//   170 Blue  ·  213 Magenta ·  255 White
//
// The final leg (magenta -> white) desaturates, which is what brings white
// within reach of a single channel. Stops fall every 255/6 = 42.5, so the
// nominal values above are +/-1.
static constexpr Rgb SPECTRUM_STOPS[7] = {
  { 255,   0,   0 },   //   0  red
  { 255, 255,   0 },   //  43  yellow
  {   0, 255,   0 },   //  85  green
  {   0, 255, 255 },   // 128  cyan
  {   0,   0, 255 },   // 170  blue
  { 255,   0, 255 },   // 213  magenta
  { 255, 255, 255 },   // 255  white
};

static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t)
{
  return (uint8_t)((int16_t)a + (((int16_t)b - (int16_t)a) * (int16_t)t) / 255);
}

static Rgb spectrum(uint8_t v)
{
  uint16_t scaled = (uint16_t)v * 6;            // 0..1530 across six legs
  uint8_t  leg    = (uint8_t)(scaled / 255);
  uint8_t  t      = (uint8_t)(scaled % 255);
  if (leg >= 6) { leg = 5; t = 255; }           // clamp 255 to exact white

  const Rgb &a = SPECTRUM_STOPS[leg];
  const Rgb &b = SPECTRUM_STOPS[leg + 1];
  Rgb o;
  o.r = lerp8(a.r, b.r, t);
  o.g = lerp8(a.g, b.g, t);
  o.b = lerp8(a.b, b.b, t);
  return o;
}

static inline Rgb scaleRgb(Rgb c, uint8_t s)
{
  Rgb o;
  o.r = (uint8_t)(((uint16_t)c.r * s) >> 8);
  o.g = (uint8_t)(((uint16_t)c.g * s) >> 8);
  o.b = (uint8_t)(((uint16_t)c.b * s) >> 8);
  return o;
}

static inline uint8_t fxFromChannel(uint8_t v)
{
  return (uint8_t)(((uint16_t)v * FX_COUNT) >> 8);   // 0..5, bands of 43
}

static void decodeDmx(Params &p)
{
  switch (cfg.mode) {
    case CM_3CH:
      p.master = 255;
      p.fg = { dch(0), dch(1), dch(2) };
      p.bg = {};  p.fx = FX_SOLID;  p.strobe = 0;
      break;

    case CM_5CH:
      p.master = dch(0);
      p.fg = { dch(1), dch(2), dch(3) };
      p.bg = {};  p.fx = FX_SOLID;  p.strobe = dch(4);
      break;

    case CM_9CH:
      p.master = dch(0);
      p.fg     = scaleRgb(spectrum(dch(1)), dch(2));
      p.bg     = scaleRgb(spectrum(dch(3)), dch(4));
      p.fx     = fxFromChannel(dch(5));
      p.speed  = dch(6);
      p.size   = dch(7);
      p.strobe = dch(8);
      break;

    case CM_11CH:
      p.master = dch(0);
      p.fg = { dch(1), dch(2), dch(3) };
      p.bg = { dch(4), dch(5), dch(6) };
      p.fx     = fxFromChannel(dch(7));
      p.speed  = dch(8);
      p.size   = dch(9);
      p.strobe = dch(10);
      break;

    case CM_301CH:
      p.master = dch(0);
      break;                    // pixel data forwarded verbatim
  }
}

// -------------------------------------------------------------- I2C link ----
static void sendParams(const Params &p, uint16_t phase, uint8_t master)
{
  ParamsMsg m;
  m.magic  = PROTO_MAGIC_PARAMS;
  m.seq    = gSeq;
  m.phase  = phase;
  m.master = master;
  m.fgR = p.fg.r; m.fgG = p.fg.g; m.fgB = p.fg.b;
  m.bgR = p.bg.r; m.bgG = p.bg.g; m.bgB = p.bg.b;
  m.fx    = p.fx;
  m.size  = p.size;
  m.speed = p.speed;

  for (uint8_t s = 0; s < SAT_COUNT; s++) {
    Wire.beginTransmission(SATS[s].addr);
    Wire.write((const uint8_t *)&m, sizeof(m));
    if (Wire.endTransmission() != 0) i2cErrors++;
  }
}

// Slices the DMX pixel block per satellite, chunked to stay inside the Wire
// buffer. Send AFTER sendParams(), which is what carries the master level.
static void sendPixels()
{
  for (uint8_t s = 0; s < SAT_COUNT; s++) {
    uint8_t sent = 0;
    while (sent < SATS[s].pixels) {
      uint8_t n = SATS[s].pixels - sent;
      if (n > PIXELS_PER_BLOCK) n = PIXELS_PER_BLOCK;

      PixelMsg m;
      m.magic = PROTO_MAGIC_PIXELS;
      m.seq   = gSeq;
      m.start = sent;
      m.count = n;
      for (uint8_t i = 0; i < n; i++) {
        uint16_t g = (uint16_t)SATS[s].offset + sent + i;   // global index
        uint16_t o = 1 + g * 3;                             // ch1 is master
        m.rgb[i * 3 + 0] = dch(o);
        m.rgb[i * 3 + 1] = dch(o + 1);
        m.rgb[i * 3 + 2] = dch(o + 2);
      }
      Wire.beginTransmission(SATS[s].addr);
      Wire.write((const uint8_t *)&m, PIXELMSG_LEN(n));
      if (Wire.endTransmission() != 0) i2cErrors++;
      sent += n;
    }
  }
}

// ------------------------------------------------------------- link task ----
static void linkTask(void *)
{
  uint32_t lastMs = millis();
  TickType_t next = xTaskGetTickCount();

  for (;;) {
    dmx_packet_t pkt;
    while (dmx_receive(DMX_UART_NUM, &pkt, 0)) {          // drain the queue
      if (!pkt.err && pkt.sc == DMX_SC) {
        dmx_read(DMX_UART_NUM, dmxData, pkt.size);
        lastDmxMs = millis();
        dmxFrameCount++;
      }
    }

    uint32_t now = millis();
    dmxAlive = (lastDmxMs != 0) && (now - lastDmxMs < DMX_TIMEOUT_MS);

    bool fromDmx = dmxAlive && !cfg.standalone;
    if (fromDmx) decodeDmx(live);
    else         live = manual;

    // The controller owns the clock. Satellites derive every position from
    // this phase, which is what keeps a chase continuous across all strips.
    uint16_t dt = (uint16_t)(now - lastMs);  lastMs = now;
    gPhase += (uint16_t)(((uint32_t)dt * (1 + live.speed)) >> 4);

    // Strobe is applied centrally so all satellites blink together. Its
    // resolution is bounded by the send rate: fine to ~10 Hz, coarse above.
    uint8_t master = live.master;
    if (live.strobe >= 8) {
      uint16_t period = 500 - (uint16_t)(((uint32_t)(live.strobe - 8) * 460) / 247);
      if ((now % period) > period / 3) master = 0;
    }

    gSeq++;
    bool pixels = fromDmx && cfg.mode == CM_301CH;
    sendParams(live, gPhase, master);
    if (pixels) sendPixels();

    vTaskDelayUntil(&next, pdMS_TO_TICKS(1000 / (pixels ? SEND_HZ_PIXELS : SEND_HZ)));
  }
}

// --------------------------------------------------------------- arduino ----
void setup()
{
  settingsLoad();

  // One bus, four things on it: the satellites, the GT911 and the CH422G. Open
  // it here, once, BEFORE uiBegin() - panelInit() in display.cpp is documented
  // to reuse this bus rather than call Wire.begin() a second time, which would
  // reset it and drop the satellites.
  Wire.begin(I2C_SDA, I2C_SCL, I2C_HZ);

  dmx_config_t dmxCfg = DMX_CONFIG_DEFAULT;
  dmx_driver_install(DMX_UART_NUM, &dmxCfg, NULL, 0);
  dmx_set_pin(DMX_UART_NUM, DMX_TX_PIN, DMX_RX_PIN, DMX_RTS_PIN);

  uiBegin();

  xTaskCreatePinnedToCore(linkTask, "link", 4096, NULL, 5, NULL, 0);
}

void loop()
{
  uiTask(millis());
}
