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
// which is what makes the panel flicker-free by construction. On LVGL 9 the
// dash takes a second buffer that size for turning a flushed area (see
// flush_cb); LVGL 8 turns areas itself (sw_rotate).
#include <config.h>
#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#if defined(CD_FLAVOR_DASH) || defined(CD_AMOLED_GLASS)
#include <esp_heap_caps.h>
#endif

#include "pins.h"
#include "canary/ui/lvgl_port.h"
#include "canary/hal/display.h"
#include "canary/glass_settings.h"  // Rotation, rotation_is_portrait
#include "canary/log.h"

namespace canary::ui {

namespace {

// The nightstand (ST7789 172x320) shares the watch's internal-RAM partial
// buffer path: a small SPI panel with a static quarter-height buffer, no
// PSRAM required (the C6 has none). SCR_W/H come from its own TFT_WIDTH/HEIGHT.
#if defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)
constexpr int16_t SCR_W = TFT_WIDTH;
constexpr int16_t SCR_H = TFT_HEIGHT;
#ifdef CD_AMOLED_GLASS
// The 450x600 AMOLED is the one nightstand-family glass big enough that a
// static internal buffer would crowd .bss (450*60*2 = 54 KB). It carries
// 8 MB PSRAM, so it takes the dash's arrangement instead: a taller PSRAM
// draw buffer (allocated in lvgl_port_init), internal-RAM fallback.
constexpr size_t BUF_PX = (size_t)SCR_W * 120;
#else
constexpr size_t BUF_PX = (size_t)SCR_W * 60;
#endif
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

// The last touch sample main.cpp fed for the LVGL pointer device (see
// lvgl_port_touch_feed). Released until a surface asks for input.
bool    s_in_down = false;
int16_t s_in_x = 0;
int16_t s_in_y = 0;

void fill_indev_data(lv_indev_data_t* data) {
  int x = s_in_x, y = s_in_y;
#if defined(CD_FLAVOR_DASH)
  // LVGL rotates every pointer sample by the display rotation itself (v9:
  // lv_display_rotate_point; v8: indev_pointer_proc, the same arithmetic in
  // native panel dims). The HAL already handed us the logical point, so hand
  // LVGL the native-frame point whose rotation IS that logical point — the
  // exact inverse, host-tested in test_display_settings.cpp against both.
  if (s_rot != 0) {
    canary::glass::rotation_to_lvgl_indev(s_rot, SCR_W, SCR_H, s_in_x, s_in_y,
                                          &x, &y);
  }
#endif
  data->point.x = x;
  data->point.y = y;
  data->state = s_in_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

#if LVGL_VERSION_MAJOR >= 9

#if (defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)) && \
    !defined(CD_AMOLED_GLASS)
alignas(4) uint8_t s_buf[BUF_BYTES];
#endif

lv_display_t* s_disp = nullptr;    // captured for runtime rotation (v9)

#ifdef CD_FLAVOR_DASH
// LVGL 9 turns nothing it renders: lv_display_set_rotation() only swaps the
// logical resolution (main-modules/display/rotation.rst), so in partial mode
// every area reaches flush_cb as a LOGICAL area over a logical-width buffer.
// The RGB framebuffer is the panel's native landscape, so flush_cb turns
// each area into it the way the docs' partial-mode example does —
// lv_display_rotate_area() for where it lands, lv_draw_sw_rotate() for its
// pixels — through this second buffer. A turned area holds the same pixels
// as the area LVGL rendered, so the draw buffer's size is enough; it is
// allocated beside the draw buffer (lvgl_port_init). Without it the glass
// stays landscape (lvgl_port_set_rotation refuses a turn it cannot draw).
uint8_t* s_turn_buf = nullptr;
size_t s_turn_bytes = 0;
// flush_cb blits rows packed (stride == width * 2), turned or not; LVGL's
// partial-mode buffers are packed only while layers keep stride alignment 1.
static_assert(LV_DRAW_BUF_STRIDE_ALIGN <= 1,
              "flush_cb assumes packed RGB565 rows (LV_DRAW_BUF_STRIDE_ALIGN 1)");
#endif

uint32_t tick_cb() { return millis(); }

void indev_read_cb(lv_indev_t*, lv_indev_data_t* data) { fill_indev_data(data); }

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  Arduino_GFX* g = canary::hal::gfx();
  if (g) {
    lv_area_t a = *area;
    uint8_t* px = px_map;
#ifdef CD_FLAVOR_DASH
    const lv_display_rotation_t rot = lv_display_get_rotation(disp);
    if (rot != LV_DISPLAY_ROTATION_0) {
      // Logical (lx, ly) lands on the panel at 90: (ly, H-1-lx), 180:
      // (W-1-lx, H-1-ly), 270: (W-1-ly, lx) — the turn rotation_map_touch()
      // inverts, so a tap lands on what was drawn under it.
      const lv_color_format_t cf = lv_display_get_color_format(disp);
      const int32_t src_w = lv_area_get_width(area);
      const int32_t src_h = lv_area_get_height(area);
      lv_display_rotate_area(disp, &a);
      const uint32_t src_stride = lv_draw_buf_width_to_stride((uint32_t)src_w, cf);
      const uint32_t dst_stride =
          lv_draw_buf_width_to_stride((uint32_t)lv_area_get_width(&a), cf);
      if (!s_turn_buf ||
          (size_t)dst_stride * (size_t)lv_area_get_height(&a) > s_turn_bytes) {
        // Not reachable (no turn without the buffer, and no area larger than
        // the draw buffer it matches); blitting unturned would corrupt more.
        lv_display_flush_ready(disp);
        return;
      }
      lv_draw_sw_rotate(px_map, s_turn_buf, src_w, src_h, (int32_t)src_stride,
                        (int32_t)dst_stride, rot, cf);
      px = s_turn_buf;
    }
#endif
    const int16_t w = (int16_t)(a.x2 - a.x1 + 1);
    const int16_t h = (int16_t)(a.y2 - a.y1 + 1);
    g->draw16bitRGBBitmap((int16_t)a.x1, (int16_t)a.y1,
                          reinterpret_cast<uint16_t*>(px), w, h);
  }
  lv_display_flush_ready(disp);
}

#else  // LVGL v8

#if (defined(CD_FLAVOR_WATCH) || defined(CD_FLAVOR_NIGHTSTAND)) && \
    !defined(CD_AMOLED_GLASS)
lv_color_t s_buf[BUF_PX];
#endif

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_indev_drv_t s_indev_drv;

void indev_read_cb(lv_indev_drv_t*, lv_indev_data_t* data) {
  fill_indev_data(data);
}

#ifdef CD_AMOLED_GLASS
// RM690B0 QSPI address windows want even column/row starts and even sizes
// (the same 2-px granularity the CO5300 family documents). Rounding every
// dirty area outward to even bounds here — with the even 16-px panel
// window offset in the HAL — keeps each flush legal without the face ever
// knowing. Costs at most one extra pixel row/column per flush.
void rounder_cb(lv_disp_drv_t* /*drv*/, lv_area_t* a) {
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}
#endif

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
#ifdef CD_AMOLED_GLASS
  // The 450x600 glass takes the dash's PSRAM arrangement (see BUF_PX note).
  buf = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM);
  buf_bytes = BUF_BYTES;
  if (!buf) {
    // PSRAM missing/hostile: shrink into internal RAM rather than dying —
    // slower flushes, same pixels.
    buf_bytes = (size_t)SCR_W * 24 * 2;
    buf = malloc(buf_bytes);
  }
#else
  buf = s_buf;
  buf_bytes = BUF_BYTES;
#endif
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
#if defined(CD_FLAVOR_DASH) && LVGL_VERSION_MAJOR >= 9
  // flush_cb's turned copy (see s_turn_buf): the draw buffer's size, from
  // the same heap tier — 128,000 B more PSRAM of the glass's 8 MB, or, on
  // the internal fallback above, 25,600 B more internal RAM.
  s_turn_buf = (uint8_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
  if (!s_turn_buf) {
    s_turn_buf = (uint8_t*)heap_caps_malloc(
        buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  s_turn_bytes = s_turn_buf ? buf_bytes : 0;
  if (!s_turn_buf) {
    canary::log_line("LVGL", "Rotation buffer allocation FAILED — the glass stays landscape.");
  }
#endif

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
  // The fed pointer device (see lvgl_port_touch_feed). Registered on every
  // flavor: it reports "released" until a surface feeds it, so a face that
  // never feeds never sees an event.
  lv_indev_t* indev = lv_indev_create();
  if (indev) {
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_display(indev, disp);
  }
#else
  lv_disp_draw_buf_init(&s_draw_buf, (lv_color_t*)buf, nullptr,
                        (uint32_t)(buf_bytes / sizeof(lv_color_t)));
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = SCR_W;
  s_disp_drv.ver_res = SCR_H;
  s_disp_drv.flush_cb = flush_cb;
#ifdef CD_AMOLED_GLASS
  s_disp_drv.rounder_cb = rounder_cb;  // even-aligned windows (QSPI AMOLED)
#endif
  s_disp_drv.draw_buf = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);
  // The fed pointer device (see lvgl_port_touch_feed) — released until a
  // surface feeds it, so the faces never see an LVGL event.
  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);
#endif

  canary::log_line("LVGL", "Renderer up (dirty-region, anti-aliased).");
  return true;
}

// ── Orientation ──────────────────────────────────────────────────────────

void lvgl_port_set_rotation(uint8_t rot) {
  rot &= 3;
#if defined(CD_FLAVOR_DASH) && LVGL_VERSION_MAJOR >= 9
  // A turn flush_cb cannot draw would lay a turned face out on an unturned
  // glass and un-rotate every tap: stay landscape, and say so.
  if (rot != 0 && !s_turn_buf) {
    canary::log_line("LVGL", "No rotation buffer — staying landscape.");
    rot = 0;
  }
#endif
  s_rot = rot;
  // Recompute the logical canvas the UI lays out in. Native is always the
  // panel's landscape; portrait swaps the axes.
  int lw = SCR_W, lh = SCR_H;
  canary::glass::rotation_logical_dims(rot, SCR_W, SCR_H, &lw, &lh);
  s_logical_w = (int16_t)lw;
  s_logical_h = (int16_t)lh;

#if defined(CD_FLAVOR_DASH) && LVGL_VERSION_MAJOR < 9
  // LVGL 8 (no dash env ships it; the browser emulator compiles this glass
  // against 8.4, emulator/build.sh): the same rotation through v8's API.
  // sw_rotate has LVGL turn each rendered area into the panel's native
  // landscape before flush_cb, and the rotation swaps the logical canvas
  // (lv_disp_get_hor_res). Same quarter turns as the v9 table below. Only a
  // change touches the driver, so a landscape boot leaves it as registered.
  {
    lv_disp_t* d = lv_disp_get_default();
    lv_disp_rot_t r = LV_DISP_ROT_NONE;
    switch (rot) {
      case canary::glass::ROT_PORTRAIT:      r = LV_DISP_ROT_90;   break;
      case canary::glass::ROT_LANDSCAPE_INV: r = LV_DISP_ROT_180;  break;
      case canary::glass::ROT_PORTRAIT_INV:  r = LV_DISP_ROT_270;  break;
      default:                               r = LV_DISP_ROT_NONE; break;
    }
    if (d && lv_disp_get_rotation(d) != r) {
      s_disp_drv.sw_rotate = 1;
      lv_disp_set_rotation(d, r);
      // The stale-frame hazard the v9 branch below names, the same way.
      lv_obj_t* scr = lv_scr_act();
      if (scr) lv_obj_invalidate(scr);
    }
  }
  canary::hal::touch_set_rotation(rot, SCR_W, SCR_H);
#endif
#if defined(CD_FLAVOR_DASH) && LVGL_VERSION_MAJOR >= 9
  // Only the RGB dash glass rotates; the round watch and the fixed-portrait
  // SPI nightstands ignore it. LVGL 9 only swaps the logical canvas here;
  // flush_cb turns each rendered area into the native framebuffer — the
  // panel keeps scanning landscape.
  if (s_disp) {
    lv_display_rotation_t r = LV_DISPLAY_ROTATION_0;
    switch (rot) {
      case canary::glass::ROT_PORTRAIT:      r = LV_DISPLAY_ROTATION_90;  break;
      case canary::glass::ROT_LANDSCAPE_INV: r = LV_DISPLAY_ROTATION_180; break;
      case canary::glass::ROT_PORTRAIT_INV:  r = LV_DISPLAY_ROTATION_270; break;
      default:                               r = LV_DISPLAY_ROTATION_0;   break;
    }
    lv_display_set_rotation(s_disp, r);
    // Same stale-frame hazard as the SPI nightlight path below: the RGB
    // panel keeps scanning the framebuffer it already has, and LVGL only
    // repaints dirty regions — so the previous orientation's pixels survive
    // wherever the new layout doesn't reach. Repaint the whole canvas.
    lv_obj_t* scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);
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

// ── Pointer input ────────────────────────────────────────────────────────

void lvgl_port_touch_feed(bool down, int16_t x, int16_t y) {
  s_in_down = down;
  if (down) {
    s_in_x = x;
    s_in_y = y;
  }
  // A release keeps the last point: LVGL wants the release where the finger
  // lifted, and the controllers report (0,0) once nothing is touching.
}

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
  // Nothing LVGL still thinks is clean actually is: the HAL just cleared the
  // panel under us, and every coordinate in the pre-rotation dirty list was
  // measured on the other axis. Ask for the whole glass back rather than
  // trusting that list — without this, regions the rebuilt face doesn't cover
  // keep whatever the previous orientation left in the panel's RAM.
  lv_obj_t* scr = lv_scr_act();
  if (scr) lv_obj_invalidate(scr);
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
