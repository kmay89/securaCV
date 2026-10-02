// Host test: where the boot splash DRAWS the bird and the speech bubble (F88).
//
// test_splash_layout.cpp holds splash_layout.h's seats on every canvas the
// splash runs on. This test holds splash.cpp to them. It compiles the REAL
// src/ui/splash.cpp and src/ui/canary_mark.cpp against fake_lvgl/lvgl.h
// (LVGL 8's position rules; a fixed-width wrapped label takes the lines
// LVGL 8.4 wraps it into, and a content-high object its children's height),
// with the Arduino clock on the fake tick (delay() runs the anims), the
// "met" flag in a fake Preferences and the pseudonym the test picks. Then,
// on every canvas of this build's glass, with both type ladders and the
// hop at the temperament's ceiling, it plays both splashes the way main.cpp
// does — the first meeting (the whole story::kHello script, typed) and,
// once splash_play() has remembered it, hello again — and reads the glass
// at every refresh (each lv_timer_handler() call):
//  * the bird is the mark's square at the canvas's center column, and its
//    drawn box breathes down to exactly the seat splash_layout.h's seat()
//    names plus kBreath, hops no higher than kHopReach above it (the hop is
//    seen), and stays on the canvas;
//  * the speech bubble is up only in the first meeting, is splash_layout.h's
//    bubble_w() for the canvas (the family's width, no wider than the canvas
//    less its margin, F160), and sits where seat() says: hung from
//    seat().bubble_top under a moved bird, centered at the family's bubble
//    offset otherwise, in whatever height the typed line wraps to; it stays
//    on the canvas, its sides too (no overhang, F160), and a hung one stays
//    clear of the bird by kGap over its whole breath;
//  * the tail is up whenever the bubble is, and at every refresh sits on
//    the bubble's top edge as it stands, centered and kTailInset px into it
//    (F158: it was aligned once, to the empty bubble, and a centered bubble
//    grew past it as its line wrapped);
//  * the tallest bubble it drew is the tallest test_splash_layout measures
//    for that pseudonym (the bubble's width, padding, border and label
//    inset are the header's, read off what was drawn);
//  * the splash leaves the screen it found, behind its curtain, and
//    splash_reveal() lifts the curtain.
// Printed, not held: the centered bubble's reach into the bird's box (the
// AMOLED's, filed). The fake's anims are linear, so the hop's drawn apex is
// canary_mark's apex without LVGL's overshoot; test_splash_layout derives
// the overshoot into kHopReach, and this test holds the drawn hop under it.
//
// Three builds, because the glass family is the config's call (splash.cpp
// branches on CD_FLAVOR_WATCH, which the nightstand line aliases): the
// watch's (the round watch, with its disc's static_assert), the
// nightstand's (every CD_FLAVOR_NIGHTSTAND env: the portrait nightstand
// line, the nightlight in both rotations, the touch169 and the AMOLED) and
// the dash's (the 800x480 dash, dash7 and nightstand7, and the dash's
// panel turned). The test refuses a splash.cpp that branches on anything
// else, since these builds would not vary it.
//
// Prints "ALL SPLASH SCENES TESTS PASSED" on success.

#include <config.h>
#include <lvgl.h>
#include <Preferences.h>

#include "canary/hal/display.h"
#include "canary/ui/canary_mark.h"
#include "canary/ui/character.h"
#include "canary/ui/splash.h"
#include "canary/ui/theme.h"
#include "identity/device_pseudonym.h"
#include "splash_test_env.h"

#if defined(CD_FLAVOR_WATCH) && !defined(CD_FLAVOR_NIGHTSTAND)
static const int kBuild = 0;  // the round watch
#elif defined(CD_FLAVOR_NIGHTSTAND)
static const int kBuild = 1;  // small rectangular glass
#elif defined(CD_FLAVOR_DASH)
static const int kBuild = 2;  // wide glass
#else
#error "test_splash_scenes builds against the watch, nightstand or dash config"
#endif
static const char* const kBuildName[3] = {"round watch", "small glass",
                                          "wide glass"};
static const sl::Family& kFam =
    kBuild == 2 ? sl::kWideGlass : sl::kSmallGlass;
// The family as this canvas draws it (its bubble's width, F160): set per
// canvas in main().
static sl::Family g_fam_drawn = kFam;

namespace canary::ui {
extern lv_obj_t* s_curtain;  // splash.cpp's exit curtain
}  // namespace canary::ui

// ── the theme and the board, from the canvas's ladder ─────────────────────
namespace {

int g_fam = kStd;  // character.cpp's ladder family for this canvas
int g_which = 0;   // 0: the default Character, 1: Heirloom

lv_font_t g_fonts[16];
int g_font_n = 0;

int font_glyph(const lv_font_t* f, uint32_t a, uint32_t b) {
  return glyph_px(*(const Face*)f->dsc, a, b);
}

// The built-in Montserrat face of `size`, with LVGL's own metrics; null
// when montserrat_metrics.h does not carry it (the wordmark's title face on
// small glass: nothing here reads the wordmark's box).
const lv_font_t* font_or_null(int size) {
  for (int i = 0; i < g_font_n; ++i)
    if (((const Face*)g_fonts[i].dsc)->size == size) return &g_fonts[i];
  const Face* f = face_of(size);
  if (f == nullptr || g_font_n >= 16) return nullptr;
  lv_font_t& out = g_fonts[g_font_n++];
  out.line_height = f->line_height;
  out.glyph_w = font_glyph;
  out.dsc = f;
  return &out;
}

const lv_font_t* role_or_null(int r) {
  return font_or_null(g_ladder[g_fam][g_which][r]);
}

}  // namespace

namespace canary::ui {
const lv_font_t* font_title() { return role_or_null(kTitle); }
const lv_font_t* font_caption() { return role_or_null(kCaption); }
// The bubble's face: it must be carried (the test measures the bubble).
const lv_font_t* font_label() {
  const lv_font_t* f = role_or_null(kLabel);
  CHECK(f != nullptr, "montserrat_%d (the label face) is not carried",
        g_ladder[g_fam][g_which][kLabel]);
  return f;
}
lv_color_t col_bg() { return lv_color_hex(0x000000); }
lv_color_t col_surface() { return lv_color_hex(0x161616); }
lv_color_t col_edge() { return lv_color_hex(0x262626); }
lv_color_t col_text() { return lv_color_hex(0xEDEDED); }
lv_color_t col_muted() { return lv_color_hex(0x9A9A9A); }
const Voice& active_voice() {
  static Voice v = Voice();
  v.hello_again = "good to see you";
  return v;
}
}  // namespace canary::ui

namespace canary::hal {
TouchSample touch_read() { return TouchSample(); }  // nobody taps
void backlight_set(uint8_t) {}
}  // namespace canary::hal

// ── reading the glass ─────────────────────────────────────────────────────
namespace {

struct Read {
  std::string who;
  int w, h;
  bool first;
  sl::Seat seat;
  lv_obj_t* home;
  // What the refreshes saw.
  int frames;
  int bird_frames, bird_x_lo, bird_x_hi, bird_y_lo, bird_y_hi;
  int bird_d;
  int bubble_frames, bubble_tallest, bubble_w;
  std::string bubble_line;  // the line the tallest bubble held
  int min_gap;              // the bubble's top less the bird's bottom
  bool word_shown;
  int placement_fails;      // refreshes whose bubble sat off its seat
  int off_canvas_fails;     // refreshes whose bubble left the canvas
  int overhang;             // how far the bubble's sides pass the canvas's
  int tail_frames;          // refreshes that read the tail with the bubble
  int tail_fails;           // ...and found it off the bubble's top edge
  int tail_moves;           // times the tail's seat changed while shown
  int tail_last_y;
};

Read g_read;

// The tail: the plain square splash.cpp turns under the bubble.
lv_obj_t* find_tail(lv_obj_t* scr) {
  for (size_t i = 0; i < scr->children.size(); ++i) {
    lv_obj_t* c = scr->children[i];
    if (c->kind == fake_lvgl::kObj && c != canary_mark_obj() &&
        c->children.empty() && c->w == sl::kTailSide && c->h == sl::kTailSide)
      return c;
  }
  return nullptr;
}

lv_obj_t* find_bubble(lv_obj_t* scr) {
  for (size_t i = 0; i < scr->children.size(); ++i) {
    lv_obj_t* c = scr->children[i];
    if (c->kind == fake_lvgl::kObj && c != canary_mark_obj() &&
        c->children.size() == 1 && c->children[0]->kind == fake_lvgl::kLabel)
      return c;
  }
  return nullptr;
}

// One refresh: lay the splash's screen out and read the bird and bubble.
void refresh() {
  Read& R = g_read;
  lv_obj_t* scr = lv_scr_act();
  if (scr == nullptr || scr == R.home) return;
  lv_obj_update_layout(scr);
  R.frames++;
  const char* n = R.who.c_str();

  lv_obj_t* bird = canary_mark_obj();
  const bool bird_up = bird != nullptr && bird->parent == scr &&
                       !lv_obj_has_flag(bird, LV_OBJ_FLAG_HIDDEN);
  if (bird_up) {
    R.bird_frames++;
    R.bird_d = bird->w == bird->h ? bird->w : -1;
    R.bird_x_lo = std::min(R.bird_x_lo, (int)bird->x1);
    R.bird_x_hi = std::max(R.bird_x_hi, (int)bird->x1);
    R.bird_y_lo = std::min(R.bird_y_lo, (int)bird->y1);
    R.bird_y_hi = std::max(R.bird_y_hi, (int)bird->y1);
  }
  for (size_t i = 0; i < scr->children.size(); ++i) {
    const lv_obj_t* c = scr->children[i];
    if (c->kind == fake_lvgl::kLabel && c->text == "SecuraCV" &&
        !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
      R.word_shown = true;
  }

  lv_obj_t* bub = find_bubble(scr);
  lv_obj_t* tail = find_tail(scr);
  if (tail == nullptr && R.tail_fails++ == 0)
    CHECK(false, "%s: no %d px tail square on the splash", n, sl::kTailSide);
  const bool tail_up = tail != nullptr && !lv_obj_has_flag(tail, LV_OBJ_FLAG_HIDDEN);
  if (bub == nullptr || lv_obj_has_flag(bub, LV_OBJ_FLAG_HIDDEN)) {
    if (tail_up && R.tail_fails++ == 0)
      CHECK(false, "%s: the tail is up without its bubble", n);
    return;
  }
  R.bubble_frames++;
  R.bubble_w = bub->w;
  const int bh = bub->h;
  if (bh > R.bubble_tallest) {
    R.bubble_tallest = bh;
    R.bubble_line = bub->children[0]->text;
  }
  // Where splash_layout.h's seat() puts it: hung from bubble_top under a
  // moved bird; centered at the family's offset (LV_ALIGN_CENTER of its
  // drawn height) otherwise.
  const int want_y = R.seat.hang ? R.seat.bubble_top
                                 : R.h / 2 - bh / 2 + kFam.bubble_off;
  const int want_x = R.w / 2 - bub->w / 2;
  if (bub->y1 != want_y || bub->x1 != want_x) {
    if (R.placement_fails++ == 0)
      CHECK(false, "%s: the bubble (%d px tall, \"%s\") is drawn at (%d, %d); "
            "seat() puts it at (%d, %d) (%s)", n, bh,
            bub->children[0]->text.c_str(), bub->x1, bub->y1, want_x, want_y,
            R.seat.hang ? "hung from bubble_top" : "centered");
  }
  if (bub->y1 < 0 || bub->y1 + bh > R.h) {
    if (R.off_canvas_fails++ == 0)
      CHECK(false, "%s: the bubble (y %d..%d, \"%s\") leaves the %d px tall "
            "canvas", n, bub->y1, bub->y1 + bh,
            bub->children[0]->text.c_str(), R.h);
  }
  // F160: the bubble's sides stay on the canvas.
  R.overhang = std::max(R.overhang, std::max(-bub->x1, bub->x1 + bub->w - R.w));
  // F158: the tail sits on the bubble's top edge as it stands now,
  // centered, kTailInset px into it — not where the empty bubble's edge was.
  if (tail != nullptr) {
    R.tail_frames++;
    const int want_tx = bub->x1 + bub->w / 2 - tail->w / 2;
    const int want_ty = bub->y1 - tail->h + sl::kTailInset;
    if (R.tail_frames > 1 && tail->y1 != R.tail_last_y) R.tail_moves++;
    R.tail_last_y = tail->y1;
    if ((!tail_up || tail->x1 != want_tx || tail->y1 != want_ty) &&
        R.tail_fails++ == 0)
      CHECK(false, "%s: the tail is %s at (%d, %d); on the bubble (%d px tall "
            "at y %d, \"%s\") it sits at (%d, %d)", n,
            tail_up ? "drawn" : "hidden", tail->x1, tail->y1, bh, bub->y1,
            bub->children[0]->text.c_str(), want_tx, want_ty);
  }
  if (bird_up) R.min_gap = std::min(R.min_gap, bub->y1 - (bird->y1 + bird->h));
}

// The pseudonym whose letters wrap the first meeting's bubble tallest.
std::string worst_subject(const Face& f) {
  int best = -1;
  std::string out;
  for (size_t c = 0; c < g_minted.alpha.size(); ++c) {
    const std::string s((size_t)g_hex_len, g_minted.alpha[c]);
    const int h = tallest_for(f, g_fam_drawn, s, nullptr);
    if (h > best) {
      best = h;
      out = s;
    }
  }
  return out;
}

// One splash on one canvas: play it, then hold what was drawn.
void play(const Canvas& cv, int which, bool first, const std::string& subject) {
  char who[160];
  std::snprintf(who, sizeof(who), "%s %dx%d/%s, pseudonym %c..., %s",
                cv.what.c_str(), cv.w, cv.h, which ? "heirloom" : "default",
                subject[0], first ? "first meeting" : "hello again");
  Read& R = g_read;
  R = Read();
  R.who = who;
  R.w = cv.w;
  R.h = cv.h;
  R.first = first;
  R.seat = sl::seat(cv.h, kFam, first);
  R.bird_x_lo = R.bird_y_lo = 1 << 30;
  R.bird_x_hi = R.bird_y_hi = -(1 << 30);
  R.min_gap = 1 << 30;
  R.overhang = -(1 << 30);
  R.tail_last_y = -(1 << 30);
  const char* n = R.who.c_str();

  const uint8_t met = fake_prefs::uchars()["scv-hello/met"];
  CHECK(met == (first ? 0 : 1), "%s: the \"met\" flag is %d going in", n, met);

  R.home = lv_obj_create(nullptr);
  lv_scr_load(R.home);
  fake_lvgl::refresh_hook() = refresh;
  // Hello again holds long enough for the breath to swing out after the
  // hop (canary_mark: a 560 ms hop, then a 1.4 s half-breath at most x1.25).
  canary::ui::splash_play(first ? 1200 : 4000);
  fake_lvgl::refresh_hook() = nullptr;

  // It leaves the screen it found, behind its curtain; splash_reveal()
  // lifts the curtain.
  CHECK(lv_scr_act() == R.home, "%s: splash_play left another screen up", n);
  CHECK(canary::ui::s_curtain != nullptr &&
            canary::ui::s_curtain->w == cv.w &&
            canary::ui::s_curtain->h == cv.h + 4,
        "%s: no %dx%d curtain dropped", n, cv.w, cv.h + 4);
  canary::ui::splash_reveal();
  CHECK(canary::ui::s_curtain == nullptr, "%s: splash_reveal left the curtain",
        n);
  CHECK(fake_prefs::uchars()["scv-hello/met"] == 1,
        "%s: the meeting was not remembered", n);
  lv_obj_del(R.home);

  // The bird: the mark's square, centered, on its seat.
  const int d = kFam.bird;
  const int seat_top = sl::bird_top(cv.h, d, R.seat.bird_off);
  const int x = cv.w / 2 - d / 2;
  CHECK(R.frames > 0 && R.bird_frames > 0, "%s: the bird never showed (%d "
        "refreshes)", n, R.frames);
  if (R.bird_frames == 0) return;
  CHECK(R.bird_d == d && R.bird_x_lo == x && R.bird_x_hi == x,
        "%s: the bird is %d px at x %d..%d; the family's is %d px at x %d", n,
        R.bird_d, R.bird_x_lo, R.bird_x_hi, d, x);
  CHECK(R.bird_y_hi == seat_top + sl::kBreath,
        "%s: the bird breathes down to y %d; seat() puts it at y %d, %d px "
        "of breath below", n, R.bird_y_hi, seat_top, sl::kBreath);
  CHECK(R.bird_y_lo >= seat_top - sl::kHopReach &&
            R.bird_y_lo < seat_top - sl::kBreath,
        "%s: the bird hops to y %d from its seat at y %d (kHopReach %d; a hop "
        "must be seen)", n, R.bird_y_lo, seat_top, sl::kHopReach);
  CHECK(R.bird_y_lo >= 0 && R.bird_y_hi + d <= cv.h,
        "%s: the bird is drawn at y %d..%d on a %d px canvas", n, R.bird_y_lo,
        R.bird_y_hi + d, cv.h);

  char line[320];
  if (!first) {
    CHECK(R.bubble_frames == 0, "%s: the speech bubble showed", n);
    CHECK(R.word_shown, "%s: the wordmark never showed", n);
    std::snprintf(line, sizeof(line), "hello again bird y %3d..%3d (seat %3d)",
                  R.bird_y_lo, R.bird_y_hi + d, seat_top);
    std::printf("      %s\n", line);
    return;
  }

  // The bubble: up, splash.cpp's width, its tallest the measured tallest,
  // on its seat (checked at every refresh, above), clear of a moved bird.
  CHECK(R.bubble_frames > 0 && R.word_shown,
        "%s: the bubble (%d refreshes) or the wordmark (%d) never showed", n,
        R.bubble_frames, R.word_shown);
  const int want_w = sl::bubble_w(cv.w, kFam);
  CHECK(R.bubble_w == want_w && want_w == g_fam_drawn.bubble_w,
        "%s: the bubble is %d px wide; bubble_w() gives %d on this canvas "
        "(the family's %d)", n, R.bubble_w, want_w, kFam.bubble_w);
  // F160: no side of it past the canvas's.
  CHECK(R.overhang <= 0, "%s: the %d px bubble runs %d px past each side of "
        "the %d px canvas", n, R.bubble_w, R.overhang, cv.w);
  // F158: the tail was read with the bubble, on its edge every time (each
  // miss is reported at the refresh it happened).
  CHECK(R.tail_frames == R.bubble_frames && R.tail_frames > 0,
        "%s: the tail was read at %d of the bubble's %d refreshes", n,
        R.tail_frames, R.bubble_frames);
  const Face* f = face_of(g_ladder[g_fam][which][kLabel]);
  const int tallest = f ? tallest_for(*f, g_fam_drawn, subject, nullptr) : -1;
  CHECK(R.bubble_tallest == tallest,
        "%s: the tallest bubble drawn is %d px (\"%s\"); test_splash_layout "
        "measures %d", n, R.bubble_tallest, R.bubble_line.c_str(), tallest);
  // Wherever the bird left its usual seat, the bubble keeps clear of it
  // (it hangs from under it); where it did not, the overlap is printed.
  const bool moved = R.seat.bird_off != kFam.intro_off;
  CHECK(R.seat.hang == moved, "%s: seat() %s the bubble though the bird %s",
        n, R.seat.hang ? "hangs" : "centers", moved ? "moved" : "did not move");
  if (moved)
    CHECK(R.min_gap >= sl::kGap, "%s: the bubble comes within %d px of the "
          "moved bird (kGap %d)", n, R.min_gap, sl::kGap);
  char reach[96] = "";
  if (R.min_gap < 0)
    std::snprintf(reach, sizeof(reach), ", in the bird's box by %d px",
                  -R.min_gap);
  std::snprintf(line, sizeof(line),
                "first meeting bird y %3d..%3d (seat %3d%s), bubble %s %d px "
                "wide (%d px clear a side), tallest %3d px%s; the tail moved "
                "%d times with it; %d refreshes", R.bird_y_lo, R.bird_y_hi + d,
                seat_top, R.seat.hang ? ", moved" : "",
                R.seat.hang ? "hung" : "centered", R.bubble_w, -R.overhang,
                R.bubble_tallest, reach, R.tail_moves, R.frames);
  std::printf("      %s\n", line);
}

// splash.cpp may only branch on what these builds vary.
void check_branches() {
  const std::vector<std::string> ls = lines_of(slurp(
      std::string(FW_DIR) + "/projects/canary-display/src/ui/splash.cpp"));
  const char* const kKnown[] = {
      "#if defined(CD_FLAVOR_NIGHTSTAND) && !defined(CD_FLAVOR_WATCH)",
      "#ifdef CD_FLAVOR_WATCH", "#if !defined(CD_FLAVOR_NIGHTSTAND)",
      "#if LVGL_VERSION_MAJOR >= 9"};
  for (size_t i = 0; i < ls.size(); ++i) {
    const std::string t = trim(ls[i]);
    if (!starts_with(t, "#if") && !starts_with(t, "#elif")) continue;
    bool known = false;
    for (size_t k = 0; k < sizeof(kKnown) / sizeof(kKnown[0]); ++k)
      known = known || t == kKnown[k];
    CHECK(known, "splash.cpp branches on \"%s\", which this test's three "
                 "builds do not vary", t.c_str());
  }
}

}  // namespace

int main() {
  load_ladders();
  load_minted_core();
  load_hex_len();
  CHECK(g_hex_len == (int)device_pseudonym::HEX_LEN,
        "the fake pseudonym is %d letters; device_pseudonym.h's HEX_LEN is %d",
        (int)device_pseudonym::HEX_LEN, g_hex_len);
  check_branches();
  const std::vector<Env> envs = load_envs();
  std::printf("the splash, as splash.cpp draws it (%s build):\n",
              kBuildName[kBuild]);
  // The hop at the temperament's ceiling (canary_mark clamps to 1.25).
  canary_mark_temperament(1.25f, 1.0f, 1.25f);
  const std::vector<Canvas> cs = canvases(envs);
  int mine = 0, moved = 0;
  for (size_t i = 0; i < cs.size(); ++i) {
    const Canvas& cv = cs[i];
    const int build = cv.wide ? 2 : cv.round ? 0 : 1;
    if (build != kBuild) continue;
    mine++;
    fake_lvgl::disp_w() = cv.w;
    fake_lvgl::disp_h() = cv.h;
    g_fam_drawn = sized(kFam, cv.w);
    for (int which = 0; which < 2; ++which) {
      g_fam = cv.fam;
      g_which = which;
      const Face* f = face_of(g_ladder[cv.fam][which][kLabel]);
      if (f == nullptr) continue;  // font_label() has failed it
      std::printf("  %s %dx%d%s/%s:\n", cv.what.c_str(), cv.w, cv.h,
                  cv.turned ? " (turned)" : "", which ? "heirloom" : "default");
      const std::string subjects[2] = {std::string((size_t)g_hex_len, '2'),
                                       worst_subject(*f)};
      for (int s = 0; s < 2; ++s) {
        if (s == 1 && subjects[1] == subjects[0]) continue;
        fake_pseudonym::id() = subjects[s];
        fake_prefs::uchars().clear();
        play(cv, which, true, subjects[s]);
        play(cv, which, false, subjects[s]);
      }
      moved += sl::seat(cv.h, kFam, true).hang ? 1 : 0;
    }
  }
  // The dash, dash7 and nightstand7 share one 800x480 canvas (and its
  // turn); canvases() keeps one of each.
  CHECK(mine >= (kBuild == 0 ? 1 : kBuild == 1 ? 6 : 2),
        "only %d canvases for this build", mine);
  std::printf("  %d canvases, %d of them (x ladder) with a moved first-meeting "
              "seat\n", mine, moved);
  if (g_fail == 0) {
    std::printf("ALL SPLASH SCENES TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
