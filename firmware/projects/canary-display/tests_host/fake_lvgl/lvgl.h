// tests_host/fake_lvgl/lvgl.h — LVGL 8's object POSITION model, for host
// tests that compile a real display TU (src/ui/canary_mark.cpp) with no
// LVGL on the machine.
//
// Only what canary_mark.cpp calls, and only the semantics a placement test
// reads. Where LVGL 8.4 has a rule, this file follows it, cited by function:
//  * lv_obj_align(obj, align, x, y) stores the anchor and the offset as
//    styles (LV_STYLE_ALIGN, LV_STYLE_X/Y) — lv_obj_pos.c. lv_obj_set_x/y/
//    pos write the offset alone. Nothing moves until a layout pass.
//  * A new object's box starts at its parent's content origin
//    (lv_obj_constructor), so before any layout pass lv_obj_get_x/y read 0.
//  * A layout pass (lv_obj_update_layout -> lv_obj_refr_pos) places each
//    object at its anchor in the parent's content box plus its offset (and
//    its translate). lv_obj_get_x/y read that laid-out box relative to the
//    parent's content origin; lv_obj_get_style_x/y read the offset.
//  * lv_anim_init defaults (time 500, repeat 1, early_apply 1), and
//    lv_anim_start applies the start value at once when early_apply is set
//    and replaces a running anim on the same var and exec_cb (lv_anim.c).
// Timers are recorded, never fired (the blink/flourish cadence places
// nothing). Anims advance on fake_lvgl::run(ms) — linear, with playback and
// repeats — so a test can watch the breath and the hop move the bird.
//
// The numbers a test pins were measured with the real LVGL 8.4.0 and the
// display's lv_conf in a native harness (see test_canary_mark_seat.cpp), so
// this model is held to LVGL, not to itself.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#define LVGL_VERSION_MAJOR 8

typedef int16_t lv_coord_t;
typedef uint8_t lv_opa_t;
typedef uint32_t lv_part_t;
typedef uint32_t lv_style_selector_t;
struct lv_color_t {
  uint32_t full;
};
inline lv_color_t lv_color_hex(uint32_t c) { return lv_color_t{c}; }
inline lv_color_t lv_color_white() { return lv_color_hex(0xFFFFFF); }
inline lv_color_t lv_color_black() { return lv_color_hex(0x000000); }
struct lv_font_t {
  int line_height;
};

enum {
  LV_ALIGN_DEFAULT = 0,
  LV_ALIGN_TOP_LEFT,
  LV_ALIGN_TOP_MID,
  LV_ALIGN_TOP_RIGHT,
  LV_ALIGN_BOTTOM_LEFT,
  LV_ALIGN_BOTTOM_MID,
  LV_ALIGN_BOTTOM_RIGHT,
  LV_ALIGN_LEFT_MID,
  LV_ALIGN_RIGHT_MID,
  LV_ALIGN_CENTER,
};
typedef uint8_t lv_align_t;

#define LV_PART_MAIN 0x000000u
#define LV_OPA_TRANSP 0
#define LV_OPA_COVER 255
#define LV_RADIUS_CIRCLE 0x7FFF
#define LV_ANIM_REPEAT_INFINITE 0xFFFF

enum : uint32_t {
  LV_OBJ_FLAG_HIDDEN = 1u << 0,
  LV_OBJ_FLAG_CLICKABLE = 1u << 1,
  LV_OBJ_FLAG_SCROLLABLE = 1u << 4,
};
typedef uint32_t lv_obj_flag_t;

enum lv_event_code_t { LV_EVENT_DELETE = 33 };

struct _lv_obj_t;
typedef _lv_obj_t lv_obj_t;
struct lv_event_t {
  lv_obj_t* target;
  lv_event_code_t code;
};
typedef void (*lv_event_cb_t)(lv_event_t*);

struct _lv_obj_t {
  lv_obj_t* parent;
  std::vector<lv_obj_t*> children;
  lv_coord_t sx, sy;  // LV_STYLE_X / LV_STYLE_Y: the offset from the anchor
  lv_coord_t tx, ty;  // LV_STYLE_TRANSLATE_X / _Y
  lv_align_t align;   // LV_STYLE_ALIGN
  lv_coord_t w, h;    // LV_STYLE_WIDTH / HEIGHT (px only)
  lv_coord_t pad;     // pad_all (the content box's inset)
  lv_coord_t x1, y1;  // the laid-out box (absolute); moves only on layout
  uint32_t flags;
  std::vector<lv_event_cb_t> on_delete;
};

namespace fake_lvgl {
inline int& disp_w() { static int w = 240; return w; }
inline int& disp_h() { static int h = 240; return h; }
inline uint32_t& tick() { static uint32_t t = 0; return t; }
}  // namespace fake_lvgl

inline uint32_t lv_tick_get() { return fake_lvgl::tick(); }
inline lv_coord_t lv_disp_get_hor_res(void*) { return (lv_coord_t)fake_lvgl::disp_w(); }
inline lv_coord_t lv_disp_get_ver_res(void*) { return (lv_coord_t)fake_lvgl::disp_h(); }

inline lv_obj_t* lv_obj_create(lv_obj_t* parent) {
  lv_obj_t* o = new lv_obj_t();
  o->parent = parent;
  o->flags = LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE;
  o->align = LV_ALIGN_DEFAULT;
  o->sx = o->sy = o->tx = o->ty = 0;
  o->pad = 0;
  if (parent == nullptr) {
    // A screen: the display's size, at the origin.
    o->w = (lv_coord_t)fake_lvgl::disp_w();
    o->h = (lv_coord_t)fake_lvgl::disp_h();
    o->x1 = o->y1 = 0;
  } else {
    // lv_obj_constructor: the box starts at the parent's content origin.
    o->w = o->h = 100;  // LV_OBJ default size; every caller here sets one
    o->x1 = (lv_coord_t)(parent->x1 + parent->pad);
    o->y1 = (lv_coord_t)(parent->y1 + parent->pad);
    parent->children.push_back(o);
  }
  return o;
}
inline void lv_obj_del(lv_obj_t* o) {
  for (size_t i = 0; i < o->children.size(); ++i) {
    o->children[i]->parent = nullptr;
    lv_obj_del(o->children[i]);
  }
  lv_event_t e = {o, LV_EVENT_DELETE};
  for (size_t i = 0; i < o->on_delete.size(); ++i) o->on_delete[i](&e);
  if (o->parent != nullptr) {
    std::vector<lv_obj_t*>& c = o->parent->children;
    c.erase(std::remove(c.begin(), c.end(), o), c.end());
  }
  delete o;
}
inline void lv_obj_add_event_cb(lv_obj_t* o, lv_event_cb_t cb,
                                lv_event_code_t code, void*) {
  if (code == LV_EVENT_DELETE) o->on_delete.push_back(cb);
}

inline void lv_obj_set_size(lv_obj_t* o, lv_coord_t w, lv_coord_t h) {
  o->w = w;
  o->h = h;
}
inline void lv_obj_set_height(lv_obj_t* o, lv_coord_t h) { o->h = h; }
inline void lv_obj_set_x(lv_obj_t* o, lv_coord_t x) { o->sx = x; }
inline void lv_obj_set_y(lv_obj_t* o, lv_coord_t y) { o->sy = y; }
inline void lv_obj_set_pos(lv_obj_t* o, lv_coord_t x, lv_coord_t y) {
  o->sx = x;
  o->sy = y;
}
inline void lv_obj_set_style_align(lv_obj_t* o, lv_align_t a, lv_style_selector_t) {
  o->align = a;
}
// lv_obj_pos.c: lv_obj_set_style_align, then lv_obj_set_pos.
inline void lv_obj_align(lv_obj_t* o, lv_align_t a, lv_coord_t x, lv_coord_t y) {
  lv_obj_set_style_align(o, a, 0);
  lv_obj_set_pos(o, x, y);
}
inline void lv_obj_set_style_translate_y(lv_obj_t* o, lv_coord_t v, lv_style_selector_t) {
  o->ty = v;
}
inline lv_coord_t lv_obj_get_style_x(const lv_obj_t* o, uint32_t) { return o->sx; }
inline lv_coord_t lv_obj_get_style_y(const lv_obj_t* o, uint32_t) { return o->sy; }
// lv_obj_pos.c: the laid-out box relative to the parent's content origin.
inline lv_coord_t lv_obj_get_x(const lv_obj_t* o) {
  return o->parent ? (lv_coord_t)(o->x1 - o->parent->x1 - o->parent->pad) : o->x1;
}
inline lv_coord_t lv_obj_get_y(const lv_obj_t* o) {
  return o->parent ? (lv_coord_t)(o->y1 - o->parent->y1 - o->parent->pad) : o->y1;
}
inline lv_coord_t lv_obj_get_width(const lv_obj_t* o) { return o->w; }
inline lv_coord_t lv_obj_get_height(const lv_obj_t* o) { return o->h; }

inline void lv_obj_add_flag(lv_obj_t* o, lv_obj_flag_t f) { o->flags |= f; }
inline void lv_obj_clear_flag(lv_obj_t* o, lv_obj_flag_t f) { o->flags &= ~f; }
inline bool lv_obj_has_flag(const lv_obj_t* o, lv_obj_flag_t f) {
  return (o->flags & f) == f;
}

inline void lv_obj_set_style_radius(lv_obj_t*, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_bg_color(lv_obj_t*, lv_color_t, lv_style_selector_t) {}
inline void lv_obj_set_style_bg_opa(lv_obj_t*, lv_opa_t, lv_style_selector_t) {}
inline void lv_obj_set_style_border_width(lv_obj_t*, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_pad_all(lv_obj_t* o, lv_coord_t p, lv_style_selector_t) {
  o->pad = p;
}

// lv_obj_refr_pos, for the whole tree under `o` (lv_obj_update_layout).
inline void lv_obj_update_layout(lv_obj_t* o) {
  for (size_t i = 0; i < o->children.size(); ++i) {
    lv_obj_t* c = o->children[i];
    const int pw = o->w - 2 * o->pad, ph = o->h - 2 * o->pad;
    int x = c->sx + c->tx, y = c->sy + c->ty;
    switch (c->align) {
      case LV_ALIGN_TOP_MID: x += pw / 2 - c->w / 2; break;
      case LV_ALIGN_TOP_RIGHT: x += pw - c->w; break;
      case LV_ALIGN_LEFT_MID: y += ph / 2 - c->h / 2; break;
      case LV_ALIGN_BOTTOM_LEFT: y += ph - c->h; break;
      case LV_ALIGN_BOTTOM_MID: x += pw / 2 - c->w / 2; y += ph - c->h; break;
      case LV_ALIGN_BOTTOM_RIGHT: x += pw - c->w; y += ph - c->h; break;
      case LV_ALIGN_RIGHT_MID: x += pw - c->w; y += ph / 2 - c->h / 2; break;
      case LV_ALIGN_CENTER: x += pw / 2 - c->w / 2; y += ph / 2 - c->h / 2; break;
      default: break;  // DEFAULT / TOP_LEFT: the offset alone
    }
    c->x1 = (lv_coord_t)(o->x1 + o->pad + x);
    c->y1 = (lv_coord_t)(o->y1 + o->pad + y);
    lv_obj_update_layout(c);
  }
}

// ── timers: recorded, never fired ─────────────────────────────────────────
struct _lv_timer_t;
typedef _lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t*);
struct _lv_timer_t {
  lv_timer_cb_t cb;
  uint32_t period;
  bool paused;
};
inline lv_timer_t* lv_timer_create(lv_timer_cb_t cb, uint32_t period, void*) {
  lv_timer_t* t = new lv_timer_t();
  t->cb = cb;
  t->period = period;
  t->paused = false;
  return t;
}
inline void lv_timer_del(lv_timer_t* t) { delete t; }
inline void lv_timer_set_period(lv_timer_t* t, uint32_t p) { t->period = p; }
inline void lv_timer_pause(lv_timer_t* t) { t->paused = true; }
inline void lv_timer_resume(lv_timer_t* t) { t->paused = false; }

// ── anims (lv_anim.c's start/delete rules; linear, playback, repeats) ─────
struct _lv_anim_t;
typedef _lv_anim_t lv_anim_t;
typedef void (*lv_anim_exec_xcb_t)(void*, int32_t);
typedef int32_t (*lv_anim_path_cb_t)(const lv_anim_t*);
typedef void (*lv_anim_ready_cb_t)(lv_anim_t*);
struct _lv_anim_t {
  void* var;
  lv_anim_exec_xcb_t exec_cb;
  lv_anim_path_cb_t path_cb;
  lv_anim_ready_cb_t ready_cb;
  int32_t start_value, end_value;
  uint32_t time, playback_time;
  uint16_t repeat_cnt;
  uint8_t early_apply;
  // run state
  uint32_t act_time;
  bool playback_now;
};
inline int32_t lv_anim_path_linear(const lv_anim_t*) { return 0; }
inline int32_t lv_anim_path_ease_in_out(const lv_anim_t*) { return 0; }
inline int32_t lv_anim_path_overshoot(const lv_anim_t*) { return 0; }
inline void lv_anim_init(lv_anim_t* a) {
  memset(a, 0, sizeof(*a));
  a->time = 500;
  a->end_value = 100;
  a->repeat_cnt = 1;
  a->path_cb = lv_anim_path_linear;
  a->early_apply = 1;
}
inline void lv_anim_set_var(lv_anim_t* a, void* v) { a->var = v; }
inline void lv_anim_set_exec_cb(lv_anim_t* a, lv_anim_exec_xcb_t cb) { a->exec_cb = cb; }
inline void lv_anim_set_values(lv_anim_t* a, int32_t s, int32_t e) {
  a->start_value = s;
  a->end_value = e;
}
inline void lv_anim_set_time(lv_anim_t* a, uint32_t t) { a->time = t; }
inline void lv_anim_set_playback_time(lv_anim_t* a, uint32_t t) { a->playback_time = t; }
inline void lv_anim_set_repeat_count(lv_anim_t* a, uint16_t n) { a->repeat_cnt = n; }
inline void lv_anim_set_path_cb(lv_anim_t* a, lv_anim_path_cb_t cb) { a->path_cb = cb; }
inline void lv_anim_set_ready_cb(lv_anim_t* a, lv_anim_ready_cb_t cb) { a->ready_cb = cb; }

namespace fake_lvgl {
inline std::vector<lv_anim_t*>& anims() {
  static std::vector<lv_anim_t*> a;
  return a;
}
}  // namespace fake_lvgl

inline bool lv_anim_del(void* var, lv_anim_exec_xcb_t cb) {
  std::vector<lv_anim_t*>& l = fake_lvgl::anims();
  bool del = false;
  for (size_t i = 0; i < l.size();) {
    if ((var == nullptr || l[i]->var == var) && (cb == nullptr || l[i]->exec_cb == cb)) {
      delete l[i];
      l.erase(l.begin() + (long)i);
      del = true;
    } else {
      ++i;
    }
  }
  return del;
}
inline lv_anim_t* lv_anim_start(const lv_anim_t* a) {
  if (a->exec_cb != nullptr) lv_anim_del(a->var, a->exec_cb);
  lv_anim_t* n = new lv_anim_t(*a);
  n->act_time = 0;
  n->playback_now = false;
  fake_lvgl::anims().push_back(n);
  if (n->early_apply && n->exec_cb && n->var) n->exec_cb(n->var, n->start_value);
  return n;
}

namespace fake_lvgl {
// Advance every anim by `ms` in 5 ms steps (a 200 Hz handler).
inline void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) {
    tick() += 5;
    std::vector<lv_anim_t*> live = anims();
    for (size_t i = 0; i < live.size(); ++i) {
      lv_anim_t* a = live[i];
      if (std::find(anims().begin(), anims().end(), a) == anims().end()) continue;
      a->act_time += 5;
      const uint32_t span = a->playback_now ? a->playback_time : a->time;
      bool done = false;
      if (a->act_time >= span) {
        a->act_time = span;
        if (!a->playback_now && a->playback_time > 0) {
          a->playback_now = true;
          a->act_time = 0;
          if (a->exec_cb) a->exec_cb(a->var, a->end_value);
          continue;
        }
        done = true;
      }
      const int32_t from = a->playback_now ? a->end_value : a->start_value;
      const int32_t to = a->playback_now ? a->start_value : a->end_value;
      const int32_t v =
          span == 0 ? to : from + (int32_t)((int64_t)(to - from) * a->act_time / span);
      if (a->exec_cb) a->exec_cb(a->var, v);
      if (!done) continue;
      if (a->repeat_cnt == LV_ANIM_REPEAT_INFINITE || a->repeat_cnt > 1) {
        if (a->repeat_cnt != LV_ANIM_REPEAT_INFINITE) a->repeat_cnt--;
        a->act_time = 0;
        a->playback_now = false;
        continue;
      }
      lv_anim_ready_cb_t ready = a->ready_cb;
      lv_anim_t copy = *a;
      anims().erase(std::find(anims().begin(), anims().end(), a));
      delete a;
      if (ready) ready(&copy);
    }
  }
}
}  // namespace fake_lvgl
