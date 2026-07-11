// src/ui/lvgl_port.cpp — LVGL display glue, dual-major (v8.4 and v9.x).
//
// LVGL split its display/driver API with the 9.x line (lv_disp_drv_t ->
// lv_display_t, buffers in bytes, tick source registered at runtime); this
// port carries both behind LVGL_VERSION_MAJOR — the same pattern as
// hal/core_compat.h for the arduino-esp32 majors. The PlatformIO release
// path pins 8.4 (bench parity); the Arduino core-3 profiles pair with the
// current 9.x so a stock Library Manager install builds unmodified.
//
// Draw buffers: the watch keeps a quarter-screen buffer in internal RAM
// (240x60x2 = 28.8 KiB — a full GC9A01 frame pushes over SPI in ~23 ms at
// 40 MHz, and LVGL only flushes dirty regions anyway). The dash buffer
// lives in PSRAM (800x80x2 = 128 KiB); flushes memcpy into the RGB
// peripheral's scanned framebuffer, so only changed regions ever repaint —
// which is what makes the panel flicker-free by construction.
#include <config.h>
#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#ifdef CD_FLAVOR_DASH
#include <esp_heap_caps.h>
#endif

#include "pins.h"
#include "canary/ui/lvgl_port.h"
#include "canary/hal/display.h"
#include "canary/log.h"

namespace canary::ui {

namespace {

#ifdef CD_FLAVOR_WATCH
constexpr int16_t SCR_W = TFT_WIDTH;
constexpr int16_t SCR_H = TFT_HEIGHT;
constexpr size_t BUF_PX = (size_t)SCR_W * 60;
#endif
#ifdef CD_FLAVOR_DASH
constexpr int16_t SCR_W = LCD_WIDTH;
constexpr int16_t SCR_H = LCD_HEIGHT;
constexpr size_t BUF_PX = (size_t)SCR_W * 80;
#endif

// RGB565 either way; v8 sizes buffers in lv_color_t (2 bytes at depth 16),
// v9 in raw bytes. The panel path is identical: LVGL renders little-endian
// RGB565 (v8: LV_COLOR_16_SWAP 0), GFX blits it verbatim.
constexpr size_t BUF_BYTES = BUF_PX * 2;

#if LVGL_VERSION_MAJOR >= 9

#ifdef CD_FLAVOR_WATCH
alignas(4) uint8_t s_buf[BUF_BYTES];
#endif

uint32_t tick_cb() { return millis(); }

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  Arduino_GFX* g = canary::hal::gfx();
  if (g) {
    const int16_t w = (int16_t)(area->x2 - area->x1 + 1);
    const int16_t h = (int16_t)(area->y2 - area->y1 + 1);
    g->draw16bitRGBBitmap(area->x1, area->y1,
                          reinterpret_cast<uint16_t*>(px_map), w, h);
  }
  lv_display_flush_ready(disp);
}

#else  // LVGL v8

#ifdef CD_FLAVOR_WATCH
lv_color_t s_buf[BUF_PX];
#endif

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  Arduino_GFX* g = canary::hal::gfx();
  if (g) {
    const int16_t w = (int16_t)(area->x2 - area->x1 + 1);
    const int16_t h = (int16_t)(area->y2 - area->y1 + 1);
    g->draw16bitRGBBitmap(area->x1, area->y1,
                          reinterpret_cast<uint16_t*>(px), w, h);
  }
  lv_disp_flush_ready(drv);
}

#endif  // LVGL_VERSION_MAJOR

}  // namespace

bool lvgl_port_init() {
  lv_init();

  void* buf = nullptr;
  size_t buf_bytes = 0;
#ifdef CD_FLAVOR_WATCH
  buf = s_buf;
  buf_bytes = BUF_BYTES;
#endif
#ifdef CD_FLAVOR_DASH
  buf = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM);
  buf_bytes = BUF_BYTES;
  if (!buf) {
    // PSRAM missing/hostile: shrink into internal RAM rather than dying —
    // slower flushes, same pixels.
    buf_bytes = (size_t)SCR_W * 16 * 2;
    buf = malloc(buf_bytes);
  }
#endif
  if (!buf) {
    canary::log_line("LVGL", "Draw buffer allocation FAILED — UI disabled.");
    return false;
  }

#if LVGL_VERSION_MAJOR >= 9
  // v9 dropped the compile-time custom tick; register millis at runtime
  // BEFORE anything can call lv_timer_handler.
  lv_tick_set_cb(tick_cb);
  lv_display_t* disp = lv_display_create(SCR_W, SCR_H);
  if (!disp) {
    canary::log_line("LVGL", "lv_display_create FAILED — UI disabled.");
    return false;
  }
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, buf, nullptr, (uint32_t)buf_bytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
  lv_disp_draw_buf_init(&s_draw_buf, (lv_color_t*)buf, nullptr,
                        (uint32_t)(buf_bytes / sizeof(lv_color_t)));
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = SCR_W;
  s_disp_drv.ver_res = SCR_H;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);
#endif

  canary::log_line("LVGL", "Renderer up (dirty-region, anti-aliased).");
  return true;
}

}  // namespace canary::ui
