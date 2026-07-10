// src/ui/lvgl_port.cpp — LVGL v8 display glue.
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
#include "log.h"

namespace canary::ui {

namespace {

#ifdef CD_FLAVOR_WATCH
constexpr int16_t SCR_W = TFT_WIDTH;
constexpr int16_t SCR_H = TFT_HEIGHT;
constexpr size_t BUF_PX = (size_t)SCR_W * 60;
lv_color_t s_buf[BUF_PX];
#endif
#ifdef CD_FLAVOR_DASH
constexpr int16_t SCR_W = LCD_WIDTH;
constexpr int16_t SCR_H = LCD_HEIGHT;
constexpr size_t BUF_PX = (size_t)SCR_W * 80;
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

}  // namespace

bool lvgl_port_init() {
  lv_init();

  lv_color_t* buf = nullptr;
  size_t buf_px = 0;
#ifdef CD_FLAVOR_WATCH
  buf = s_buf;
  buf_px = BUF_PX;
#endif
#ifdef CD_FLAVOR_DASH
  buf = (lv_color_t*)heap_caps_malloc(BUF_PX * sizeof(lv_color_t),
                                      MALLOC_CAP_SPIRAM);
  buf_px = BUF_PX;
  if (!buf) {
    // PSRAM missing/hostile: shrink into internal RAM rather than dying —
    // slower flushes, same pixels.
    buf_px = (size_t)SCR_W * 16;
    buf = (lv_color_t*)malloc(buf_px * sizeof(lv_color_t));
  }
#endif
  if (!buf) {
    canary::log_line("LVGL", "Draw buffer allocation FAILED — UI disabled.");
    return false;
  }

  lv_disp_draw_buf_init(&s_draw_buf, buf, nullptr, (uint32_t)buf_px);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = SCR_W;
  s_disp_drv.ver_res = SCR_H;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);

  canary::log_line("LVGL", "Renderer up (dirty-region, anti-aliased).");
  return true;
}

}  // namespace canary::ui
