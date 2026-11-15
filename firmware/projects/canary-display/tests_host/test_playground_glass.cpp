// Host test for the Dev Playground glass's RS485 words (F317): the REAL
// src/playground/playground_ui.cpp, compiled against fake_lvgl/ (LVGL 8's
// object, position and label-size model) with LVGL's own Montserrat metrics
// (montserrat_metrics.h), the 4.3B board pins and the dash config, as the
// playground env compiles it. The dash builds the glass on LVGL 9.5, whose
// Montserrat 12/14/36 advances and kerning are the same as 8.4's (compared
// value for value, 2026-11) and whose child placement (the parent's padding
// and border) and child clip (the parent's box) are the same rules, so the
// widths measured here are the dash's.
//
// For every RS485 outcome the station can leave, it holds:
//   * the pin tracker's RS485 row and the station's tile draw exactly
//     playground.h's rs485_row_words() / rs485_tile_words(), words and look
//     (the color each look is drawn in), so a word written into
//     playground_ui.cpp instead of the helper fails here; the words
//     themselves are held to each probe outcome by test_fieldbus_glue.cpp;
//   * those words fit: the row's value ends inside the tracker panel's border
//     (LVGL clips a child to its parent's box) for every exception code
//     0..255, and inside the panel's content box, where every other row's
//     value sits, for the codes Modbus defines (1..11); the tile's sub line
//     and the detail panel's value fit theirs.
//
// Prints "ALL PLAYGROUND GLASS TESTS PASSED" on success.

#include <Arduino.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "canary/playground/playground.h"
#include "canary/playground/playground_ui.h"
#include "montserrat_metrics.h"

namespace pg = canary::playground;

static int g_fail = 0;

#define CHECK(cond, ...)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      std::printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                            \
      std::printf("\n");                                   \
      g_fail++;                                            \
    }                                                      \
  } while (0)

// ── LVGL's measure, from LVGL's font data ───────────────────────────────────
// The reading onboard_test_env.h's glyph_px() makes (lv_font_get_glyph_width:
// the advance, class kerning against the next letter, its rounding). A code
// point the metrics do not carry (the LV_SYMBOL_* icons) measures 0 and is
// counted, so a measured RS485 label can be shown to carry none.

namespace {

typedef montserrat_metrics::Face Face;

const Face* face_of(int size) {
  for (int i = 0; i < montserrat_metrics::kFaceCount; i++) {
    if (montserrat_metrics::kFaces[i].size == size) return &montserrat_metrics::kFaces[i];
  }
  return nullptr;
}

int glyph_index(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return (int)cp - 0x20;
  if (cp == 0xB0) return 95;
  if (cp == 0x2022) return 96;
  return -1;
}

int g_unmeasured = 0;

int glyph_px(const lv_font_t* font, uint32_t a, uint32_t b) {
  const int ia = glyph_index(a);
  if (font->dsc == nullptr || ia < 0) {
    g_unmeasured++;
    return 0;
  }
  const Face& f = *(const Face*)font->dsc;
  int k = 0;
  const int ib = glyph_index(b);
  if (ib >= 0) {
    const int l = f.kern_left[ia];
    const int r = f.kern_right[ib];
    if (l > 0 && r > 0) k = f.kern_values[(l - 1) * f.right_classes + (r - 1)];
  }
  const int kv = (k * f.kern_scale) >> 4;
  return (f.adv_w[ia] + kv + 8) >> 4;
}

// montserrat_metrics.h carries the faces the Character ladders set
// (gen_montserrat_metrics.py reads them out of src/ui/character.cpp), not the
// playground's: a face it does not carry is an unmeasured one here, and the
// labels this test measures (12 px row and tile, 36 px detail) say so.
lv_font_t make_font(int size) {
  const Face* f = face_of(size);
  lv_font_t out;
  out.line_height = f != nullptr ? f->line_height : size;
  out.glyph_w = glyph_px;
  out.dsc = f;
  return out;
}

}  // namespace

const lv_font_t lv_font_montserrat_12 = make_font(12);
const lv_font_t lv_font_montserrat_14 = make_font(14);
const lv_font_t lv_font_montserrat_16 = make_font(16);
const lv_font_t lv_font_montserrat_20 = make_font(20);
const lv_font_t lv_font_montserrat_24 = make_font(24);
const lv_font_t lv_font_montserrat_28 = make_font(28);
const lv_font_t lv_font_montserrat_36 = make_font(36);

// ── What playground_ui.cpp links against from playground.cpp, stubbed ──────

namespace canary::playground {
static PgState g_state;
const PgState& state() { return g_state; }
void action_do_pulse(int) {}
void action_do_latch_toggle(int) {}
void action_pad_cycle_preset() {}
void action_tof_cycle_trip() {}
void action_rs485_probe() {}
void action_can_send() {}
const char* pad_preset_name(uint8_t) { return "contact"; }
bool i2c_addr_reserved(uint8_t) { return false; }
}  // namespace canary::playground

// ── Finding the RS485 row, tile and detail value on the glass ───────────────

static lv_obj_t* child_labeled(lv_obj_t* parent, const char* text) {
  for (lv_obj_t* c : parent->children) {
    if (c->kind == fake_lvgl::kLabel && c->text == text) return c;
  }
  return nullptr;
}

static lv_obj_t* panel_with_label(lv_obj_t* scr, const char* text) {
  for (lv_obj_t* c : scr->children) {
    if (c->kind == fake_lvgl::kObj && child_labeled(c, text) != nullptr) return c;
  }
  return nullptr;
}

static int inner_w(const lv_obj_t* o) { return o->w - 2 * fake_lvgl::inset(o); }

// The width LVGL gives `text` in a label's own font, and whether every glyph
// in it was measured.
static int measured(const lv_obj_t* label, const std::string& text, bool* whole) {
  const int before = g_unmeasured;
  const int w = fake_lvgl::text_width(lv_obj_get_style_text_font(label, LV_PART_MAIN), text);
  *whole = g_unmeasured == before;
  return w;
}

// The colors playground_ui.cpp draws each look in (its Quiet Glass tokens).
static uint32_t row_color(pg::Rs485Look l) {
  return l == pg::RS485_LOOK_OK ? 0x4CAF50 : l == pg::RS485_LOOK_WARN ? 0xFFB300 : 0x5C5C5C;
}
static uint32_t tile_color(pg::Rs485Look l) {
  return l == pg::RS485_LOOK_OK ? 0x4CAF50 : l == pg::RS485_LOOK_WARN ? 0xFFB300 : 0xEDEDED;
}

struct Glass {
  lv_obj_t* scr;
  lv_obj_t* track;
  lv_obj_t* row_val;
  lv_obj_t* card;
  lv_obj_t* card_val;
  lv_obj_t* card_sub;
};

static Glass build() {
  fake_lvgl::disp_w() = 800;
  fake_lvgl::disp_h() = 480;
  Glass g = {};
  g.scr = lv_obj_create(nullptr);
  lv_scr_load(g.scr);
  pg::playground_ui_create();
  lv_obj_update_layout(g.scr);
  g.track = panel_with_label(g.scr, "RS485 44/43");
  g.card = panel_with_label(g.scr, "RS485 • A/B");
  if (g.track == nullptr || g.card == nullptr) {
    std::printf("FAIL: the RS485 row or tile is not on the glass\n");
    std::exit(1);
  }
  // Each tracker row is three labels: its icon, its name and its value.
  lv_obj_t* name = child_labeled(g.track, "RS485 44/43");
  for (size_t i = 0; i + 1 < g.track->children.size(); i++) {
    if (g.track->children[i] == name) g.row_val = g.track->children[i + 1];
  }
  // A tile is its title, its value and its sub line.
  if (g.row_val == nullptr || g.card->children.size() != 3) {
    std::printf("FAIL: the RS485 row value or the tile's labels are not where the UI puts them\n");
    std::exit(1);
  }
  g.card_val = g.card->children[1];
  g.card_sub = g.card->children[2];
  return g;
}

static void show(const Glass& g, const pg::Rs485State& s) {
  pg::g_state.rs485 = s;
  pg::playground_ui_update(pg::g_state);
  lv_obj_update_layout(g.scr);
}

static int g_widest_row = 0;
static std::string g_widest_row_text;

static void check_case(const Glass& g, const char* what, const pg::Rs485State& s) {
  show(g, s);
  char row[48], val[32], sub[24];
  const pg::Rs485Look rl = pg::rs485_row_words(s, row, sizeof(row));
  const pg::Rs485Look tl = pg::rs485_tile_words(s, val, sizeof(val), sub, sizeof(sub));

  // The glass draws the helper's words, in the look's color.
  CHECK(g.row_val->text == row, "%s: row reads \"%s\", playground.h says \"%s\"", what,
        g.row_val->text.c_str(), row);
  CHECK(g.row_val->text_color == row_color(rl), "%s: row drawn in %06X, its look is %06X", what,
        (unsigned)g.row_val->text_color, (unsigned)row_color(rl));
  CHECK(g.card_val->text == val, "%s: tile value reads \"%s\", playground.h says \"%s\"", what,
        g.card_val->text.c_str(), val);
  CHECK(g.card_sub->text == sub, "%s: tile sub reads \"%s\", playground.h says \"%s\"", what,
        g.card_sub->text.c_str(), sub);
  CHECK(g.card_val->text_color == tile_color(tl), "%s: tile drawn in %06X, its look is %06X", what,
        (unsigned)g.card_val->text_color, (unsigned)tile_color(tl));

  // And they fit.
  bool whole = false;
  const int rw = measured(g.row_val, g.row_val->text, &whole);
  CHECK(whole, "%s: the row's text has a glyph (or a face) the metrics do not carry", what);
  CHECK(rw == g.row_val->w, "%s: the row label is %d px, its text %d", what, g.row_val->w, rw);
  const int row_end = lv_obj_get_x(g.row_val) + g.row_val->w;
  // Inside the border: LVGL clips a child to its parent's box, and the border
  // is drawn on the box's last pixel column.
  CHECK(row_end <= inner_w(g.track) + g.track->pad, "%s: the row ends at %d, the panel's border at %d",
        what, row_end, inner_w(g.track) + g.track->pad);
  const bool defined_code = s.last_exc >= 1 && s.last_exc <= 11;
  if (s.last_exc < 0 || defined_code) {
    CHECK(row_end <= inner_w(g.track), "%s: the row ends at %d, past the content box's %d", what,
          row_end, inner_w(g.track));
  }
  if (rw > g_widest_row) {
    g_widest_row = rw;
    g_widest_row_text = g.row_val->text;
  }
  const int sub_end = lv_obj_get_x(g.card_sub) + measured(g.card_sub, g.card_sub->text, &whole);
  CHECK(whole, "%s: the tile's sub line has a glyph (or a face) the metrics do not carry", what);
  CHECK(sub_end <= inner_w(g.card), "%s: the tile's sub line ends at %d, its content box at %d", what,
        sub_end, inner_w(g.card));
}

static pg::Rs485State st(uint32_t polls, uint32_t replies, uint16_t val, bool ok, int16_t exc) {
  pg::Rs485State s;
  s.ready = polls > 0;
  s.polls = polls;
  s.replies = replies;
  s.last_val = val;
  s.last_ok = ok;
  s.last_exc = exc;
  return s;
}

int main() {
  std::printf("test_playground_glass\n");
  const Glass g = build();

  check_case(g, "before any probe", st(0, 0, 0, false, -1));
  check_case(g, "a good read", st(1, 1, 4660, true, -1));
  check_case(g, "the widest good read", st(1, 1, 65535, true, -1));
  check_case(g, "no reply", st(1, 0, 0, false, -1));
  check_case(g, "no reply after a good read", st(2, 1, 7, false, -1));
  check_case(g, "an exception after a good read", st(2, 1, 7, false, 2));
  char what[48];
  for (int code = 0; code <= 255; code++) {
    std::snprintf(what, sizeof(what), "exception %d", code);
    check_case(g, what, st(1, 0, 0, false, (int16_t)code));
  }
  // Measured under the same rules, for the record: the phrase the row could
  // not hold.
  bool whole = false;
  const int long_form = measured(g.row_val, "exception 2 • check the register", &whole);
  std::printf("  row value: widest \"%s\" %d px, from x=%d; the panel's border at %d, its content "
              "box %d; \"exception 2 • check the register\" would be %d px\n",
              g_widest_row_text.c_str(), g_widest_row, (int)lv_obj_get_x(g.row_val),
              inner_w(g.track) + g.track->pad, inner_w(g.track), long_form);
  CHECK(lv_obj_get_x(g.row_val) + long_form > inner_w(g.track) + g.track->pad,
        "the long form now fits the row: the shortening is no longer needed");

  // The detail panel: open the tile, and its value line is the tile's words.
  pg::Rs485State s = st(1, 0, 0, false, 255);
  show(g, s);
  pg::playground_ui_handle_tap((int16_t)(g.card->x1 + 10), (int16_t)(g.card->y1 + 10));
  lv_obj_update_layout(g.scr);
  lv_obj_t* detail = g.scr->children.back();
  CHECK(detail != g.card && (g.card->flags & LV_OBJ_FLAG_HIDDEN), "the RS485 detail panel did not open");
  char val[32], sub[24];
  pg::rs485_tile_words(s, val, sizeof(val), sub, sizeof(sub));
  const std::string big = std::string(val) + "  " + sub;
  lv_obj_t* big_label = nullptr;
  for (lv_obj_t* c : detail->children) {
    if (c->kind == fake_lvgl::kLabel && c->text == big) big_label = c;
  }
  CHECK(big_label != nullptr, "the detail panel does not read \"%s\"", big.c_str());
  if (big_label != nullptr) {
    const int w = measured(big_label, big_label->text, &whole);
    CHECK(whole, "the detail value has a glyph (or a face) the metrics do not carry");
    CHECK(lv_obj_get_x(big_label) + w <= inner_w(detail), "the detail value ends at %d, its box at %d",
          (int)(lv_obj_get_x(big_label) + w), inner_w(detail));
    std::printf("  detail value: \"%s\" %d px of %d\n", big.c_str(), w, inner_w(detail));
  }

  if (g_fail) {
    std::printf("%d PLAYGROUND GLASS CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL PLAYGROUND GLASS TESTS PASSED\n");
  return 0;
}
