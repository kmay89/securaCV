// canary-local/emulator/test/glass_splash_lvgl9_test.cpp — the boot splash's
// speech-bubble tail as the LVGL 9.5 builds draw it (sweep F246). Built and
// run by glass_turn_lvgl9.sh beside glass_turn_lvgl9_test.cpp, against the
// same real LVGL release and lv_conf.h, in the two configs that compile
// LVGL 9: the dash (on its 800x480 panel, and turned to 480x800 the way
// main.cpp wears a saved portrait before the splash) and nightstand-c6 (its
// lean config on the 172x320 panel). g++ only, no emsdk, no ESP32.
//
// test_splash_scenes (tests_host, fake LVGL) holds where splash.cpp puts the
// tail OBJECT: a 12 px square aligned on the bubble's top edge, kTailInset
// px into it, re-aligned whenever the typed line changes (F158). Nothing
// there is drawn. LVGL 8.4 draws no tail on this lv_conf (F185); LVGL 9.5
// turns the square through a transform layer, and the library's half of
// that was rendered by glass_turn_lvgl9_test (a 145 px diamond hung from
// the box's top-left corner, F225). This test renders splash.cpp's own
// tail: it links the REAL ui/splash.cpp, canary_mark.cpp, theme.cpp,
// character.cpp and lvgl_port.cpp (and the story engine's headers) over a
// recording Arduino_GFX of the panel's native size, with an Arduino clock
// the splash's own pump drives (delay() advances millis()), a Preferences
// stand-in whose "met" flag the test clears before each splash, and a fixed
// pseudonym. It plays the first meeting, the whole story::kHello script, on
// each canvas, and at every tick where the bubble's text starts over, or the
// bubble or the tail moved, it reads the tail OFF THE FRAME: the canvas
// rendered whole with the tail and without it, read back through the port's
// turn from the native panel, the pixels that differ being the tail's. It
// holds, on every canvas:
//
//  1. at every tick, the tail is up exactly when the bubble is (a wordless
//     beat hides both);
//  2. at every read, the tail's box (the one lv_obj_get_coords reports, the
//     one test_splash_scenes holds against fake LVGL) is on the bubble's top
//     edge, centered, kTailInset px into it, through LVGL 9.5's own layout;
//  3. at every read, the tail is drawn on the glass: at least kMinInk pixels
//     of ink where the bubble stands under it;
//  4. at every read, the tail's own ink lands where its styles put it: read
//     with the bubble hidden as well (so the part of a tail that has the
//     bubble's color over the bubble is read too), its bounding box is the
//     box's 12 px square turned by its transform_rotation (450, 45 degrees)
//     about its transform_pivot (unset, so the box's top-left corner), to
//     within 2 px a side (the radius-2 corners round the diamond's tips);
//  5. at every read, each of F185's options, rendered on the same frame (the
//     tail's styles changed for that render alone and put back), keeps 3 and
//     4: radius 0, a centered pivot, both, and a triangle drawn in the
//     tail's own box with no transform (apex at the box's top center, base
//     on its bottom edge, filled with the tail's own bg color, the bubble's).
//     F185 is a decision this test does not take: whichever of those it
//     takes, drawn that way on the same 12 px object, keeps the test green
//     (a tail made of another widget would also have to change find_parts);
//  6. the bubble typed every line story::kHello has, whole and in the
//     script's order (each line counted at its first letter, the pseudonym
//     in place of its %s), and the meeting was remembered.
//
// Printed, not held: where the diamond stands against the bubble (its center
// against the bubble's center line, how far it pokes above the top edge and
// reaches into the bubble), and where each of F185's options would land on
// the same frame.
//
// Prints "ALL LVGL 9 SPLASH TAIL TESTS PASSED" on success.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <lvgl.h>
#include <config.h>
#include "pins.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "identity/device_pseudonym.h"
#include "story/story_scripts.h"

#include "canary/glass_settings.h"
#include "canary/hal/display.h"
#include "canary/log.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/lvgl_port.h"
#include "canary/ui/splash.h"
#include "canary/ui/splash_layout.h"

#if LVGL_VERSION_MAJOR != 9
#error "glass_splash_lvgl9_test.cpp builds against LVGL 9 (glass_turn_lvgl9.sh)"
#endif
#if defined(CD_FLAVOR_DASH)
#define GLASS_TURNS 1
#elif defined(CD_FLAVOR_NIGHTSTAND) && defined(CD_LEAN_BUILD) && !defined(CD_AMOLED_GLASS)
#define GLASS_TURNS 0
#else
#error "glass_splash_lvgl9_test.cpp builds the dash or the nightstand-c6 config (glass_turn_lvgl9.sh)"
#endif

namespace sl = canary::ui::splashlayout;

// ── The bench this test stands in for ───────────────────────────────────
namespace {
uint32_t g_now = 0;
int g_fail = 0;
int g_checks = 0;
#if GLASS_TURNS
constexpr int PW = LCD_WIDTH, PH = LCD_HEIGHT;  // the dash panel's native scan
#else
constexpr int PW = TFT_WIDTH, PH = TFT_HEIGHT;  // the C6 ST7789's 172x320
#endif
Arduino_GFX g_panel(PW, PH);
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
Arduino_GFX* gfx() { return &g_panel; }
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

// A check made at every tick or capture: each evaluation counts, and only
// the first failure of its kind (`fails`) is printed.
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

constexpr long kMinInk = 30;  // a drawn tail; 0 where 8.4 draws none (F185)
constexpr int kWin = 40;      // the window read around the tail's box

// ── Reading the glass ────────────────────────────────────────────────────
lv_display_rotation_t g_rot = LV_DISPLAY_ROTATION_0;
int g_lw = PW, g_lh = PH;  // the logical canvas

// Where the port draws logical (lx, ly) on the native panel (the quarter
// turns glass_settings.h states; glass_turn_lvgl9_test holds the port to
// them pixel for pixel).
uint16_t logical_px(int lx, int ly) {
  int px = lx, py = ly;
  switch (g_rot) {
    case LV_DISPLAY_ROTATION_90:  px = ly;          py = PH - 1 - lx; break;
    case LV_DISPLAY_ROTATION_180: px = PW - 1 - lx; py = PH - 1 - ly; break;
    case LV_DISPLAY_ROTATION_270: px = PW - 1 - ly; py = lx;          break;
    default: break;
  }
  return g_panel.fb[(size_t)py * PW + px];
}

void render_whole() {
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(nullptr);
}

struct Box {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // inclusive
};
struct Ink {
  long n = 0;
  Box b{9999, 9999, -9999, -9999};
};

// The window around `c` read off the glass.
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

// The tail's ink: the canvas rendered whole with the tail and without it;
// the pixels that differ are the tail's. With `under` null that is the
// glass as it stands; with the bubble as `under`, the bubble is hidden for
// both renders, so the tail is read against what lies behind the bubble and
// a part of it in the bubble's own color counts too. The glass is left as
// the splash drew it.
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
  lv_obj_remove_flag(tail, LV_OBJ_FLAG_HIDDEN);
  if (under != nullptr) lv_obj_remove_flag(under, LV_OBJ_FLAG_HIDDEN);
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
// transform_rotation about its transform_pivot (lv_refr.c resolves a pivot
// from the box's top-left corner, a percentage of the box's side).
Box styled_extent(const lv_obj_t* tail) {
  lv_area_t c;
  lv_obj_get_coords(tail, &c);
  const int32_t w = lv_area_get_width(&c), h = lv_area_get_height(&c);
  int32_t px = lv_obj_get_style_transform_pivot_x(tail, LV_PART_MAIN);
  int32_t py = lv_obj_get_style_transform_pivot_y(tail, LV_PART_MAIN);
  if (LV_COORD_IS_PCT(px)) px = lv_pct_to_px(px, w);
  if (LV_COORD_IS_PCT(py)) py = lv_pct_to_px(py, h);
  const double a = lv_obj_get_style_transform_rotation(tail, LV_PART_MAIN) / 10.0 * M_PI / 180.0;
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

// One read of the tail under its styles as they stand: its ink on the glass
// (checks 3 and the report), its own ink with the bubble hidden and the
// extent its styles give it (check 4).
struct Read {
  Ink glass, own;
  Box want;
  bool drawn() const { return glass.n >= kMinInk; }
  bool lands() const {
    return own.n > 0 && abs(own.b.x0 - want.x0) <= 2 && abs(own.b.x1 - want.x1) <= 2 &&
           abs(own.b.y0 - want.y0) <= 2 && abs(own.b.y1 - want.y1) <= 2;
  }
};
Read read_tail(lv_obj_t* tail, lv_obj_t* bub) {
  Read r;
  r.glass = tail_ink(tail);
  r.own = tail_ink(tail, bub);
  r.want = styled_extent(tail);
  return r;
}

// ── F185's options, rendered on the same frame ──────────────────────────
lv_color_t g_tri_color;
void draw_triangle_cb(lv_event_t* e) {
  const lv_obj_t* obj = lv_event_get_target_obj(e);
  lv_area_t c;
  lv_obj_get_coords(obj, &c);
  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  d.p[0].x = c.x1;
  d.p[0].y = c.y2 + 1;
  d.p[1].x = c.x2 + 1;
  d.p[1].y = c.y2 + 1;
  d.p[2].x = (c.x1 + c.x2 + 1) / 2;
  d.p[2].y = c.y1;
  d.color = g_tri_color;
  d.opa = LV_OPA_COVER;
  lv_draw_triangle(lv_event_get_layer(e), &d);
}

enum class Option { Radius0, CenteredPivot, Both, Triangle };
const char* option_name(Option o) {
  switch (o) {
    case Option::Radius0:       return "radius 0, turned as today";
    case Option::CenteredPivot: return "a centered pivot";
    case Option::Both:          return "radius 0 and a centered pivot";
    case Option::Triangle:      return "a triangle in the box, no transform";
  }
  return "?";
}

// The tail read with option `o`'s styles, then put back as the splash set it.
Read option_read(lv_obj_t* tail, lv_obj_t* bub, Option o) {
  const int32_t radius = lv_obj_get_style_radius(tail, LV_PART_MAIN);
  const int32_t rot = lv_obj_get_style_transform_rotation(tail, LV_PART_MAIN);
  const int32_t pvx = lv_obj_get_style_transform_pivot_x(tail, LV_PART_MAIN);
  const int32_t pvy = lv_obj_get_style_transform_pivot_y(tail, LV_PART_MAIN);
  const lv_opa_t bg_opa = lv_obj_get_style_bg_opa(tail, LV_PART_MAIN);
  const int32_t border = lv_obj_get_style_border_width(tail, LV_PART_MAIN);
  if (o == Option::Radius0 || o == Option::Both) lv_obj_set_style_radius(tail, 0, 0);
  if (o == Option::CenteredPivot || o == Option::Both) {
    lv_obj_set_style_transform_pivot_x(tail, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(tail, lv_pct(50), 0);
  }
  if (o == Option::Triangle) {
    g_tri_color = lv_obj_get_style_bg_color(tail, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(tail, 0, 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tail, 0, 0);
    lv_obj_add_event_cb(tail, draw_triangle_cb, LV_EVENT_DRAW_MAIN_END, nullptr);
  }
  const Read r = read_tail(tail, bub);
  if (o == Option::Triangle) lv_obj_remove_event_cb(tail, draw_triangle_cb);
  lv_obj_set_style_radius(tail, radius, 0);
  lv_obj_set_style_transform_rotation(tail, rot, 0);
  lv_obj_set_style_transform_pivot_x(tail, pvx, 0);
  lv_obj_set_style_transform_pivot_y(tail, pvy, 0);
  lv_obj_set_style_bg_opa(tail, bg_opa, 0);
  lv_obj_set_style_border_width(tail, border, 0);
  render_whole();
  return r;
}

// The ink against the bubble, in words.
void say_landing(const char* what, const Ink& ink, const lv_area_t& bub) {
  const int line2 = bub.x1 + bub.x2 + 1;  // twice the bubble's center line
  const int ink2 = ink.b.x0 + ink.b.x1 + 1;
  printf("      %-37s ink %3ld px at x %d..%d, y %d..%d: its center %+.1f px from the "
         "bubble's center line, %d px above its top edge, %d px into it\n",
         what, ink.n, ink.b.x0, ink.b.x1, ink.b.y0, ink.b.y1, (ink2 - line2) / 2.0,
         bub.y1 - ink.b.y0, ink.b.y1 - bub.y1 + 1);
}

// ── Watching the splash ─────────────────────────────────────────────────
struct Watch {
  const char* tag = "";
  lv_obj_t* home = nullptr;
  bool capturing = false;
  int ticks = 0;          // observed ticks with the splash up
  int bubble_ticks = 0;   // ...with the bubble up
  int mismatched = 0;     // ...where the tail and the bubble disagreed
  int captures = 0;
  int seat_fails = 0, ink_fails = 0, land_fails = 0, option_fails = 0;
  long ink_lo = 1 << 30, ink_hi = 0;
  bool reported = false;
  std::string last_text;   // the bubble's text at the last tick it was up
  // The lines the bubble typed, each as far as it got: a line starts at its
  // first letter, after the bubble emptied or with a text that does not
  // carry on the one before it.
  std::vector<std::string> typed;
  bool emptied = false;
  lv_area_t last_tail{0, 0, -1, -1}, last_bub{0, 0, -1, -1};
};
Watch g_w;

bool same_area(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

// The splash's tail (the plain kTailSide square with no children, however
// it is styled, as test_splash_scenes finds it) and bubble (the one child
// holding a single label). The screen is laid out first.
void find_parts(const lv_obj_t* scr, lv_obj_t** tail, lv_obj_t** bub) {
  *tail = *bub = nullptr;
  const uint32_t n = lv_obj_get_child_count(scr);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(scr, (int32_t)i);
    if (c == canary::ui::canary_mark_obj()) continue;
    if (lv_obj_check_type(c, &lv_obj_class) && lv_obj_get_child_count(c) == 0 &&
        lv_obj_get_width(c) == sl::kTailSide && lv_obj_get_height(c) == sl::kTailSide)
      *tail = c;
    if (lv_obj_get_child_count(c) == 1 &&
        lv_obj_check_type(lv_obj_get_child(c, 0), &lv_label_class))
      *bub = c;
  }
}

void capture(lv_obj_t* tail, lv_obj_t* bub) {
  Watch& W = g_w;
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
       "\"%s\") it sits at (%d,%d)",
       W.tag, (int)lv_area_get_width(&t), (int)lv_area_get_height(&t), (int)t.x1, (int)t.y1,
       (int)b.x1, (int)b.x2, (int)b.y1, text, want_x, want_y);
  // 3 + 4: drawn on the glass, its own ink where its styles put it.
  const Read r = read_tail(tail, bub);
  const Ink& ink = r.glass;
  if (ink.n < W.ink_lo) W.ink_lo = ink.n;
  if (ink.n > W.ink_hi) W.ink_hi = ink.n;
  HOLD(r.drawn(), W.ink_fails,
       "%s: the tail draws %ld px of ink (box at %d,%d, \"%s\"); a drawn tail has at "
       "least %ld",
       W.tag, ink.n, (int)t.x1, (int)t.y1, text, kMinInk);
  HOLD(r.lands(), W.land_fails,
       "%s: the tail's own ink spans x %d..%d, y %d..%d; its box (%d,%d) turned %d about "
       "its pivot spans x %d..%d, y %d..%d (\"%s\")",
       W.tag, r.own.b.x0, r.own.b.x1, r.own.b.y0, r.own.b.y1, (int)t.x1, (int)t.y1,
       (int)lv_obj_get_style_transform_rotation(tail, LV_PART_MAIN), r.want.x0, r.want.x1,
       r.want.y0, r.want.y1, text);
  // 5: each of F185's options, on this frame, keeps 3 and 4.
  const bool report = !W.reported && text[0] != '\0';
  if (report) {
    // The report, once a canvas, on the frame of its first typed line.
    W.reported = true;
    printf("    the tail's box %dx%d at (%d,%d) on a %dx%d bubble at (%d,%d), \"%.32s\":\n",
           (int)lv_area_get_width(&t), (int)lv_area_get_height(&t), (int)t.x1, (int)t.y1,
           (int)lv_area_get_width(&b), (int)lv_area_get_height(&b), (int)b.x1, (int)b.y1, text);
    say_landing("today (radius 2, turned 45 about (0,0))", ink, b);
  }
  for (Option o : {Option::Radius0, Option::CenteredPivot, Option::Both, Option::Triangle}) {
    const Read alt = option_read(tail, bub, o);
    if (report) say_landing(option_name(o), alt.glass, b);
    HOLD(alt.drawn() && alt.lands(), W.option_fails,
         "%s: F185's option \"%s\" would fail the test: %ld px of ink on the glass (at "
         "least %ld), its own ink x %d..%d, y %d..%d against its styles' x %d..%d, "
         "y %d..%d (\"%s\")",
         W.tag, option_name(o), alt.glass.n, kMinInk, alt.own.b.x0, alt.own.b.x1,
         alt.own.b.y0, alt.own.b.y1, alt.want.x0, alt.want.x1, alt.want.y0, alt.want.y1,
         text);
  }
}

void observe() {
  Watch& W = g_w;
  if (W.capturing) return;
  lv_obj_t* scr = lv_screen_active();
  if (scr == nullptr || scr == W.home) return;
  W.ticks++;
  lv_obj_update_layout(scr);
  lv_obj_t* tail = nullptr;
  lv_obj_t* bub = nullptr;
  find_parts(scr, &tail, &bub);
  HOLD(tail != nullptr, W.mismatched, "%s: no %d px tail square on the splash", W.tag,
       sl::kTailSide);
  const bool tail_up = tail != nullptr && !lv_obj_has_flag(tail, LV_OBJ_FLAG_HIDDEN);
  const bool bub_up = bub != nullptr && !lv_obj_has_flag(bub, LV_OBJ_FLAG_HIDDEN);
  // 1: up together.
  HOLD(tail_up == bub_up, W.mismatched, "%s: the tail is %s while the bubble is %s", W.tag,
       tail_up ? "up" : "hidden", bub_up ? "up" : "hidden");
  if (!tail_up || !bub_up) return;
  W.bubble_ticks++;
  lv_area_t t, b;
  lv_obj_get_coords(tail, &t);
  lv_obj_get_coords(bub, &b);
  const std::string text = lv_label_get_text(lv_obj_get_child(bub, 0));
  // 6: the lines typed, each counted at its first letter.
  if (text.empty()) {
    W.emptied = true;
  } else {
    if (W.typed.empty() || W.emptied ||
        text.compare(0, W.typed.back().size(), W.typed.back()) != 0)
      W.typed.push_back(text);
    else
      W.typed.back() = text;
    W.emptied = false;
  }
  // A read wherever the text starts over (emptied, or not carrying on the
  // last tick's text) or the tail or the bubble moved.
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
// The lines story::kHello types, as the script writes them, the pseudonym
// (the stand-in splash.cpp reads) in place of each %s.
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

void play(const char* tag) {
  Watch& W = g_w;
  W = Watch();
  W.tag = tag;
  native9::prefs().clear();  // never met: the first meeting
  g_lw = canary::ui::lvgl_port_width();
  g_lh = canary::ui::lvgl_port_height();
  g_rot = lv_display_get_rotation(lv_display_get_default());
  CHECK(lv_display_get_horizontal_resolution(nullptr) == g_lw &&
            lv_display_get_vertical_resolution(nullptr) == g_lh,
        "%s: LVGL's canvas is %dx%d, the port's %dx%d", tag,
        (int)lv_display_get_horizontal_resolution(nullptr),
        (int)lv_display_get_vertical_resolution(nullptr), g_lw, g_lh);
  W.home = lv_screen_active();
  printf("  %s, canvas %dx%d:\n", tag, g_lw, g_lh);
  const uint32_t t0 = g_now;
  canary::ui::splash_play(1700);
  canary::ui::splash_reveal();
  CHECK(native9::prefs()["scv-hello/met"] == 1, "%s: the meeting was not remembered", tag);
  CHECK(W.captures > 0, "%s: the tail was never read over %d ticks", tag, W.ticks);
  // 6: every line of the script, whole, in its order.
  const std::vector<std::string> want = hello_lines();
  size_t same = 0;
  while (same < want.size() && same < W.typed.size() && W.typed[same] == want[same]) same++;
  CHECK(same == want.size() && W.typed.size() == want.size(),
        "%s: the bubble typed %zu lines where story::kHello has %zu; line %zu differs (typed "
        "\"%s\", the script's \"%s\")",
        tag, W.typed.size(), want.size(), same + 1,
        same < W.typed.size() ? W.typed[same].c_str() : "(nothing)",
        same < want.size() ? want[same].c_str() : "(nothing)");
  printf("    %d ticks (%.1f s), %d with the bubble up, %zu of the script's %zu lines typed "
         "whole, the tail read off the frame %d times, %ld..%ld px of ink, %d checks "
         "failed\n",
         W.ticks, (g_now - t0) / 1000.0, W.bubble_ticks, same, want.size(), W.captures,
         W.ink_lo, W.ink_hi,
         W.seat_fails + W.ink_fails + W.land_fails + W.option_fails + W.mismatched);
}

}  // namespace

int main() {
  printf("the splash's tail (LVGL %d.%d.%d, the real renderer, %s config):\n",
         LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
         GLASS_TURNS ? "dash" : "nightstand-c6");
  CHECK(canary::ui::lvgl_port_init(), "lvgl_port_init failed");
  if (lv_display_get_default() == nullptr) return 1;
  // main.cpp: a landscape boot (rotation 0) changes nothing; a saved
  // portrait is worn before the splash (the dash only).
#if GLASS_TURNS
  canary::ui::lvgl_port_set_rotation(canary::glass::ROT_LANDSCAPE);
  play("dash, landscape");
  canary::ui::lvgl_port_set_rotation(canary::glass::ROT_PORTRAIT);
  play("dash, portrait");
#else
  play("nightstand-c6");
#endif
  if (g_fail) {
    printf("%d of %d LVGL 9 splash tail checks FAILED\n", g_fail, g_checks);
    return 1;
  }
  printf("ALL LVGL 9 SPLASH TAIL TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
