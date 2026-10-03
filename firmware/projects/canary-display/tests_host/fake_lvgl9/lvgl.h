// tests_host/fake_lvgl9/lvgl.h — LVGL 9.5's display layer, as much of it as
// src/ui/lvgl_port.cpp's LVGL 9 branch calls, so a host test can compile the
// REAL port with no LVGL on the machine and drive its flush callback the way
// LVGL's partial render mode does (test_lvgl_port_turn.cpp).
//
// Nothing renders here: the test is the renderer. It fills the draw buffer
// the port registered with a logical area's pixels and calls the flush
// callback the port registered, exactly what lv_refr.c's call_flush_cb()
// does in LV_DISPLAY_RENDER_MODE_PARTIAL (a logical area over a packed,
// logical-width buffer; LVGL 9 turns nothing it renders —
// main-modules/display/rotation.rst). What LVGL itself computes, this file
// quotes from lvgl v9.5.0 (MIT), function by function, so the test is held
// to LVGL, not to itself:
//  * lv_display_rotate_area() and lv_display_rotate_point() —
//    src/display/lv_display.c (hor_res/ver_res are the display's NATIVE
//    size; lv_display_get_horizontal_resolution swaps them at 90/270);
//  * lv_draw_sw_rotate()       — src/draw/sw/lv_draw_sw_utils.c, the
//    portable RGB565 loops (rotate90/180/270_rgb565; no ASM backend);
//  * lv_draw_buf_width_to_stride() at LV_DRAW_BUF_STRIDE_ALIGN 1, the
//    lv_conf_internal.h default the display's lv_conf leaves alone.
// A native LVGL 9.5.0 run of the same port (dash config, a recording
// Arduino_GFX; F205) drew the same glass these quotes predict.
#pragma once
#include <stdint.h>
#include <string.h>

#define LVGL_VERSION_MAJOR 9
#define LVGL_VERSION_MINOR 5
#define LVGL_VERSION_PATCH 0
#define LV_DRAW_BUF_STRIDE_ALIGN 1

typedef uint8_t lv_opa_t;
typedef uint32_t lv_style_selector_t;
struct lv_color_t { uint8_t blue, green, red; };
inline lv_color_t lv_color_black() { return lv_color_t{0, 0, 0}; }

struct lv_area_t { int32_t x1, y1, x2, y2; };
struct lv_point_t { int32_t x, y; };

typedef enum {
  LV_DISPLAY_ROTATION_0 = 0,
  LV_DISPLAY_ROTATION_90,
  LV_DISPLAY_ROTATION_180,
  LV_DISPLAY_ROTATION_270,
} lv_display_rotation_t;

typedef enum {
  LV_DISPLAY_RENDER_MODE_PARTIAL,
  LV_DISPLAY_RENDER_MODE_DIRECT,
  LV_DISPLAY_RENDER_MODE_FULL,
} lv_display_render_mode_t;

typedef enum { LV_COLOR_FORMAT_RGB565 = 0x12 } lv_color_format_t;

typedef enum { LV_INDEV_STATE_RELEASED = 0, LV_INDEV_STATE_PRESSED } lv_indev_state_t;
typedef enum { LV_INDEV_TYPE_NONE = 0, LV_INDEV_TYPE_POINTER } lv_indev_type_t;
struct lv_indev_data_t {
  lv_point_t point;
  lv_indev_state_t state;
};

typedef enum {
  LV_OBJ_FLAG_CLICKABLE = 1 << 1,
  LV_OBJ_FLAG_SCROLLABLE = 1 << 4,
} lv_obj_flag_t;

struct lv_display_t;
struct lv_indev_t;
typedef void (*lv_display_flush_cb_t)(lv_display_t* disp, const lv_area_t* area,
                                      uint8_t* px_map);
typedef void (*lv_indev_read_cb_t)(lv_indev_t* indev, lv_indev_data_t* data);
typedef uint32_t (*lv_tick_get_cb_t)(void);

struct lv_obj_t {
  int invalidations = 0;
  uint32_t flags = LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE;
  int32_t x = 0, y = 0, w = 0, h = 0;
  lv_opa_t bg_opa = 0;
};

struct lv_display_t {
  int32_t hor_res = 0, ver_res = 0;  // native, as created
  lv_display_rotation_t rotation = LV_DISPLAY_ROTATION_0;
  lv_display_flush_cb_t flush_cb = nullptr;
  uint8_t* buf1 = nullptr;
  uint32_t buf_size = 0;
  lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
  int flushing = 0;     // set by call_flush, cleared by lv_display_flush_ready
  int flush_ready = 0;  // lv_display_flush_ready() calls
  int rotation_sets = 0;
};

struct lv_indev_t {
  lv_indev_type_t type = LV_INDEV_TYPE_NONE;
  lv_indev_read_cb_t read_cb = nullptr;
  lv_display_t* disp = nullptr;
};

namespace fake_lvgl9 {
struct State {
  lv_display_t disp;
  lv_indev_t indev;
  lv_obj_t screen, top_layer, scrim;
  bool display_created = false;
  lv_tick_get_cb_t tick = nullptr;
};
inline State& st() { static State s; return s; }
inline lv_display_t* display() { return st().display_created ? &st().disp : nullptr; }
inline lv_obj_t* screen() { return &st().screen; }
}  // namespace fake_lvgl9

inline void lv_init() { fake_lvgl9::st() = fake_lvgl9::State(); }
inline void lv_tick_set_cb(lv_tick_get_cb_t cb) { fake_lvgl9::st().tick = cb; }

inline lv_display_t* lv_display_create(int32_t hor_res, int32_t ver_res) {
  fake_lvgl9::State& s = fake_lvgl9::st();
  s.disp = lv_display_t();
  s.disp.hor_res = hor_res;
  s.disp.ver_res = ver_res;
  s.display_created = true;
  return &s.disp;
}
inline void lv_display_set_flush_cb(lv_display_t* d, lv_display_flush_cb_t cb) { d->flush_cb = cb; }
inline void lv_display_set_buffers(lv_display_t* d, void* buf1, void* /*buf2*/,
                                   uint32_t buf_size, lv_display_render_mode_t mode) {
  d->buf1 = static_cast<uint8_t*>(buf1);
  d->buf_size = buf_size;
  d->render_mode = mode;
}
inline void lv_display_flush_ready(lv_display_t* d) {
  d->flushing = 0;
  d->flush_ready++;
}
inline lv_display_rotation_t lv_display_get_rotation(lv_display_t* d) { return d->rotation; }
inline void lv_display_set_rotation(lv_display_t* d, lv_display_rotation_t r) {
  // lv_display.c: store the rotation, then update_resolution() resizes the
  // screens and invalidates — it turns nothing that was drawn.
  d->rotation = r;
  d->rotation_sets++;
}
inline lv_color_format_t lv_display_get_color_format(lv_display_t*) {
  return LV_COLOR_FORMAT_RGB565;  // LV_COLOR_DEPTH 16
}
inline int32_t lv_area_get_width(const lv_area_t* a) { return a->x2 - a->x1 + 1; }
inline int32_t lv_area_get_height(const lv_area_t* a) { return a->y2 - a->y1 + 1; }

// lv_display.c (v9.5.0), as written there.
inline void lv_display_rotate_area(lv_display_t* disp, lv_area_t* area) {
  lv_display_rotation_t rotation = lv_display_get_rotation(disp);

  if (rotation == LV_DISPLAY_ROTATION_0) return;

  int32_t w = lv_area_get_width(area);
  int32_t h = lv_area_get_height(area);

  switch (rotation) {
    case LV_DISPLAY_ROTATION_90:
      area->y2 = disp->ver_res - area->x1 - 1;
      area->x1 = area->y1;
      area->x2 = area->x1 + h - 1;
      area->y1 = area->y2 - w + 1;
      break;
    case LV_DISPLAY_ROTATION_180:
      area->y2 = disp->ver_res - area->y1 - 1;
      area->y1 = area->y2 - h + 1;
      area->x2 = disp->hor_res - area->x1 - 1;
      area->x1 = area->x2 - w + 1;
      break;
    case LV_DISPLAY_ROTATION_270:
      area->x1 = disp->hor_res - area->y2 - 1;
      area->y2 = area->x2;
      area->x2 = area->x1 + h - 1;
      area->y1 = area->y2 - w + 1;
      break;
    default:
      break;
  }
}

// lv_display.c (v9.5.0), as written there: what lv_indev.c's
// indev_pointer_proc() does to every pointer sample a read_cb reports.
inline void lv_display_rotate_point(lv_display_t* disp, lv_point_t* point) {
  lv_display_rotation_t rotation = lv_display_get_rotation(disp);

  if (rotation == LV_DISPLAY_ROTATION_0) return;

  const int32_t x = point->x;
  const int32_t y = point->y;

  switch (rotation) {
    case LV_DISPLAY_ROTATION_90:
      point->x = disp->ver_res - y - 1;
      point->y = x;
      break;
    case LV_DISPLAY_ROTATION_180:
      point->x = disp->hor_res - x - 1;
      point->y = disp->ver_res - y - 1;
      break;
    case LV_DISPLAY_ROTATION_270:
      point->x = y;
      point->y = disp->hor_res - x - 1;
      break;
    default:
      break;
  }
}

// lv_draw_buf.c's default width_to_stride at 16 bpp, LV_DRAW_BUF_STRIDE_ALIGN 1.
inline uint32_t lv_draw_buf_width_to_stride(uint32_t w, lv_color_format_t) { return w * 2; }

// lv_draw_sw_utils.c (v9.5.0): the portable RGB565 rotations, as written there.
namespace fake_lvgl9 {
inline void rotate90_rgb565(const uint16_t* src, uint16_t* dst, int32_t src_width,
                            int32_t src_height, int32_t src_stride, int32_t dst_stride) {
  src_stride /= sizeof(uint16_t);
  dst_stride /= sizeof(uint16_t);
  for (int32_t x = 0; x < src_width; ++x) {
    int32_t dstIndex = (src_width - x - 1);
    int32_t srcIndex = x;
    for (int32_t y = 0; y < src_height; ++y) {
      dst[dstIndex * dst_stride + y] = src[srcIndex];
      srcIndex += src_stride;
    }
  }
}
inline void rotate180_rgb565(const uint16_t* src, uint16_t* dst, int32_t width,
                             int32_t height, int32_t src_stride, int32_t dest_stride) {
  src_stride /= sizeof(uint16_t);
  dest_stride /= sizeof(uint16_t);
  for (int32_t y = 0; y < height; ++y) {
    int32_t dstIndex = (height - y - 1) * dest_stride;
    int32_t srcIndex = y * src_stride;
    for (int32_t x = 0; x < width; ++x) {
      dst[dstIndex + width - x - 1] = src[srcIndex + x];
    }
  }
}
inline void rotate270_rgb565(const uint16_t* src, uint16_t* dst, int32_t src_width,
                             int32_t src_height, int32_t src_stride, int32_t dst_stride) {
  src_stride /= sizeof(uint16_t);
  dst_stride /= sizeof(uint16_t);
  for (int32_t x = 0; x < src_width; ++x) {
    int32_t dstIndex = x * dst_stride;
    int32_t srcIndex = x;
    for (int32_t y = 0; y < src_height; ++y) {
      dst[dstIndex + (src_height - y - 1)] = src[srcIndex];
      srcIndex += src_stride;
    }
  }
}
}  // namespace fake_lvgl9

inline void lv_draw_sw_rotate(const void* src, void* dest, int32_t src_width,
                              int32_t src_height, int32_t src_stride, int32_t dest_stride,
                              lv_display_rotation_t rotation, lv_color_format_t color_format) {
  if (color_format != LV_COLOR_FORMAT_RGB565) return;
  const uint16_t* s = static_cast<const uint16_t*>(src);
  uint16_t* d = static_cast<uint16_t*>(dest);
  if (rotation == LV_DISPLAY_ROTATION_90)
    fake_lvgl9::rotate90_rgb565(s, d, src_width, src_height, src_stride, dest_stride);
  else if (rotation == LV_DISPLAY_ROTATION_180)
    fake_lvgl9::rotate180_rgb565(s, d, src_width, src_height, src_stride, dest_stride);
  else if (rotation == LV_DISPLAY_ROTATION_270)
    fake_lvgl9::rotate270_rgb565(s, d, src_width, src_height, src_stride, dest_stride);
}

// ── Pointer device ───────────────────────────────────────────────────────
inline lv_indev_t* lv_indev_create() { return &fake_lvgl9::st().indev; }
inline void lv_indev_set_type(lv_indev_t* i, lv_indev_type_t t) { i->type = t; }
inline void lv_indev_set_read_cb(lv_indev_t* i, lv_indev_read_cb_t cb) { i->read_cb = cb; }
inline void lv_indev_set_display(lv_indev_t* i, lv_display_t* d) { i->disp = d; }

// ── Objects: the active screen (invalidated on a turn) and the scrim ─────
inline lv_obj_t* lv_scr_act() { return &fake_lvgl9::st().screen; }
inline void lv_obj_invalidate(lv_obj_t* obj) { obj->invalidations++; }
inline lv_obj_t* lv_layer_top() { return &fake_lvgl9::st().top_layer; }
inline lv_obj_t* lv_obj_create(lv_obj_t*) { return &fake_lvgl9::st().scrim; }
inline void lv_obj_remove_style_all(lv_obj_t*) {}
inline void lv_obj_set_style_bg_color(lv_obj_t*, lv_color_t, lv_style_selector_t) {}
inline void lv_obj_set_style_border_width(lv_obj_t*, int32_t, lv_style_selector_t) {}
inline void lv_obj_set_style_radius(lv_obj_t*, int32_t, lv_style_selector_t) {}
inline void lv_obj_set_style_bg_opa(lv_obj_t* o, lv_opa_t opa, lv_style_selector_t) { o->bg_opa = opa; }
inline void lv_obj_clear_flag(lv_obj_t* o, lv_obj_flag_t f) { o->flags &= ~(uint32_t)f; }
inline void lv_obj_set_pos(lv_obj_t* o, int32_t x, int32_t y) { o->x = x; o->y = y; }
inline void lv_obj_set_size(lv_obj_t* o, int32_t w, int32_t h) { o->w = w; o->h = h; }
