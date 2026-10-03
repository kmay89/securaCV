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
//
// A turned glass (F184): the panel keeps scanning its native landscape and
// LVGL hands every flush in that frame, already turned by its software
// rotation (lvgl_port_set_rotation: sw_rotate on the LVGL 8.4 this build
// pins). The page's canvas is the glass as a person in front of it reads
// it, so the framebuffer holds the TURNED frame — 480x800 for a portrait
// dash — and the page reads it as it reads any other glass.
//
// The nightlight turns in hardware instead (F204): display_set_rotation()
// writes the ST7789's MADCTL on the board, the panel re-addresses its own
// RAM, and lvgl_port_set_panel_rotation() only swaps LVGL's logical canvas,
// so LVGL renders nothing turned and every flush arrives in the logical
// frame. That frame is the glass as the person who stood the nightlight on
// its edge reads it — 320x180 for a landscape one — so this HAL holds the
// framebuffer in it and the page reads it the same way. What the MADCTL
// table does to the panel's RAM is silicon and stays display_1in47.cpp's
// (bench-verify, as that file says); this shows what the firmware intends.
#include "canary/hal/display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <string.h>
#include <stdlib.h>

#include <emscripten.h>

#include "emu_bus.h"
#include <config.h>  // flavor selector (CD_FLAVOR_*) before the geometry pick
#include "pins.h"
#ifdef CD_NIGHTLIGHT
#include "canary/log.h"  // imu_init's line, as the board's HAL says it
#endif

// Flavor geometry from the board pin map (same -I the firmware build uses).
#ifdef CD_FLAVOR_WATCH
#define EMU_W TFT_WIDTH
#define EMU_H TFT_HEIGHT
#else
#define EMU_W LCD_WIDTH
#define EMU_H LCD_HEIGHT
#endif

namespace {

// RGBA8888 framebuffer shared with JS (browser-native byte order), in the
// frame the viewer sees: EMU_W x EMU_H, or turned (see glass_turn below).
uint8_t* g_fb = nullptr;
uint32_t g_flush_count = 0;   // dirty-region flushes since boot (teaching!)
uint32_t g_dirty_serial = 0;  // bumped per flush; JS re-uploads on change

#ifdef CD_FLAVOR_WATCH
constexpr int kRoundMask = 1;
#else
constexpr int kRoundMask = 0;
#endif

// The quarter turns the framebuffer is held in (lv_disp_rot_t), and its
// size in that frame. Landscape, the size every flavor always had, until
// the firmware turns its glass.
int g_turn = LV_DISP_ROT_NONE;
int g_view_w = EMU_W;
int g_view_h = EMU_H;

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

// The turn LVGL rendered this flush in: its software rotation, read off the
// driver of the display being refreshed, so the framebuffer follows exactly
// what LVGL did to the pixels (none, until lvgl_port_set_rotation turns the
// dash glass). Outside a refresh (no flush comes from there today), the
// default display — this build's only one.
int lvgl_turn() {
  lv_disp_t* d = _lv_refr_get_disp_refreshing();
  if (d == nullptr) d = lv_disp_get_default();
  if (d == nullptr || d->driver == nullptr || !d->driver->sw_rotate) {
    return LV_DISP_ROT_NONE;
  }
  return (int)d->driver->rotated;
}

// The glass changed shape: hold the framebuffer in the new frame (cleared,
// as the firmware repaints the whole canvas on a turn) and tell the page,
// which sizes its canvas from js_display_ready.
void glass_turn(int turn) {
  g_turn = turn;
  const bool side = turn == LV_DISP_ROT_90 || turn == LV_DISP_ROT_270;
  g_view_w = side ? EMU_H : EMU_W;
  g_view_h = side ? EMU_W : EMU_H;
  for (size_t i = 0; i < (size_t)EMU_W * EMU_H; i++) {
    g_fb[i * 4 + 0] = 0;
    g_fb[i * 4 + 1] = 0;
    g_fb[i * 4 + 2] = 0;
    g_fb[i * 4 + 3] = 255;
  }
  js_display_ready(g_view_w, g_view_h, kRoundMask);
}

// A native panel pixel, where the viewer of the turned glass sees it: the
// inverse of LVGL 8.4's draw_buf_rotate (lv_refr.c), which puts the logical
// pixel (lx, ly) at native (ly, H-1-lx) for 90, (W-1-lx, H-1-ly) for 180 and
// (W-1-ly, lx) for 270, W x H being the panel's native landscape.
inline void native_to_view(int nx, int ny, int* vx, int* vy) {
  switch (g_turn) {
    case LV_DISP_ROT_90:  *vx = EMU_H - 1 - ny; *vy = nx;             break;
    case LV_DISP_ROT_180: *vx = EMU_W - 1 - nx; *vy = EMU_H - 1 - ny; break;
    case LV_DISP_ROT_270: *vx = ny;             *vy = EMU_W - 1 - nx; break;
    default:              *vx = nx;             *vy = ny;             break;
  }
}

// RGB565 (little-endian native, LV_COLOR_16_SWAP 0) → RGBA8888 with
// low-bit replication so pure white is pure white.
inline void put565(uint8_t* d, uint16_t c) {
  const uint8_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
  d[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
  d[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
  d[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
  d[3] = 255;
}

#ifdef CD_NIGHTLIGHT
// The nightlight's quarter turns (Arduino_GFX numbering, as
// display_set_rotation takes them): the frame the framebuffer is held in.
int g_panel_rot = 0;

// The panel turned in hardware: the framebuffer takes the logical frame's
// shape (sides swapped for 1 and 3), cleared to black as the board's HAL
// clears the panel on every turn (fillScreen after the MADCTL write), and
// the page is told the glass's new shape.
void panel_turn(int rot) {
  g_panel_rot = rot & 3;
  const bool side = (g_panel_rot & 1) != 0;
  g_view_w = side ? EMU_H : EMU_W;
  g_view_h = side ? EMU_W : EMU_H;
  for (size_t i = 0; i < (size_t)EMU_W * EMU_H; i++) {
    g_fb[i * 4 + 0] = 0;
    g_fb[i * 4 + 1] = 0;
    g_fb[i * 4 + 2] = 0;
    g_fb[i * 4 + 3] = 255;
  }
  js_display_ready(g_view_w, g_view_h, kRoundMask);
}
#endif

// One flush, in the panel's native frame (x, y, w, h as LVGL handed it).
inline void blit565(int16_t x, int16_t y, const uint16_t* src, int16_t w,
                    int16_t h) {
  if (!g_fb) return;
#ifdef CD_NIGHTLIGHT
  // Every flush is already in the logical frame the framebuffer holds (the
  // panel turns, LVGL does not): it lands where LVGL drew it, clipped to the
  // glass's current shape.
  for (int16_t row = 0; row < h; row++) {
    const int16_t fy = y + row;
    if (fy < 0 || fy >= g_view_h) continue;
    const uint16_t* s = src + (size_t)row * w;
    for (int16_t col = 0; col < w; col++, s++) {
      const int16_t fx = x + col;
      if (fx < 0 || fx >= g_view_w) continue;
      put565(g_fb + ((size_t)fy * g_view_w + fx) * 4, *s);
    }
  }
  return;
#endif
  const int turn = lvgl_turn();
  if (turn != g_turn) glass_turn(turn);
  for (int16_t row = 0; row < h; row++) {
    const int16_t fy = y + row;
    if (fy < 0 || fy >= EMU_H) continue;
    const uint16_t* s = src + (size_t)row * w;
    if (g_turn == LV_DISP_ROT_NONE) {
      uint8_t* d = g_fb + ((size_t)fy * EMU_W + x) * 4;
      for (int16_t col = 0; col < w; col++) {
        const int16_t fx = x + col;
        if (fx < 0 || fx >= EMU_W) { d += 4; s++; continue; }
        put565(d, *s++);
        d += 4;
      }
      continue;
    }
    for (int16_t col = 0; col < w; col++, s++) {
      const int16_t fx = x + col;
      if (fx < 0 || fx >= EMU_W) continue;
      int vx = 0, vy = 0;
      native_to_view(fx, fy, &vx, &vy);
      put565(g_fb + ((size_t)vy * g_view_w + vx) * 4, *s);
    }
  }
}

}  // namespace

// ── Arduino_GFX shim entry points (called by the real lvgl_port.cpp) ────
void Arduino_GFX::fillScreen(uint16_t color565) {
#ifdef CD_NIGHTLIGHT
  // The turned panel fills its turned shape (Arduino_GFX's own width and
  // height follow setRotation on the board).
  uint16_t row[EMU_W > EMU_H ? EMU_W : EMU_H];
  for (int i = 0; i < g_view_w; i++) row[i] = color565;
  for (int y = 0; y < g_view_h; y++) {
    blit565(0, (int16_t)y, row, (int16_t)g_view_w, 1);
  }
#else
  uint16_t row[EMU_W];
  for (int i = 0; i < EMU_W; i++) row[i] = color565;
  for (int y = 0; y < EMU_H; y++) blit565(0, (int16_t)y, row, EMU_W, 1);
#endif
  g_flush_count++;
  g_dirty_serial++;
  js_flush(0, 0, g_view_w, g_view_h, (int)g_dirty_serial);
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
  js_display_ready(g_view_w, g_view_h, kRoundMask);
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

#ifdef CD_NIGHTLIGHT
// The nightlight's hardware rotation (F204): on the board a MADCTL write
// that turns the panel's scan, then a black frame (display_1in47.cpp). Here
// the framebuffer takes the turned shape, cleared, and the page is told —
// main.cpp calls this before lvgl_port_set_panel_rotation(), which swaps
// LVGL's logical canvas to match, and LVGL's flushes then land in that frame.
void display_set_rotation(uint8_t rot) {
  if (!g_fb) return;  // the board's HAL also ignores a turn before the panel
  panel_turn(rot);
}

// No QMI8658 answers on the emulated bench's I2C bus (shim/Wire.h), so the
// firmware takes the board's no-IMU path: auto-orient stays off, and the
// glass keeps the rotation it booted with (staged before power-on by
// emu_preset_rotation, the way a unit that saved it would).
bool imu_init() {
  canary::log_line("IMU", "QMI8658 quiet (emulated bench) — auto-orient off, everything else fine.");
  return false;
}

bool imu_read_accel(int32_t* /*ax*/, int32_t* /*ay*/, int32_t* /*az*/) {
  return false;
}
#endif

#ifdef CD_FLAVOR_DASH
// The page's finger lands on the canvas, and the canvas shows the glass as
// its viewer sees it — turned with the firmware's rotation (glass_turn) — so
// a pointer sample is already in the logical frame touch_read() promises.
// There is no raw GT911 frame here to un-rotate (display_dash.cpp's job).
void touch_set_rotation(uint8_t /*rot*/, int16_t /*native_w*/,
                        int16_t /*native_h*/) {}
#endif

}  // namespace canary::hal

// ── C exports for the page (framebuffer + input + diagnostics) ──────────
extern "C" {

EMSCRIPTEN_KEEPALIVE uint8_t* emu_fb_ptr(void) { return g_fb; }
// The framebuffer's size in the frame it is held in (turned with the glass).
EMSCRIPTEN_KEEPALIVE int emu_fb_width(void) { return g_view_w; }
EMSCRIPTEN_KEEPALIVE int emu_fb_height(void) { return g_view_h; }
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
