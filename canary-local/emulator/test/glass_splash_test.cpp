// canary-local/emulator/test/glass_splash_test.cpp — the boot splash's
// speech-bubble tail, and the bird's seat over it, as the LVGL 8.4 glass
// draws them (sweep F185, F159). Built and run by glass_turn.sh against the
// LVGL 8.4 checkout build.sh pins and the display's own lv_conf.h, in the
// config of every display env that builds LVGL 8.4: the round watch, the
// nightstand on the S3 stick, the touch169, the AMOLED and the nightlight (on
// its 180x320 panel, and turned to 320x180 the way main.cpp wears a saved
// landscape before the splash). g++ only, no emsdk, no ESP32. Its sibling for
// the LVGL 9.5 builds is glass_splash_lvgl9_test.cpp.
//
// Before F185 the 8.4 glass drew no tail: splash.cpp turned a rounded
// square, LVGL 8.4 renders a turned object through a layer, a rounded one
// needs that layer's alpha, and lv_draw_sw_layer_create refuses an alpha
// layer while LV_COLOR_SCREEN_TRANSP is 0. splash.cpp now turns a square
// with no radius (it covers its layer, so the layer needs no alpha) about
// its center. This test links the REAL ui/splash.cpp, canary_mark.cpp,
// theme.cpp, character.cpp and lvgl_port.cpp (LVGL 8 branch) over a
// recording Arduino_GFX of the panel's size (test/native9's stand-ins, as
// the 9.5 test uses them), with an Arduino clock the splash's own pump
// drives, a Preferences stand-in whose "met" flag it clears before each
// splash, and a pseudonym of one repeated letter. It plays the first
// meeting, the whole story::kHello script, under the default Character and
// under Heirloom, with the pseudonym "2222..." and the wide-lettered
// "WWWW..." (the one that wraps the bubble tallest), on each canvas, and at
// every tick where the bubble's text starts over, or the bubble or the tail
// moved, it reads the tail OFF THE FRAME: the canvas rendered whole with the
// tail and without it, the pixels that differ being the tail's. It holds:
//
//  1. at every tick, the tail is up exactly when the bubble is;
//  2. at every read, the tail's box (lv_obj_get_coords) is on the bubble's
//     top edge, centered, kTailInset px into it;
//  3. at every read, the tail is drawn on the glass: at least kMinInk
//     pixels of ink where the bubble stands under it;
//  4. at every read, the tail's own ink (read with the bubble hidden as
//     well, so the half over the bubble, in the bubble's color, is read
//     too) is the whole turned square: its bounding box is the box's square
//     turned by its transform_angle (450) about its transform_pivot, within
//     2 px a side, and it has at least kWholeInk pixels (a turned 12 px
//     square covers 144; a layer clipped to the box would ink less);
//  5. at every read, that ink stands on the bubble's center line (its
//     center within 1 px of it) and reaches no higher than kTailRise above
//     the bubble's top edge, the reach splash_layout.h seats the bird from;
//  6. over the meeting, the bird breathes down to the seat seat() names for
//     the tallest bubble the meeting drew (splash.cpp measures it itself,
//     through LVGL, before it seats the bird), and hops no higher than
//     kHopReach above it, on the glass;
//  7. wherever seat() says the bird is clear of the tail (F159), the tail's
//     ink starts at least kGap below the bird's box at every read;
//  8. the bubble typed every line story::kHello has, whole and in order,
//     and the meeting was remembered.
//
// Printed, not held: the tail's ink, its offset from the bubble's center
// line and its reach, and, where seat() leaves the bird within the tail's
// reach (the round watch, the landscape nightlight, pinned), how far into
// the bird's box it reaches and how many of the bird's drawn pixels it hides
// at the reads that see it there (the bird drawn with the bubble and the
// tail hidden, against the same frame with the tail shown).
//
// Prints "ALL LVGL 8 SPLASH TAIL TESTS PASSED" on success.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <lvgl.h>
#include <config.h>
#include "pins.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "identity/device_pseudonym.h"
#include "story/story_scripts.h"

#include "canary/hal/display.h"
#include "canary/log.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/character.h"
#include "canary/ui/lvgl_port.h"
#include "canary/ui/splash.h"
#include "canary/ui/splash_layout.h"

#if LVGL_VERSION_MAJOR != 8
#error "glass_splash_test.cpp builds against LVGL 8.4 (glass_turn.sh); its 9.x sibling is glass_splash_lvgl9_test.cpp"
#endif
#if !defined(CD_FLAVOR_WATCH) && !defined(CD_FLAVOR_NIGHTSTAND)
#error "glass_splash_test.cpp builds a small-glass config (glass_turn.sh): the dash line is LVGL 9"
#endif

namespace sl = canary::ui::splashlayout;

// ── The bench this test stands in for ───────────────────────────────────
namespace {
uint32_t g_now = 0;
int g_fail = 0;
int g_checks = 0;
constexpr int PW = TFT_WIDTH, PH = TFT_HEIGHT;  // the panel's own scan
Arduino_GFX g_panel(PW, PH);
#ifdef CD_NIGHTLIGHT
// The panel once the HAL's display_set_rotation() has turned its scan
// (MADCTL): it takes the turned canvas's coordinates as they are.
Arduino_GFX g_turned(PH, PW);
#endif
Arduino_GFX* g_gfx = &g_panel;
void observe();
}  // namespace

extern "C" uint32_t millis(void) { return g_now; }
// splash.cpp's pump waits here between LVGL passes: the clock moves, and
// the test looks at the glass.
extern "C" void delay(uint32_t ms) {
  g_now += ms;
  observe();
}
Native9Serial Serial;
namespace canary {
LogSink g_log_sink = nullptr;
}
namespace canary::hal {
Arduino_GFX* gfx() { return g_gfx; }
void touch_set_rotation(uint8_t, int16_t, int16_t) {}
TouchSample touch_read() { return TouchSample(); }  // nobody taps
void backlight_set(uint8_t) {}
}  // namespace canary::hal

#define CHECK(cond, ...)                                    \
  do {                                                      \
    g_checks++;                                             \
    if (!(cond)) {                                          \
      g_fail++;                                             \
      printf("  FAIL (%s:%d): ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
    }                                                       \
  } while (0)

// A check made at every tick or read: each evaluation counts, and only the
// first failure of its kind (`fails`) is printed.
#define HOLD(cond, fails, ...)                                \
  do {                                                        \
    g_checks++;                                               \
    if (!(cond)) {                                            \
      g_fail++;                                               \
      if ((fails)++ == 0) {                                   \
        printf("  FAIL (%s:%d): ", __FILE__, __LINE__);       \
        printf(__VA_ARGS__);                                  \
        printf("\n");                                         \
      }                                                       \
    }                                                         \
  } while (0)

namespace {

constexpr long kMinInk = 30;     // a drawn tail; 0 where 8.4 drew none (F185)
constexpr long kWholeInk = 130;  // the whole turned square (144 px of area)
constexpr int kWin = 40;         // the window read around the tail's box

// ── Reading the glass ────────────────────────────────────────────────────
int g_lw = PW, g_lh = PH;  // the logical canvas (the panel's, turned or not)

uint16_t logical_px(int x, int y) { return g_gfx->fb[(size_t)y * g_gfx->W + x]; }

void render_whole() {
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);
}

struct Box {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // inclusive
};
struct Ink {
  long n = 0;
  Box b{9999, 9999, -9999, -9999};
};

std::vector<uint16_t> read_window(const lv_area_t& c, Box* w) {
  w->x0 = c.x1 - kWin;
  w->y0 = c.y1 - kWin;
  w->x1 = c.x2 + kWin;
  w->y1 = c.y2 + kWin;
  std::vector<uint16_t> out;
  for (int y = w->y0; y <= w->y1; y++)
    for (int x = w->x0; x <= w->x1; x++)
      out.push_back(x >= 0 && y >= 0 && x < g_lw && y < g_lh ? logical_px(x, y) : 0);
  return out;
}

// The tail's ink: the canvas rendered whole with the tail and without it.
// With the bubble as `under`, the bubble is hidden for both renders, so a
// part of the tail in the bubble's own color counts too. The glass is left
// as the splash drew it.
Ink tail_ink(lv_obj_t* tail, lv_obj_t* under = nullptr) {
  lv_area_t c;
  lv_obj_get_coords(tail, &c);
  Box w;
  if (under != nullptr) lv_obj_add_flag(under, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  const std::vector<uint16_t> with = read_window(c, &w);
  lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  const std::vector<uint16_t> without = read_window(c, &w);
  lv_obj_clear_flag(tail, LV_OBJ_FLAG_HIDDEN);
  if (under != nullptr) lv_obj_clear_flag(under, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  Ink ink;
  size_t i = 0;
  for (int y = w.y0; y <= w.y1; y++) {
    for (int x = w.x0; x <= w.x1; x++, i++) {
      if (with[i] == without[i]) continue;
      ink.n++;
      if (x < ink.b.x0) ink.b.x0 = x;
      if (x > ink.b.x1) ink.b.x1 = x;
      if (y < ink.b.y0) ink.b.y0 = y;
      if (y > ink.b.y1) ink.b.y1 = y;
    }
  }
  return ink;
}

// Where the tail's styles put its ink: the box's square turned by its
// transform_angle about its transform_pivot (lv_refr.c's refr_obj resolves
// the pivot from the box's top-left corner, a percentage of its side).
Box styled_extent(const lv_obj_t* tail) {
  lv_area_t c;
  lv_obj_get_coords(tail, &c);
  const int w = lv_area_get_width(&c), h = lv_area_get_height(&c);
  int px = lv_obj_get_style_transform_pivot_x(tail, LV_PART_MAIN);
  int py = lv_obj_get_style_transform_pivot_y(tail, LV_PART_MAIN);
  if (LV_COORD_IS_PCT(px)) px = LV_COORD_GET_PCT(px) * w / 100;
  if (LV_COORD_IS_PCT(py)) py = LV_COORD_GET_PCT(py) * h / 100;
  const double a = lv_obj_get_style_transform_angle(tail, LV_PART_MAIN) / 10.0 * M_PI / 180.0;
  const double ox = c.x1 + px, oy = c.y1 + py;
  const double cx[4] = {(double)c.x1, (double)c.x2 + 1, (double)c.x2 + 1, (double)c.x1};
  const double cy[4] = {(double)c.y1, (double)c.y1, (double)c.y2 + 1, (double)c.y2 + 1};
  double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  for (int k = 0; k < 4; k++) {
    const double dx = cx[k] - ox, dy = cy[k] - oy;
    const double rx = ox + dx * cos(a) - dy * sin(a), ry = oy + dx * sin(a) + dy * cos(a);
    x0 = fmin(x0, rx);
    x1 = fmax(x1, rx);
    y0 = fmin(y0, ry);
    y1 = fmax(y1, ry);
  }
  Box b;
  b.x0 = (int)floor(x0);
  b.y0 = (int)floor(y0);
  b.x1 = (int)ceil(x1) - 1;
  b.y1 = (int)ceil(y1) - 1;
  return b;
}

// The bird's drawn pixels the tail hides: those the bird inks (the frame
// with the bird against the frame without it, the tail and the bubble
// hidden) that the tail's own ink then covers (the tail shown over the
// bird, the bubble still hidden). The glass is left as the splash drew it.
long bird_hidden(lv_obj_t* tail, lv_obj_t* bub, lv_obj_t* bird) {
  lv_area_t c;
  lv_obj_get_coords(bird, &c);
  Box w;
  lv_obj_add_flag(bub, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(tail, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  const std::vector<uint16_t> bird_only = read_window(c, &w);
  lv_obj_add_flag(bird, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  const std::vector<uint16_t> bare = read_window(c, &w);
  lv_obj_clear_flag(bird, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(tail, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  const std::vector<uint16_t> with_tail = read_window(c, &w);
  lv_obj_clear_flag(bub, LV_OBJ_FLAG_HIDDEN);
  render_whole();
  long n = 0;
  for (size_t i = 0; i < bare.size(); i++)
    if (bird_only[i] != bare[i] && with_tail[i] != bird_only[i]) n++;
  return n;
}

// ── Watching the splash ─────────────────────────────────────────────────
struct Watch {
  std::string tag;
  lv_obj_t* home = nullptr;
  bool capturing = false;
  int ticks = 0;          // observed ticks with the splash up
  int bubble_ticks = 0;   // ...with the bubble up
  int mismatched = 0;     // ...where the tail and the bubble disagreed
  int captures = 0;
  int inked = 0;          // ...of them with tail ink to read
  int seat_fails = 0, ink_fails = 0, land_fails = 0, center_fails = 0, rise_fails = 0;
  long ink_lo = 1 << 30, ink_hi = 0, own_lo = 1 << 30;
  double center_worst = 0;  // the own ink's center less the bubble's, worst
  int rise_hi = -9999;      // the highest the ink stood over the bubble's top
  int tallest = 0;          // the tallest bubble drawn
  int bird_lo = 1 << 30, bird_hi = -(1 << 30);  // the bird's box top, drawn
  int tail_gap = 1 << 30;   // the tail's ink top less the bird's box bottom
  long hidden = 0;          // the most of the bird's drawn pixels the tail hid
  std::string last_text;
  std::vector<std::string> typed;
  bool emptied = false;
  lv_area_t last_tail{0, 0, -1, -1}, last_bub{0, 0, -1, -1};
};
Watch g_w;

bool same_area(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

// The splash's tail (the plain kTailSide square with no children, as
// test_splash_scenes finds it) and bubble (the one child holding a single
// label). The screen is laid out first.
void find_parts(const lv_obj_t* scr, lv_obj_t** tail, lv_obj_t** bub) {
  *tail = *bub = nullptr;
  const uint32_t n = lv_obj_get_child_cnt(scr);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(scr, (int32_t)i);
    if (c == canary::ui::canary_mark_obj()) continue;
    if (lv_obj_check_type(c, &lv_obj_class) && lv_obj_get_child_cnt(c) == 0 &&
        lv_obj_get_width(c) == sl::kTailSide && lv_obj_get_height(c) == sl::kTailSide)
      *tail = c;
    if (lv_obj_get_child_cnt(c) == 1 && lv_obj_check_type(lv_obj_get_child(c, 0), &lv_label_class))
      *bub = c;
  }
}

void capture(lv_obj_t* tail, lv_obj_t* bub) {
  Watch& W = g_w;
  const char* tag = W.tag.c_str();
  W.captures++;
  lv_area_t t, b;
  lv_obj_get_coords(tail, &t);
  lv_obj_get_coords(bub, &b);
  const char* text = lv_label_get_text(lv_obj_get_child(bub, 0));
  // 2: the box on the bubble's top edge, centered, kTailInset px into it.
  const int want_x = b.x1 + (lv_area_get_width(&b) - sl::kTailSide) / 2;
  const int want_y = b.y1 - sl::kTailSide + sl::kTailInset;
  HOLD(t.x1 == want_x && t.y1 == want_y && lv_area_get_width(&t) == sl::kTailSide &&
           lv_area_get_height(&t) == sl::kTailSide,
       W.seat_fails, "%s: the tail's box is %dx%d at (%d,%d); on the bubble (x %d..%d, top %d, "
       "\"%s\") it sits at (%d,%d)", tag, (int)lv_area_get_width(&t),
       (int)lv_area_get_height(&t), (int)t.x1, (int)t.y1, (int)b.x1, (int)b.x2, (int)b.y1, text,
       want_x, want_y);
  // 3: drawn on the glass.
  const Ink glass = tail_ink(tail);
  if (glass.n < W.ink_lo) W.ink_lo = glass.n;
  if (glass.n > W.ink_hi) W.ink_hi = glass.n;
  HOLD(glass.n >= kMinInk, W.ink_fails,
       "%s: the tail draws %ld px of ink (box at %d,%d, \"%s\"); a drawn tail has at least %ld",
       tag, glass.n, (int)t.x1, (int)t.y1, text, kMinInk);
  // 4: its own ink, the whole turned square, where its styles put it.
  const Ink own = tail_ink(tail, bub);
  const Box want = styled_extent(tail);
  if (own.n < W.own_lo) W.own_lo = own.n;
  HOLD(own.n >= kWholeInk && abs(own.b.x0 - want.x0) <= 2 && abs(own.b.x1 - want.x1) <= 2 &&
           abs(own.b.y0 - want.y0) <= 2 && abs(own.b.y1 - want.y1) <= 2,
       W.land_fails, "%s: the tail's own ink is %ld px at x %d..%d, y %d..%d; its box (%d,%d) "
       "turned %d about its pivot spans x %d..%d, y %d..%d with %ld px at least (\"%s\")",
       tag, own.n, own.b.x0, own.b.x1, own.b.y0, own.b.y1, (int)t.x1, (int)t.y1,
       (int)lv_obj_get_style_transform_angle(tail, LV_PART_MAIN), want.x0, want.x1, want.y0,
       want.y1, kWholeInk, text);
  // 5: on the bubble's center line, no higher than kTailRise over its top.
  if (own.n == 0) {
    HOLD(false, W.center_fails, "%s: no tail ink to stand on the bubble's center line (\"%s\")",
         tag, text);
    return;
  }
  W.inked++;
  const double off = ((own.b.x0 + own.b.x1 + 1) - (b.x1 + b.x2 + 1)) / 2.0;
  if (fabs(off) > fabs(W.center_worst)) W.center_worst = off;
  HOLD(fabs(off) <= 1.0, W.center_fails,
       "%s: the tail's ink (x %d..%d) stands %+.1f px off the bubble's center line (x %d..%d, "
       "\"%s\")", tag, own.b.x0, own.b.x1, off, (int)b.x1, (int)b.x2, text);
  const int rise = (int)b.y1 - own.b.y0;
  if (rise > W.rise_hi) W.rise_hi = rise;
  HOLD(rise <= sl::kTailRise, W.rise_fails,
       "%s: the tail's ink starts %d rows above the bubble's top edge (y %d); kTailRise is %d "
       "(\"%s\")", tag, rise, (int)b.y1, sl::kTailRise, text);
  // 7's read: how far the tail's ink starts below the bird's box.
  lv_obj_t* bird = canary::ui::canary_mark_obj();
  if (bird != nullptr && !lv_obj_has_flag(bird, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t m;
    lv_obj_get_coords(bird, &m);
    const int gap = own.b.y0 - ((int)m.y2 + 1);
    if (gap < W.tail_gap) W.tail_gap = gap;
    if (gap < 0) {
      const long h = bird_hidden(tail, bub, bird);
      if (h > W.hidden) W.hidden = h;
    }
  }
}

void observe() {
  Watch& W = g_w;
  if (W.capturing) return;
  lv_obj_t* scr = lv_scr_act();
  if (scr == nullptr || scr == W.home) return;
  W.ticks++;
  lv_obj_update_layout(scr);
  lv_obj_t* bird = canary::ui::canary_mark_obj();
  if (bird != nullptr && lv_obj_get_parent(bird) == scr && !lv_obj_has_flag(bird, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t m;
    lv_obj_get_coords(bird, &m);
    if (m.y1 < W.bird_lo) W.bird_lo = m.y1;
    if (m.y1 > W.bird_hi) W.bird_hi = m.y1;
  }
  lv_obj_t* tail = nullptr;
  lv_obj_t* bub = nullptr;
  find_parts(scr, &tail, &bub);
  HOLD(tail != nullptr, W.mismatched, "%s: no %d px tail square on the splash", W.tag.c_str(),
       sl::kTailSide);
  const bool tail_up = tail != nullptr && !lv_obj_has_flag(tail, LV_OBJ_FLAG_HIDDEN);
  const bool bub_up = bub != nullptr && !lv_obj_has_flag(bub, LV_OBJ_FLAG_HIDDEN);
  // 1: up together.
  HOLD(tail_up == bub_up, W.mismatched, "%s: the tail is %s while the bubble is %s",
       W.tag.c_str(), tail_up ? "up" : "hidden", bub_up ? "up" : "hidden");
  if (!tail_up || !bub_up) return;
  W.bubble_ticks++;
  lv_area_t t, b;
  lv_obj_get_coords(tail, &t);
  lv_obj_get_coords(bub, &b);
  if (lv_area_get_height(&b) > W.tallest) W.tallest = lv_area_get_height(&b);
  const std::string text = lv_label_get_text(lv_obj_get_child(bub, 0));
  // 8: the lines typed, each counted at its first letter.
  if (text.empty()) {
    W.emptied = true;
  } else {
    if (W.typed.empty() || W.emptied || text.compare(0, W.typed.back().size(), W.typed.back()) != 0)
      W.typed.push_back(text);
    else
      W.typed.back() = text;
    W.emptied = false;
  }
  const bool restart = text.compare(0, W.last_text.size(), W.last_text) != 0 ||
                       (W.last_text.empty() && !text.empty());
  if (restart || !same_area(t, W.last_tail) || !same_area(b, W.last_bub)) {
    W.capturing = true;
    capture(tail, bub);
    W.capturing = false;
  }
  W.last_text = text;
  W.last_tail = t;
  W.last_bub = b;
}

// ── One first meeting on one canvas ─────────────────────────────────────
std::vector<std::string> hello_lines() {
  char pseudo[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(pseudo, sizeof(pseudo));
  std::vector<std::string> out;
  const canary::story::Scene& sc = canary::story::kHello;
  for (uint8_t i = 0; i < sc.n; i++) {
    if (sc.beats[i].line == nullptr) continue;
    std::string l = sc.beats[i].line;
    for (size_t at = l.find("%s"); at != std::string::npos; at = l.find("%s", at + strlen(pseudo)))
      l.replace(at, 2, pseudo);
    out.push_back(l);
  }
  return out;
}

std::vector<std::string> g_unclear;  // the plays seat() leaves within the tail's reach

void play(const char* canvas, canary::ui::Character ch, char letter) {
  Watch& W = g_w;
  W = Watch();
  canary::ui::character_apply(ch);
  device_pseudonym::fill() = letter;
  char tag[160];
  snprintf(tag, sizeof(tag), "%s/%s, pseudonym %c...", canvas,
           ch == canary::ui::Character::Heirloom ? "heirloom" : "default", letter);
  W.tag = tag;
  native9::prefs().clear();  // never met: the first meeting
  g_lw = canary::ui::lvgl_port_width();
  g_lh = canary::ui::lvgl_port_height();
  CHECK(lv_disp_get_hor_res(nullptr) == g_lw && lv_disp_get_ver_res(nullptr) == g_lh &&
            g_gfx->W == g_lw && g_gfx->H == g_lh,
        "%s: LVGL's canvas is %dx%d, the port's %dx%d, the panel's %dx%d", tag,
        (int)lv_disp_get_hor_res(nullptr), (int)lv_disp_get_ver_res(nullptr), g_lw, g_lh,
        g_gfx->W, g_gfx->H);
  W.home = lv_scr_act();
  printf("  %s, canvas %dx%d:\n", tag, g_lw, g_lh);
  const uint32_t t0 = g_now;
  canary::ui::splash_play(1700);
  canary::ui::splash_reveal();
  CHECK(native9::prefs()["scv-hello/met"] == 1, "%s: the meeting was not remembered", tag);
  CHECK(W.captures > 0, "%s: the tail was never read over %d ticks", tag, W.ticks);
  // 8: every line of the script, whole, in its order.
  const std::vector<std::string> want = hello_lines();
  size_t same = 0;
  while (same < want.size() && same < W.typed.size() && W.typed[same] == want[same]) same++;
  CHECK(same == want.size() && W.typed.size() == want.size(),
        "%s: the bubble typed %zu lines where story::kHello has %zu; line %zu differs (typed "
        "\"%s\", the script's \"%s\")", tag, W.typed.size(), want.size(), same + 1,
        same < W.typed.size() ? W.typed[same].c_str() : "(nothing)",
        same < want.size() ? want[same].c_str() : "(nothing)");
  // 6: the bird on seat()'s seat for the tallest bubble drawn.
  const sl::Seat s = sl::seat(g_lh, sl::kSmallGlass, true, W.tallest);
  const int seat_top = sl::bird_top(g_lh, sl::kSmallGlass.bird, s.bird_off);
  CHECK(W.bird_hi >= seat_top + sl::kBreath - 1 && W.bird_hi <= seat_top + sl::kBreath &&
            W.bird_lo >= seat_top - sl::kHopReach && W.bird_lo >= 0 &&
            W.bird_lo < seat_top - sl::kBreath,
        "%s: the bird's top is drawn at y %d..%d; seat() seats it at y %d for the %d px tallest "
        "bubble (breathing %d px, hopping %d at most, a hop seen)", tag, W.bird_lo, W.bird_hi,
        seat_top, W.tallest, sl::kBreath, sl::kHopReach);
  // 7: clear of the tail wherever seat() says so (a tail never inked is
  // check 3's to fail).
  if (s.clear) {
    if (W.inked > 0)
      CHECK(W.tail_gap >= sl::kGap, "%s: the tail's ink starts %d px from the bird's box "
            "(below it; negative, into it); seat() keeps it kGap (%d) clear", tag, W.tail_gap,
            sl::kGap);
  } else {
    g_unclear.push_back(canvas);
  }
  char where[96];
  if (W.inked == 0)
    snprintf(where, sizeof(where), "never inked");
  else if (W.tail_gap >= 0)
    snprintf(where, sizeof(where), "%d px below the bird's box", W.tail_gap);
  else
    snprintf(where, sizeof(where), "%d px into the bird's box, hiding %ld of its drawn pixels",
             -W.tail_gap, W.hidden);
  printf("    bird seated at y %d%s, tallest bubble %d px; the tail read %d times: %ld..%ld px "
         "on the glass, %ld+ of its own, at worst %+.1f px off the bubble's center line, %d rows "
         "above its top edge at most, %s; %d checks failed\n",
         seat_top, s.raised ? " (raised)" : s.hang ? " (hung bubble)" : "", W.tallest, W.captures,
         W.ink_lo, W.ink_hi, W.own_lo, W.center_worst, W.inked ? W.rise_hi : 0, where,
         W.seat_fails + W.ink_fails + W.land_fails + W.center_fails + W.rise_fails + W.mismatched);
  (void)t0;
}

void play_all(const char* canvas) {
  for (canary::ui::Character ch : {canary::ui::Character::QuietGlass, canary::ui::Character::Heirloom})
    for (char letter : {'2', 'W'}) play(canvas, ch, letter);
}

}  // namespace

int main() {
  printf("the splash's tail (LVGL %d.%d.%d, the real renderer, a %dx%d panel):\n",
         LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, PW, PH);
  CHECK(canary::ui::lvgl_port_init(), "lvgl_port_init failed");
  if (lv_disp_get_default() == nullptr) return 1;
  char canvas[32];
  snprintf(canvas, sizeof(canvas), "%dx%d", PW, PH);
  play_all(canvas);
#ifdef CD_NIGHTLIGHT
  // main.cpp: a saved landscape is worn before the splash, the panel turned
  // in hardware (the HAL's MADCTL write, here the turned panel) and LVGL
  // told its new shape.
  g_gfx = &g_turned;
  canary::ui::lvgl_port_set_panel_rotation(1);
  snprintf(canvas, sizeof(canvas), "%dx%d (turned)", PH, PW);
  play_all(canvas);
#endif
  // Pinned (F159): seat() leaves the bird within the tail's reach only on
  // the round watch (its clear seat would lift its hop off the disc) and on
  // the landscape nightlight (its bubble hangs kGap under the bird).
  std::string unclear;
  for (const std::string& u : g_unclear)
    if (unclear.find(u) == std::string::npos) unclear += (unclear.empty() ? "" : ", ") + u;
#if defined(CD_FLAVOR_WATCH) && !defined(CD_FLAVOR_NIGHTSTAND)
  const char* want = "240x240";
#elif defined(CD_NIGHTLIGHT)
  const char* want = "320x180 (turned)";
#else
  const char* want = "";
#endif
  printf("  not clear of the tail: %s\n", unclear.empty() ? "none" : unclear.c_str());
  CHECK(unclear == want, "the canvases seat() leaves within the tail's reach are \"%s\"; only "
        "\"%s\" should be", unclear.c_str(), want);
  if (g_fail) {
    printf("%d of %d LVGL 8 splash tail checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("ALL LVGL 8 SPLASH TAIL TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
