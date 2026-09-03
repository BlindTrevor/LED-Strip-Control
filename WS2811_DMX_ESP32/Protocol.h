#pragma once
#include <stdint.h>

// ===========================================================================
//  Shared wire protocol: ESP32-S3 controller  ->  D1 Mini satellites, over I2C
//
//  KEEP THIS FILE IDENTICAL IN BOTH SKETCH FOLDERS.
//
//  Division of labour:
//    Controller  decodes DMX, runs the UI, and OWNS THE CLOCK. It has no free
//                GPIO for pixels, so it never renders.
//    Satellite   renders its own segment from the parameters it is sent.
//
//  The controller sending `phase` rather than letting each satellite keep its
//  own is the important bit: it makes a chase or comet run CONTINUOUSLY across
//  all three strips instead of three independent animations. Satellites also
//  hold their last message if one goes missing, so a dropped frame is invisible.
// ===========================================================================

#define PROTO_MAGIC_PARAMS  0xA5
#define PROTO_MAGIC_PIXELS  0xA6

#define SAT_I2C_BASE        0x30    // unit 0 = 0x30, unit 1 = 0x31, ...
#define SAT_MAX             3
#define TOTAL_PIXELS        100     // across every satellite, for pixel map
#define PIXELS_PER_BLOCK    30      // keeps a block under the 128 B Wire buffer

// --------------------------------------------------------------- effects ----
//  The DMX Mode channel divides 0..255 evenly across these six.
//  Bands: 0-42 / 43-85 / 86-127 / 128-170 / 171-213 / 214-255
enum Effect : uint8_t {
  FX_SOLID = 0, FX_BREATHE, FX_RAINBOW, FX_CHASE, FX_COMET, FX_SPARKLE, FX_COUNT
};

// ---------------------------------------------------------- DMX personalities ----
enum ChannelMode : uint8_t {
  CM_3CH = 0,   //  1 R  2 G  3 B
  CM_5CH,       //  1 Master  2 R 3 G 4 B  5 Strobe
  CM_9CH,       //  1 Master  2 Spectrum  3 FG int  4 BG spectrum  5 BG int
                //  6 Mode  7 Speed  8 Size  9 Strobe
  CM_11CH,      //  1 Master  2 R 3 G 4 B  5 BGR 6 BGG 7 BGB
                //  8 Mode  9 Speed  10 Size  11 Strobe
  CM_301CH,     //  1 Master  then 100 x R,G,B
  CM_COUNT
};
static const uint16_t CM_FOOTPRINT[CM_COUNT] = { 3, 5, 9, 11, 1 + TOTAL_PIXELS * 3 };

// ------------------------------------------------------------- messages ----
struct __attribute__((packed)) ParamsMsg {
  uint8_t  magic;      // PROTO_MAGIC_PARAMS
  uint8_t  seq;        // increments per send; satellites can spot gaps
  uint16_t phase;      // global animation phase, controller-owned
  uint8_t  master;     // strobe is already folded into this by the controller
  uint8_t  fgR, fgG, fgB;
  uint8_t  bgR, bgG, bgB;
  uint8_t  fx;         // Effect
  uint8_t  size;       // spread / spacing / density
  uint8_t  speed;      // sparkle decay only - motion comes from `phase`
};                     // 14 bytes

struct __attribute__((packed)) PixelMsg {
  uint8_t magic;       // PROTO_MAGIC_PIXELS
  uint8_t seq;
  uint8_t start;       // first pixel index WITHIN the satellite's segment
  uint8_t count;       // pixels in this block, <= PIXELS_PER_BLOCK
  uint8_t rgb[PIXELS_PER_BLOCK * 3];
};
#define PIXELMSG_LEN(n) (4 + (n) * 3)
