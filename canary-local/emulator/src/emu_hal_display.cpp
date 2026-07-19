// canary-local/emulator/src/emu_hal_display.cpp — glass made of pixels.
//
// Implements the canary::hal display contract (display.h) for the wasm
// build, replacing src/hal/display_watch.cpp / display_dash.cpp — the
// only two firmware files whose job is silicon. Everything the UI ever
// does still flows through the REAL lvgl_port.cpp: LVGL renders dirty
// regions and flushes them through Arduino_GFX::draw16bitRGBBitmap,
// which lands here and becomes RGBA pixels the page textures onto the
// 3D device's screen. Backlight intent (day PWM ladder, 13-bit night
// floor) is forwarded to JS so the on-screen glass really dims.
#include "canary/hal/display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdlib.h>

#include <emscripten.h>

#include "emu_bus.h"
#include <config.h>  // flavor selector (CD_FLAVOR_*) before the geometry pick
#include "pins.h"

// Flavor geometry from the board pin map (same -I the firmware build uses).
#ifdef CD_FLAVOR_WATCH
#define EMU_W TFT_WIDTH
#define EMU_H TFT_HEIGHT
#else
#define EMU_W LCD_WIDTH
#define EMU_H LCD_HEIGHT
#endif

namespace {

// RGBA8888 framebuffer shared with JS (browser-native byte order).
uint8_t* g_fb = nullptr;
uint32_t g_flush_count = 0;   // dirty-region flushes since boot (teaching!)
uint32_t g_dirty_serial = 0;  // bumped per flush; JS re-uploads on change

// Touch state pushed by JS pointer events, drained by touch_read() each
// loop pass — the same poll cadence the CST816S/GT911 get on hardware.
volatile int g_touch_down = 0;
volatile int g_touch_x = 0;
volatile int g_touch_y = 0;

// Backlight intent as last applied by the firmware's brightness policy.
volatile int g_backlight_level = 255;   // 0..255 day ladder
volatile int g_night_duty13 = -1;       // 0..8191 when night profile owns it

EM_JS(void, js_display_ready, (int w, int h, int round_mask), {
  if (Module.onDisplayReady) Module.onDisplayReady(w, h, !!round_mask);
});
EM_JS(void, js_flush, (int x, int y, int w, int h, int serial), {
  if (Module.onFlush) Module.onFlush(x, y, w, h, serial);
});
EM_JS(void, js_backlight_apply, (int level, int duty13), {
  if (Module.onBacklight) Module.onBacklight(level, duty13);
});

class EmuGFX : public Arduino_GFX {
 public:
  EmuGFX() : Arduino_GFX(EMU_W, EMU_H) {}
};

EmuGFX g_gfx;

inline void blit565(int16_t x, int16_t y, const uint16_t* src, int16_t w,
                    int16_t h) {
  if (!g_fb) return;
  for (int16_t row = 0; row < h; row++) {
    const int16_t fy = y + row;
    if (fy < 0 || fy >= EMU_H) continue;
    const uint16_t* s = src + (size_t)row * w;
    uint8_t* d = g_fb + ((size_t)fy * EMU_W + x) * 4;
    for (int16_t col = 0; col < w; col++) {
      const int16_t fx = x + col;
      if (fx < 0 || fx >= EMU_W) { d += 4; s++; continue; }
      const uint16_t c = *s++;
      // RGB565 (little-endian native, LV_COLOR_16_SWAP 0) → RGBA8888 with
      // low-bit replication so pure white is pure white.
      const uint8_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
      d[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
      d[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
      d[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
      d[3] = 255;
      d += 4;
    }
  }
}

}  // namespace

// ── Arduino_GFX shim entry points (called by the real lvgl_port.cpp) ────
void Arduino_GFX::fillScreen(uint16_t color565) {
  uint16_t row[EMU_W];
  for (int i = 0; i < EMU_W; i++) row[i] = color565;
  for (int y = 0; y < EMU_H; y++) blit565(0, (int16_t)y, row, EMU_W, 1);
  g_flush_count++;
  g_dirty_serial++;
  js_flush(0, 0, EMU_W, EMU_H, (int)g_dirty_serial);
}

void Arduino_GFX::draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap,
                                     int16_t w, int16_t h) {
  blit565(x, y, bitmap, w, h);
  g_flush_count++;
  g_dirty_serial++;
  js_flush(x, y, w, h, (int)g_dirty_serial);
}

// ── canary::hal contract ────────────────────────────────────────────────
namespace canary::hal {

bool display_init() {
  if (!g_fb) {
    g_fb = (uint8_t*)calloc((size_t)EMU_W * EMU_H, 4);
    if (!g_fb) return false;
    // Panel powers up dark, alpha opaque.
    for (size_t i = 0; i < (size_t)EMU_W * EMU_H; i++) g_fb[i * 4 + 3] = 255;
  }
#ifdef CD_FLAVOR_WATCH
  js_display_ready(EMU_W, EMU_H, 1);
#else
  js_display_ready(EMU_W, EMU_H, 0);
#endif
  return true;
}

Arduino_GFX* gfx() { return &g_gfx; }

void display_flush() {}

void backlight_set(uint8_t level) {
  g_backlight_level = level;
  g_night_duty13 = -1;
  js_backlight_apply(level, -1);
}

void backlight_night_set(uint16_t duty13) {
  g_night_duty13 = duty13;
  js_backlight_apply(-1, duty13);
}

TouchSample touch_read() {
  TouchSample s;
  s.touched = g_touch_down != 0;
  s.x = (int16_t)g_touch_x;
  s.y = (int16_t)g_touch_y;
  return s;
}

}  // namespace canary::hal

// ── C exports for the page (framebuffer + input + diagnostics) ──────────
extern "C" {

EMSCRIPTEN_KEEPALIVE uint8_t* emu_fb_ptr(void) { return g_fb; }
EMSCRIPTEN_KEEPALIVE int emu_fb_width(void) { return EMU_W; }
EMSCRIPTEN_KEEPALIVE int emu_fb_height(void) { return EMU_H; }
EMSCRIPTEN_KEEPALIVE int emu_fb_serial(void) { return (int)g_dirty_serial; }
EMSCRIPTEN_KEEPALIVE int emu_flush_count(void) { return (int)g_flush_count; }
EMSCRIPTEN_KEEPALIVE int emu_backlight_level(void) { return g_backlight_level; }
EMSCRIPTEN_KEEPALIVE int emu_backlight_night_duty(void) { return g_night_duty13; }

EMSCRIPTEN_KEEPALIVE void emu_touch(int down, int x, int y) {
  g_touch_down = down;
  g_touch_x = x;
  g_touch_y = y;
}

void emu_bus_ledc_write(uint8_t /*channel*/, uint32_t /*duty*/,
                        uint32_t /*freq*/, uint8_t /*res_bits*/) {
  // Backlight PWM arrives through the HAL entry points above on this
  // build (display_watch.cpp, the LEDC caller, is replaced); chime tones
  // go through ledcWriteTone directly. Nothing further to route.
}

}  // extern "C"
