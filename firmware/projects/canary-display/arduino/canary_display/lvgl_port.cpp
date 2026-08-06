// src/ui/lvgl_port.cpp — LVGL display glue, dual-major (v8.4 and v9.x).
//
// LVGL split its display/driver API with the 9.x line (lv_disp_drv_t ->
// lv_display_t, buffers in bytes, tick source registered at runtime); this
// port carries both behind LVGL_VERSION_MAJOR — the same pattern as
// hal/core_compat.h for the arduino-esp32 majors. The SPI-panel PlatformIO
// envs (watch/nightstand/touch169) pin 8.4; the RGB dash family and the
// Arduino core-3 profiles pair with the current 9.x (the dash family rides
// core 3 for its bounce buffers — see canary-display.ini), so a stock
// Library Manager install builds unmodified either way.
//
// Draw buffers: the watch keeps a quarter-screen buffer in internal RAM
// (240x60x2 = 28.8 KiB — a full GC9A01 frame pushes over SPI in ~23 ms at
// 40 MHz, and LVGL only flushes dirty regions anyway). The dash buffer
// lives in PSRAM (800x80x2 = 128 KiB); flushes memcpy into the RGB
// peripheral's scanned framebuffer, so only changed regions ever repaint —
// which is what makes the panel flicker-free by construction.
#include "flavor_config.h"
#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#ifdef CD_FLAVOR_DASH
#include <esp_heap_caps.h>
#endif

#include "pins.h"
#include "lvgl_port.h"
#include "display.h"
#include "glass_settings.h"  // Rotation, rotation_is_portrait
#include "log.h"

namespace canary::ui {

namespace {

// The nightstand (ST7789 172x320) shares the watch's internal-RAM partial
// buffer path: a small SPI panel with a static quarter-height buffer, no
// PSRAM required (the C6 has none). SCR_W/H come from its own TFT_WIDTH/HEIGHT.
#if defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)
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

// Live orientation state. Native = the panel's own scan dims (landscape);
// logical = what the UI draws in (axes swapped in portrait). The default is
// landscape, so every non-rotating flavor keeps the values it always had.
uint8_t s_rot = 0;                 // canary::glass::Rotation
int16_t s_logical_w = SCR_W;
int16_t s_logical_h = SCR_H;
lv_obj_t* s_scrim = nullptr;       // rendered brightness dim (top layer)

#if LVGL_VERSION_MAJOR >= 9

#if defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)
alignas(4) uint8_t s_buf[BUF_BYTES];
#endif

lv_display_t* s_disp = nullptr;    // captured for runtime rotation (v9)

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

#if defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)
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
#if defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)
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
  s_disp = disp;
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

// ── Orientation ──────────────────────────────────────────────────────────

void lvgl_port_set_rotation(uint8_t rot) {
  rot &= 3;
  s_rot = rot;
  // Recompute the logical canvas the UI lays out in. Native is always the
  // panel's landscape; portrait swaps the axes.
  int lw = SCR_W, lh = SCR_H;
  canary::glass::rotation_logical_dims(rot, SCR_W, SCR_H, &lw, &lh);
  s_logical_w = (int16_t)lw;
  s_logical_h = (int16_t)lh;

#if defined(CD_FLAVOR_DASH) && LVGL_VERSION_MAJOR >= 9
  // Only the RGB dash glass rotates; the round watch and the fixed-portrait
  // SPI nightstands ignore it. LVGL software-rotates the render into the
  // native framebuffer — the panel keeps scanning landscape.
  if (s_disp) {
    lv_display_rotation_t r = LV_DISPLAY_ROTATION_0;
    switch (rot) {
      case canary::glass::ROT_PORTRAIT:      r = LV_DISPLAY_ROTATION_90;  break;
      case canary::glass::ROT_LANDSCAPE_INV: r = LV_DISPLAY_ROTATION_180; break;
      case canary::glass::ROT_PORTRAIT_INV:  r = LV_DISPLAY_ROTATION_270; break;
      default:                               r = LV_DISPLAY_ROTATION_0;   break;
    }
    lv_display_set_rotation(s_disp, r);
  }
  // Un-rotate raw touch to match, so a tap lands in the logical frame.
  canary::hal::touch_set_rotation(rot, SCR_W, SCR_H);
#endif

  // Keep a live scrim covering the (possibly re-oriented) glass.
  if (s_scrim) lv_obj_set_size(s_scrim, 960, 960);
}

uint8_t lvgl_port_rotation() { return s_rot; }
int16_t lvgl_port_width() { return s_logical_w; }
int16_t lvgl_port_height() { return s_logical_h; }

#if defined(CD_NIGHTLIGHT) && LVGL_VERSION_MAJOR < 9
// The nightlight rotates in HARDWARE (hal display_set_rotation writes the
// panel's MADCTL), so unlike the dash path above there is nothing for LVGL
// to rotate — the driver just adopts the new logical shape and the face
// rebuilds into it. v8's lv_disp_drv_update refreshes the resolution on
// the live display without re-registering it.
void lvgl_port_set_panel_rotation(uint8_t rot) {
  rot &= 3;
  s_rot = rot;
  const bool turned = (rot & 1) != 0;
  s_logical_w = turned ? SCR_H : SCR_W;
  s_logical_h = turned ? SCR_W : SCR_H;
  s_disp_drv.hor_res = s_logical_w;
  s_disp_drv.ver_res = s_logical_h;
  lv_disp_t* d = lv_disp_get_default();
  if (d) lv_disp_drv_update(d, &s_disp_drv);
  if (s_scrim) lv_obj_set_size(s_scrim, 960, 960);
}
#endif  // CD_NIGHTLIGHT && v8

// ── Rendered brightness scrim ────────────────────────────────────────────

void lvgl_port_set_dim(uint8_t opa) {
  if (opa == 0 && !s_scrim) return;  // nothing to dim, nothing built yet
  if (!s_scrim) {
    // Top layer sits above every screen, including the settings sheet, so a
    // live drag previews on whatever is on the glass. It never eats a tap:
    // this project polls touch itself and hit-tests its own objects.
    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_size(s_scrim, 960, 960);  // overdraws both orientations; clipped
  }
  lv_obj_set_style_bg_opa(s_scrim, opa, 0);
}

}  // namespace canary::ui
