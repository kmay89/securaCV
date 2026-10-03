// tests_host/fake_lvgl/lvgl.h — LVGL 8's object POSITION model, for host
// tests that compile a real display TU (src/ui/canary_mark.cpp,
// src/ui/onboard_ui.cpp with src/ui/round_frame.cpp, and src/ui/splash.cpp)
// with no LVGL on the machine.
//
// Only what those TUs call, and only the semantics a placement test reads:
// where each object's box lands, and what a label says in which font at
// what width. Nothing is drawn. Where LVGL 8.4 has a rule, this file
// follows it, cited by function:
//  * lv_obj_align(obj, align, x, y) stores the anchor and the offset as
//    styles (LV_STYLE_ALIGN, LV_STYLE_X/Y) — lv_obj_pos.c. lv_obj_set_x/y/
//    pos write the offset alone. Nothing moves until a layout pass.
//  * A new object's box starts at its parent's content origin
//    (lv_obj_constructor), so before any layout pass lv_obj_get_x/y read 0.
//    The content box is the object's box inset by its padding and its
//    border (lv_obj_get_content_coords; lv_obj_get_x/y subtract both).
//  * A layout pass (lv_obj_update_layout -> lv_obj_refr_pos) places each
//    object at its anchor in the parent's content box plus its offset (and
//    its translate). lv_obj_get_x/y read that laid-out box relative to the
//    parent's content origin; lv_obj_get_style_x/y read the offset.
//  * lv_anim_init defaults (time 500, repeat 1, early_apply 1), and
//    lv_anim_start applies the start value at once when early_apply is set
//    and replaces a running anim on the same var and exec_cb (lv_anim.c).
//  * A label is LV_SIZE_CONTENT until sized (lv_label_constructor): a
//    layout pass gives it its text's width and its font's line height; a
//    label given a width in LV_LABEL_LONG_WRAP takes as many lines as LVGL
//    8.4 wraps its text into at that width (lv_txt_get_size, through
//    ../lv_txt_wrap.h).
//  * An object whose height is LV_SIZE_CONTENT takes its children's
//    (lv_obj_pos.c calc_content_height, the top aligns): the lowest
//    visible child's bottom plus the padding and border at both edges.
//  * lv_obj_align_to(obj, base, LV_ALIGN_OUT_TOP_MID, x, y) updates the
//    layout, then places obj on base's top edge, centered, as a
//    TOP_LEFT offset from obj's parent's content origin (lv_obj_pos.c).
//    LV_PCT sizes resolve against the parent's content box when set (every
//    caller here sizes a child of an already-sized parent).
//  * Text width is lv_txt_get_width at letter_space 0: each glyph's
//    lv_font_get_glyph_width given the letter after it. A test supplies the
//    font's glyph widths (lv_font_t::glyph_w) from LVGL's own font data.
// Timers are recorded, never fired (the blink/flourish cadence places
// nothing). lv_timer_handler() is a refresh: it calls the test's
// fake_lvgl::refresh_hook(), where a test reads the glass, and fires no
// timer either. Anims advance on fake_lvgl::run(ms) — linear, with playback and
// repeats — so a test can watch the breath and the hop move the bird.
//
// The numbers a test pins were measured with the real LVGL 8.4.0 and the
// display's lv_conf in a native harness (see test_canary_mark_seat.cpp), so
// this model is held to LVGL, not to itself.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../lv_txt_wrap.h"

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
  // lv_font_get_glyph_width(font, letter, next): the test's font data.
  int (*glyph_w)(const lv_font_t*, uint32_t, uint32_t);
  const void* dsc;  // the test's own handle for the face
};
inline lv_coord_t lv_font_get_line_height(const lv_font_t* f) {
  return (lv_coord_t)f->line_height;
}
inline uint16_t lv_font_get_glyph_width(const lv_font_t* f, uint32_t a,
                                        uint32_t b) {
  return f->glyph_w ? (uint16_t)f->glyph_w(f, a, b) : 0;
}

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
  LV_ALIGN_OUT_TOP_LEFT,
  LV_ALIGN_OUT_TOP_MID,
};
typedef uint8_t lv_align_t;

#define LV_PART_MAIN 0x000000u
#define LV_PART_INDICATOR 0x020000u
#define LV_PART_KNOB 0x030000u
#define LV_OPA_TRANSP 0
#define LV_OPA_20 51
#define LV_OPA_30 76
#define LV_OPA_40 102
#define LV_OPA_50 127
#define LV_OPA_70 178
#define LV_OPA_COVER 255
// lv_area.h: a coordinate with LV_COORD_TYPE_SPEC set is a special value.
#define LV_COORD_TYPE_SPEC (1 << 13)
#define LV_PCT(x) ((lv_coord_t)(LV_COORD_TYPE_SPEC | (x)))
#define LV_SIZE_CONTENT ((lv_coord_t)(LV_COORD_TYPE_SPEC | 2001))
typedef uint8_t lv_res_t;
#define LV_RES_INV 0
#define LV_RES_OK 1
enum { LV_TEXT_ALIGN_AUTO = 0, LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER,
       LV_TEXT_ALIGN_RIGHT };
enum { LV_LABEL_LONG_WRAP = 0, LV_LABEL_LONG_DOT, LV_LABEL_LONG_SCROLL,
       LV_LABEL_LONG_SCROLL_CIRCULAR, LV_LABEL_LONG_CLIP };
enum { LV_SCR_LOAD_ANIM_NONE = 0, LV_SCR_LOAD_ANIM_FADE_ON = 9 };
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

namespace fake_lvgl {
enum Kind { kObj = 0, kLabel, kArc, kQr };
}  // namespace fake_lvgl

struct _lv_obj_t {
  lv_obj_t* parent;
  std::vector<lv_obj_t*> children;
  lv_coord_t sx, sy;  // LV_STYLE_X / LV_STYLE_Y: the offset from the anchor
  lv_coord_t tx, ty;  // LV_STYLE_TRANSLATE_X / _Y
  lv_align_t align;   // LV_STYLE_ALIGN
  lv_coord_t w, h;    // LV_STYLE_WIDTH / HEIGHT (px only)
  bool content_w, content_h;  // LV_SIZE_CONTENT: sized on layout
  lv_coord_t pad;     // pad_all (the content box's inset, with border)
  lv_coord_t border;  // LV_STYLE_BORDER_WIDTH
  lv_coord_t x1, y1;  // the laid-out box (absolute); moves only on layout
  uint32_t flags;
  std::vector<lv_event_cb_t> on_delete;
  // What the onboarding and splash tests read (nothing else styles
  // anything here).
  int kind;                   // fake_lvgl::Kind
  std::string text;           // a label's text; a QR code's payload
  // lv_label_set_text(_fmt) calls that left the text as it was (or passed
  // NULL, LVGL's "refresh the text"): each still invalidates the label in
  // LVGL 8.4 (lv_label_refr_text), so each is a redraw of an unchanged line.
  int same_text_sets;
  const lv_font_t* font;      // LV_STYLE_TEXT_FONT (inherited when null)
  int long_mode;              // a label's lv_label_set_long_mode
  int text_align;             // LV_STYLE_TEXT_ALIGN
  lv_coord_t radius;          // LV_STYLE_RADIUS
  uint32_t bg_color;          // LV_STYLE_BG_COLOR
  uint32_t text_color;        // LV_STYLE_TEXT_COLOR as set on the object
  bool text_color_set;        // (not inherited: the test reads what it set)
  lv_coord_t arc_w_main, arc_w_ind;  // LV_STYLE_ARC_WIDTH per part
};

namespace fake_lvgl {
inline int& disp_w() { static int w = 240; return w; }
inline int& disp_h() { static int h = 240; return h; }
inline uint32_t& tick() { static uint32_t t = 0; return t; }
inline lv_obj_t*& active_screen() { static lv_obj_t* s = nullptr; return s; }
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
  o->border = 0;
  o->content_w = o->content_h = false;
  o->kind = fake_lvgl::kObj;
  o->font = nullptr;
  o->long_mode = LV_LABEL_LONG_WRAP;
  o->text_align = LV_TEXT_ALIGN_AUTO;
  o->radius = 0;
  o->bg_color = 0;
  o->text_color = 0;
  o->same_text_sets = 0;
  o->text_color_set = false;
  o->arc_w_main = o->arc_w_ind = 0;
  if (parent == nullptr) {
    // A screen: the display's size, at the origin. The first one is active
    // (lv_obj_constructor: the display's act_scr when it has none).
    o->w = (lv_coord_t)fake_lvgl::disp_w();
    o->h = (lv_coord_t)fake_lvgl::disp_h();
    o->x1 = o->y1 = 0;
    if (fake_lvgl::active_screen() == nullptr) fake_lvgl::active_screen() = o;
  } else {
    // lv_obj_constructor: the box starts at the parent's content origin.
    o->w = o->h = 100;  // LV_OBJ default size; every caller here sets one
    o->x1 = (lv_coord_t)(parent->x1 + parent->pad + parent->border);
    o->y1 = (lv_coord_t)(parent->y1 + parent->pad + parent->border);
    parent->children.push_back(o);
  }
  return o;
}
// lv_anim_del, defined with the anims below (same signature).
inline bool lv_anim_del(void* var, void (*cb)(void*, int32_t));
// obj_del_core: the children first, LV_EVENT_DELETE, then every anim on the
// object (lv_anim_del(obj, NULL)).
inline void lv_obj_del(lv_obj_t* o) {
  lv_anim_del(o, nullptr);
  if (fake_lvgl::active_screen() == o) fake_lvgl::active_screen() = nullptr;
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

namespace fake_lvgl {
// The content box's inset: the padding and the border.
inline int inset(const lv_obj_t* o) { return o->pad + o->border; }

// A width or height as lv_obj_set_size stores it: px, LV_PCT of the
// parent's content box, or LV_SIZE_CONTENT (resolved on layout).
inline lv_coord_t dim(const lv_obj_t* o, lv_coord_t v, bool horiz,
                      bool* content) {
  *content = false;
  if (v == LV_SIZE_CONTENT) {
    *content = true;
    return 0;
  }
  if ((v & LV_COORD_TYPE_SPEC) && o->parent != nullptr) {
    const int pct = v & ~LV_COORD_TYPE_SPEC;
    const int base = (horiz ? o->parent->w : o->parent->h) - 2 * inset(o->parent);
    return (lv_coord_t)(base * pct / 100);
  }
  return v;
}
}  // namespace fake_lvgl

inline void lv_obj_set_size(lv_obj_t* o, lv_coord_t w, lv_coord_t h) {
  o->w = fake_lvgl::dim(o, w, true, &o->content_w);
  o->h = fake_lvgl::dim(o, h, false, &o->content_h);
}
inline void lv_obj_set_width(lv_obj_t* o, lv_coord_t w) {
  o->w = fake_lvgl::dim(o, w, true, &o->content_w);
}
inline void lv_obj_set_height(lv_obj_t* o, lv_coord_t h) {
  o->h = fake_lvgl::dim(o, h, false, &o->content_h);
}
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
inline void lv_obj_center(lv_obj_t* o) { lv_obj_align(o, LV_ALIGN_CENTER, 0, 0); }
inline void lv_obj_set_style_translate_x(lv_obj_t* o, lv_coord_t v, lv_style_selector_t) {
  o->tx = v;
}
inline void lv_obj_set_style_translate_y(lv_obj_t* o, lv_coord_t v, lv_style_selector_t) {
  o->ty = v;
}
inline lv_coord_t lv_obj_get_style_x(const lv_obj_t* o, uint32_t) { return o->sx; }
inline lv_coord_t lv_obj_get_style_y(const lv_obj_t* o, uint32_t) { return o->sy; }
// lv_obj_pos.c: the laid-out box relative to the parent's content origin.
inline lv_coord_t lv_obj_get_x(const lv_obj_t* o) {
  return o->parent ? (lv_coord_t)(o->x1 - o->parent->x1 - fake_lvgl::inset(o->parent))
                   : o->x1;
}
inline lv_coord_t lv_obj_get_y(const lv_obj_t* o) {
  return o->parent ? (lv_coord_t)(o->y1 - o->parent->y1 - fake_lvgl::inset(o->parent))
                   : o->y1;
}
inline lv_coord_t lv_obj_get_width(const lv_obj_t* o) { return o->w; }
inline lv_coord_t lv_obj_get_height(const lv_obj_t* o) { return o->h; }

inline void lv_obj_add_flag(lv_obj_t* o, lv_obj_flag_t f) { o->flags |= f; }
inline void lv_obj_clear_flag(lv_obj_t* o, lv_obj_flag_t f) { o->flags &= ~f; }
inline bool lv_obj_has_flag(const lv_obj_t* o, lv_obj_flag_t f) {
  return (o->flags & f) == f;
}

inline void lv_obj_set_style_radius(lv_obj_t* o, lv_coord_t r, lv_style_selector_t) {
  o->radius = r;
}
inline void lv_obj_set_style_bg_color(lv_obj_t* o, lv_color_t c, lv_style_selector_t) {
  o->bg_color = c.full;
}
inline void lv_obj_set_style_bg_opa(lv_obj_t*, lv_opa_t, lv_style_selector_t) {}
inline void lv_obj_set_style_border_width(lv_obj_t* o, lv_coord_t w, lv_style_selector_t) {
  o->border = w;
}
inline void lv_obj_set_style_border_color(lv_obj_t*, lv_color_t, lv_style_selector_t) {}
inline void lv_obj_set_style_transform_angle(lv_obj_t*, lv_coord_t, lv_style_selector_t) {}
inline void lv_obj_set_style_pad_all(lv_obj_t* o, lv_coord_t p, lv_style_selector_t) {
  o->pad = p;
}

// ── labels, arcs, QR codes, screens (onboard_ui.cpp's surface) ──────────
inline lv_obj_t* lv_label_create(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  o->kind = fake_lvgl::kLabel;
  o->flags &= ~(uint32_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  o->text = "Text";  // lv_label_constructor's default
  o->content_w = o->content_h = true;
  return o;
}
inline void lv_label_set_text(lv_obj_t* o, const char* t) {
  if (t == nullptr || o->text == t) {
    o->same_text_sets++;
    return;
  }
  o->text = t;
}
inline void lv_label_set_text_fmt(lv_obj_t* o, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (o->text == buf) o->same_text_sets++;
  o->text = buf;
}
inline void lv_label_set_long_mode(lv_obj_t* o, int m) { o->long_mode = m; }
inline void lv_obj_set_style_text_font(lv_obj_t* o, const lv_font_t* f,
                                       lv_style_selector_t) {
  o->font = f;
}
// lv_obj_get_style_text_font: the text font is an inherited style.
inline const lv_font_t* lv_obj_get_style_text_font(const lv_obj_t* o,
                                                   uint32_t) {
  for (; o != nullptr; o = o->parent)
    if (o->font != nullptr) return o->font;
  return nullptr;
}
inline void lv_obj_set_style_text_align(lv_obj_t* o, int a, lv_style_selector_t) {
  o->text_align = a;
}
inline void lv_obj_set_style_text_color(lv_obj_t* o, lv_color_t c,
                                        lv_style_selector_t) {
  o->text_color = c.full;
  o->text_color_set = true;
}
inline void lv_obj_set_style_text_opa(lv_obj_t*, lv_opa_t, lv_style_selector_t) {}
// Not modeled: text widths are measured at letter_space 0 (no caller here
// reads a spaced label's box).
inline void lv_obj_set_style_text_letter_space(lv_obj_t*, lv_coord_t, lv_style_selector_t) {}

inline lv_obj_t* lv_arc_create(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  o->kind = fake_lvgl::kArc;
  return o;
}
inline void lv_arc_set_rotation(lv_obj_t*, uint16_t) {}
inline void lv_arc_set_bg_angles(lv_obj_t*, uint16_t, uint16_t) {}
inline void lv_arc_set_angles(lv_obj_t*, uint16_t, uint16_t) {}
inline void lv_obj_set_style_arc_width(lv_obj_t* o, lv_coord_t w,
                                       lv_style_selector_t sel) {
  if ((sel & 0xFF0000u) == LV_PART_INDICATOR) o->arc_w_ind = w;
  else if ((sel & 0xFF0000u) == LV_PART_MAIN) o->arc_w_main = w;
}
inline void lv_obj_set_style_arc_opa(lv_obj_t*, lv_opa_t, lv_style_selector_t) {}
inline void lv_obj_set_style_arc_color(lv_obj_t*, lv_color_t, lv_style_selector_t) {}
inline void lv_obj_set_style_arc_rounded(lv_obj_t*, bool, lv_style_selector_t) {}

// lv_qrcode_update: the payload is kept, nothing is encoded.
inline lv_res_t lv_qrcode_update(lv_obj_t* o, const void* data, uint32_t n) {
  o->text.assign((const char*)data, n);
  return LV_RES_OK;
}

inline void lv_obj_center(lv_obj_t* o);  // below, after lv_obj_align

namespace fake_lvgl {
// lv_txt_get_width at letter_space 0, one line (UTF-8 as lv_txt_utf8_next).
inline int text_width(const lv_font_t* f, const std::string& t) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < t.size();) {
    const unsigned char c = (unsigned char)t[i];
    int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    uint32_t cp = more == 0 ? c : more == 1 ? (c & 0x1F) : more == 2 ? (c & 0x0F) : (c & 0x07);
    ++i;
    for (; more > 0 && i < t.size(); --more, ++i) cp = (cp << 6) | ((unsigned char)t[i] & 0x3F);
    cps.push_back(cp);
  }
  int w = 0;
  for (size_t k = 0; k < cps.size(); ++k)
    w += lv_font_get_glyph_width(f, cps[k], k + 1 < cps.size() ? cps[k + 1] : 0);
  return w;
}
}  // namespace fake_lvgl
inline lv_obj_t* lv_scr_act() { return fake_lvgl::active_screen(); }
inline void lv_scr_load(lv_obj_t* scr) { fake_lvgl::active_screen() = scr; }
// lv_scr_load_anim with auto_del: the old screen is deleted once the new one
// is in (at once here: nothing animates a screen in this model).
inline void lv_scr_load_anim(lv_obj_t* scr, int, uint32_t, uint32_t,
                             bool auto_del) {
  lv_obj_t* old = fake_lvgl::active_screen();
  fake_lvgl::active_screen() = scr;
  if (auto_del && old != nullptr && old != scr) lv_obj_del(old);
}

namespace fake_lvgl {
// lv_font_get_glyph_width as lv_txt_wrap.h's letter measure.
struct FontGlyphW {
  const lv_font_t* f;
  int operator()(uint32_t a, uint32_t b) const {
    return lv_font_get_glyph_width(f, a, b);
  }
};

// lv_obj_refr_size for what is LV_SIZE_CONTENT, children first (LVGL's
// layout_update_core sizes the children before their parent): a label
// takes its text's extent, an object its children's.
inline void refr_size(lv_obj_t* c) {
  if (c->kind == kLabel) {
    if (!c->content_w && !c->content_h) return;
    const lv_font_t* f = lv_obj_get_style_text_font(c, LV_PART_MAIN);
    if (c->content_w) c->w = (lv_coord_t)(f ? text_width(f, c->text) : 0);
    if (c->content_h) {
      int lines = 1;
      if (f != nullptr && !c->content_w && c->long_mode == LV_LABEL_LONG_WRAP) {
        FontGlyphW g = {f};
        lines = lvwrap::line_count(c->text.c_str(), g, c->w - 2 * inset(c));
      }
      c->h = (lv_coord_t)(f ? lines * f->line_height : 0);
    }
    return;
  }
  for (size_t i = 0; i < c->children.size(); ++i) refr_size(c->children[i]);
  if (!c->content_h) return;
  // calc_content_height: the lowest visible child (top aligns: its offset,
  // translate and height below the content origin), then both insets.
  int bottom = 0;
  for (size_t i = 0; i < c->children.size(); ++i) {
    const lv_obj_t* k = c->children[i];
    if (k->flags & LV_OBJ_FLAG_HIDDEN) continue;
    if (k->align != LV_ALIGN_DEFAULT && k->align != LV_ALIGN_TOP_LEFT &&
        k->align != LV_ALIGN_TOP_MID && k->align != LV_ALIGN_TOP_RIGHT) {
      fprintf(stderr, "fake_lvgl: a content-sized object's child is "
                           "aligned %d (only the top aligns are modeled)\n",
                   (int)k->align);
      abort();
    }
    bottom = std::max(bottom, k->sy + k->ty + k->h);
  }
  c->h = (lv_coord_t)(bottom + 2 * inset(c));
}
}  // namespace fake_lvgl

// lv_obj_refr_pos, for the whole tree under `o` (lv_obj_update_layout).
// What is LV_SIZE_CONTENT takes its extent first (lv_obj_refr_size).
inline void lv_obj_update_layout(lv_obj_t* o) {
  for (size_t i = 0; i < o->children.size(); ++i) {
    lv_obj_t* c = o->children[i];
    fake_lvgl::refr_size(c);
    const int in = fake_lvgl::inset(o);
    const int pw = o->w - 2 * in, ph = o->h - 2 * in;
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
    c->x1 = (lv_coord_t)(o->x1 + in + x);
    c->y1 = (lv_coord_t)(o->y1 + in + y);
    lv_obj_update_layout(c);
  }
}

// lv_obj_align_to (lv_obj_pos.c): the screen's layout first, then obj on
// base's outside edge as a TOP_LEFT offset from obj's parent's content
// origin. Only LV_ALIGN_OUT_TOP_MID is modeled (the splash's tail).
inline void lv_obj_align_to(lv_obj_t* o, const lv_obj_t* base, lv_align_t a,
                            lv_coord_t x_ofs, lv_coord_t y_ofs) {
  lv_obj_t* root = o;
  while (root->parent != nullptr) root = root->parent;
  lv_obj_update_layout(root);
  if (a != LV_ALIGN_OUT_TOP_MID || o->parent == nullptr) {
    fprintf(stderr, "fake_lvgl: lv_obj_align_to(%d) is not modeled\n",
                 (int)a);
    abort();
  }
  const int x = base->w / 2 - o->w / 2 + x_ofs + base->x1 - o->parent->x1 -
                fake_lvgl::inset(o->parent);
  const int y = -o->h + y_ofs + base->y1 - o->parent->y1 -
                fake_lvgl::inset(o->parent);
  lv_obj_set_style_align(o, LV_ALIGN_TOP_LEFT, 0);
  lv_obj_set_pos(o, (lv_coord_t)x, (lv_coord_t)y);
}

// The display's top layer: a screen-sized object that is never the active
// screen (lv_disp_get_layer_top). Sized to the display as it stands.
inline lv_obj_t* lv_layer_top() {
  static lv_obj_t* top = nullptr;
  if (top == nullptr) {
    lv_obj_t* const act = fake_lvgl::active_screen();
    top = lv_obj_create(nullptr);
    fake_lvgl::active_screen() = act;
  }
  top->w = (lv_coord_t)fake_lvgl::disp_w();
  top->h = (lv_coord_t)fake_lvgl::disp_h();
  return top;
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

namespace fake_lvgl {
// What lv_timer_handler() calls: a test's refresh, where it reads the glass.
inline void (*&refresh_hook())() {
  static void (*hook)() = nullptr;
  return hook;
}
}  // namespace fake_lvgl
// A refresh, not a timer pass: no timer fires (see the top of this file).
inline uint32_t lv_timer_handler() {
  if (fake_lvgl::refresh_hook() != nullptr) fake_lvgl::refresh_hook()();
  return 5;
}

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
inline int32_t lv_anim_path_ease_out(const lv_anim_t*) { return 0; }
inline int32_t lv_anim_path_ease_in(const lv_anim_t*) { return 0; }
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
