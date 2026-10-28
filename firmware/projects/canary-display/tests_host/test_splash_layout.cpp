// Host test: the boot splash's seats on every canvas it runs on (F88).
//
// splash.cpp seated the first meeting's bird at CENTER -70, a seat drawn for
// the round watch's 240 px disc. The nightlight boots into its saved
// rotation, and in landscape that is a 320x180 canvas: the bird's top sat at
// y -12, its head off the glass, and its intro hops lifted it to y -25
// (native LVGL 8.4). splash_layout.h now names every seat the splash takes,
// and this test holds them, on every canvas:
//  * the canvases are each display env's panel (the ini -> its board's
//    pins.h), and the panel turned a quarter for the two flavors whose
//    saved rotation main.cpp applies before splash_play (the nightlight's,
//    and the 7"/dash glass's) — read off main.cpp, not typed;
//  * on each, both splashes (the first meeting and hello again) keep the
//    bird's whole box on the canvas over its breath and its highest hop
//    (sl::kHopReach, which this test derives from canary_mark.cpp's hop and
//    LVGL's overshoot curve);
//  * the seat is the one the splash always had wherever that holds the
//    hop: only the canvases that needed it drop (pinned: the landscape
//    nightlight's first meeting), and a first meeting's bird otherwise
//    moves only up, clear of its bubble (F159, below);
//  * where the bird moved, the speech bubble hangs from under it: its
//    tallest form (the script's longest line, wrapped the way LVGL wraps a
//    label, in each type ladder's label face) stays on the canvas and clear
//    of the bird's breath. The wrap model is held to the bubble heights the
//    real LVGL 8.4 drew in a native harness;
//  * the bubble is bubble_w() wide on each canvas (F160): the family's
//    width wherever the canvas less its margin holds it, else the canvas
//    less its margin — so its sides stay on the glass — and its tallest
//    form is measured at that width (pinned: only the 172 and 180 px
//    portrait glass narrow it, and only the 172 px glass under Heirloom
//    wraps a line more);
//  * where the bubble stays centered, the first meeting's bird sits clear
//    of its tallest form and the tail on it (F159): its box, over its
//    breath, ends sl::kGap above the tail's reach (sl::kTailRise, which
//    this test derives from the tail's side, inset and pivot), wherever the
//    canvas holds that seat's hop; it rises just enough, and not at all
//    where its family's seat already clears. Held for every tallest bubble
//    a canvas could measure (seat() takes splash.cpp's measure, so a
//    pseudonym of mixed letters is covered too), and for the bubbles the
//    script's lines wrap to with each one-letter pseudonym. Pinned: the
//    seats that rise (the AMOLED, the 172 and 180 px portrait glass, the
//    touch169, the dash under Heirloom with a wide-lettered pseudonym), and
//    the only ones not clear: the round watch, whose raised seat would lift
//    its hop off the disc (its bubble's top stays in the bird's box by 2 px,
//    6 under Heirloom, the tail 10 px above that), and the landscape
//    nightlight, whose hung bubble keeps kGap under the bird and stands its
//    tail in the foot of the bird's box.
//
// This test holds the header. test_splash_scenes.cpp holds splash.cpp to it:
// it compiles the real splash.cpp against fake_lvgl and reads where the bird
// and the bubble land on the same canvases (the wrap model below is shared
// with that fake through lv_txt_wrap.h, so these native pins hold both).
//
// Reads the tree through FW_DIR. The test is C++17 (story_scripts.h's inline
// variables); the Makefile holds the header alone to gnu++11 first, since
// the core-2.0.17 sketch compiles it there. Prints "ALL SPLASH LAYOUT TESTS
// PASSED" on success.

#include <cmath>

#include "splash_test_env.h"

namespace {

// ── canary_mark's hop: how high it lifts the bird ────────────────────────
// lv_bezier3 (lv_math.c) and lv_anim_path_overshoot's control points
// (lv_anim.c: 0, 1000, 1300, LV_BEZIER_VAL_MAX = 1024) — LVGL's numbers.
uint32_t bezier3(uint32_t t, uint32_t u0, uint32_t u1, uint32_t u2,
                 uint32_t u3) {
  const uint32_t r = 1024 - t;
  const uint32_t r2 = (r * r) >> 10, r3 = (r2 * r) >> 10;
  const uint32_t t2 = (t * t) >> 10, t3 = (t2 * t) >> 10;
  return ((r3 * u0) >> 10) + ((3 * r2 * t * u1) >> 20) +
         ((3 * r * t2 * u2) >> 20) + ((t3 * u3) >> 10);
}

// A float literal after `key` in `text` (canary_mark.cpp's numbers).
double number_after(const std::string& text, const char* key) {
  const size_t at = text.find(key);
  CHECK(at != std::string::npos, "canary_mark.cpp: \"%s\" not found", key);
  return at == std::string::npos ? 0 : std::atof(text.c_str() + at +
                                                  std::strlen(key));
}

// The tail's reach above the bubble's top edge: its box, kTailSide square,
// kTailInset px into the edge, turned 45 degrees about kTailPivot (the
// square's center); the tip stands half a diagonal above the pivot.
int derived_tail_rise() {
  const double pivot_above = sl::kTailSide - sl::kTailInset - sl::kTailPivot;
  const double tip = pivot_above + sl::kTailSide * std::sqrt(2.0) / 2.0;
  const int rise = (int)std::ceil(tip);
  std::printf("the tail: a %d px square %d px into the bubble, turned about (%d, %d): its tip "
              "%.2f px above the bubble's top edge, %d rows; kTailRise %d\n",
              sl::kTailSide, sl::kTailInset, sl::kTailPivot, sl::kTailPivot, tip, rise,
              sl::kTailRise);
  CHECK(sl::kTailPivot * 2 == sl::kTailSide,
        "kTailPivot is %d; the tail turns about the center of its %d px square (F185)",
        sl::kTailPivot, sl::kTailSide);
  return rise;
}

int derived_hop_reach() {
  std::printf("the hop (canary_mark.cpp, LVGL's overshoot curve):\n");
  const std::string mark = slurp(std::string(FW_DIR) +
                                 "/projects/canary-display/src/ui/canary_mark.cpp");
  const double base = number_after(mark, "int apex = (int)(");
  const double ceil = number_after(mark, "if (hop > ");
  const double cap = number_after(mark, "if (apex > ");
  CHECK(mark.find("lv_anim_set_path_cb(&s_bob, lv_anim_path_overshoot);") !=
            std::string::npos,
        "canary_mark.cpp's hop no longer rides lv_anim_path_overshoot");
  int apex = (int)(base * ceil);
  if (apex > (int)cap) apex = (int)cap;
  int reach = 0;
  for (uint32_t t = 0; t <= 1024; ++t) {
    const int v = (int)((bezier3(t, 0, 1000, 1300, 1024) * (uint32_t)apex) >> 10);
    if (v > reach) reach = v;
  }
  std::printf("  apex %.0f px x temperament ceiling %.2f = %d px, %d at the "
              "overshoot's swing; kHopReach %d\n", base, ceil, apex, reach,
              sl::kHopReach);
  return reach;
}

// The wrap model against what the real LVGL 8.4 drew: a native harness
// built splash.cpp's speech bubble (its width, padding, border, label width
// and long mode, the Character's label face) with the display's lv_conf
// and set every line of the first meeting in it, three pseudonyms each —
// "2222222222222222", "WWWWWWWWWWWWWWWW" (wide letters: the token breaks
// inside itself) and "iiiiiiiiiiiiiiii" — and these are the tallest
// bubbles it measured, per ladder family and type: at the families' widths,
// and at the widths bubble_w() gives the 172 and 180 px portrait glass
// (164 and 172 px, F160).
const sl::Family k172 = sized(sl::kSmallGlass, 172);
const sl::Family k180 = sized(sl::kSmallGlass, 180);

void check_wrap_model() {
  std::printf("the bubble's wrap (LVGL 8.4's, against the native harness):\n");
  struct Pin {
    const char* what;
    int fam;
    const sl::Family* glass;
    int which;
    int drawn[3];  // px, measured: subjects '2', 'W', 'i'
  };
  const Pin pins[] = {
      {"small glass", kStd, &sl::kSmallGlass, 0, {70, 86, 70}},
      {"small glass", kStd, &sl::kSmallGlass, 1, {94, 94, 94}},
      {"lean small glass", kLean, &sl::kSmallGlass, 0, {70, 86, 70}},
      {"lean small glass", kLean, &sl::kSmallGlass, 1, {94, 94, 94}},
      {"the AMOLED", kBig, &sl::kSmallGlass, 0, {110, 132, 110}},
      {"the AMOLED", kBig, &sl::kSmallGlass, 1, {157, 184, 157}},
      {"wide glass", kBig, &sl::kWideGlass, 0, {66, 66, 66}},
      {"wide glass", kBig, &sl::kWideGlass, 1, {76, 103, 76}},
      {"172 px glass", kStd, &k172, 0, {86, 86, 86}},
      {"172 px glass", kStd, &k172, 1, {94, 112, 94}},
      {"180 px glass", kStd, &k180, 0, {86, 86, 86}},
      {"180 px glass", kStd, &k180, 1, {94, 94, 94}},
  };
  const char kSubject[3] = {'2', 'W', 'i'};
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    const Pin& p = pins[i];
    const Face* f = face_of(g_ladder[p.fam][p.which][kLabel]);
    CHECK(f != nullptr, "%s: no label face carried", p.what);
    if (!f) continue;
    char got[64] = "";
    for (int k = 0; k < 3; ++k) {
      const std::string subject((size_t)g_hex_len, kSubject[k]);
      const int h = tallest_for(*f, *p.glass, subject, nullptr);
      CHECK(h == p.drawn[k], "%s/%s, pseudonym \"%s\": the model's tallest "
            "bubble is %d px; LVGL drew %d", p.what,
            p.which ? "heirloom" : "default", subject.c_str(), h, p.drawn[k]);
      std::snprintf(got + std::strlen(got), sizeof(got) - std::strlen(got),
                    "%s%d", k ? " / " : "", h);
    }
    std::string line;
    const int worst = tallest_bubble(*f, *p.glass, &line);
    std::printf("  %-17s %-8s montserrat_%d on %d px: %s px; any pseudonym "
                "%d (\"%s\")\n", p.what, p.which ? "heirloom" : "default",
                f->size, p.glass->bubble_w - sl::kBubbleTextInset, got, worst,
                line.c_str());
  }
}

// seat()'s rule on a canvas whose family's first-meeting seat holds its hop
// (the bubble centered), for every tallest bubble the canvas could measure:
// the bird stays on the canvas over its hop and breath; it is clear of the
// tail on that bubble wherever a seat that clears holds the hop; it rises
// only when its family's seat does not clear, and then just enough; and
// where it cannot clear it keeps its family's seat.
void check_seat_rule(const Canvas& c, const sl::Family& fam, const char* who, int hop_reach) {
  const int nominal = sl::bird_top(c.h, fam.bird, fam.intro_off);
  if (nominal - hop_reach < 0) return;  // the hung bubble (below)
  int fails = 0;
  for (int t = 1; t <= c.h; ++t) {
    const sl::Seat s = sl::seat(c.h, fam, true, t);
    const int top = sl::bird_top(c.h, fam.bird, s.bird_off);
    const int b_top = sl::centered_top(c.h, fam, t);
    const int need = top + fam.bird + sl::kBreath + sl::kGap + sl::kTailRise;  // <= b_top
    const int deepest = b_top - (fam.bird + sl::kBreath + sl::kGap + sl::kTailRise);
    bool ok = !s.hang && s.bubble_top == b_top && top - hop_reach >= 0;
    if (deepest >= hop_reach) {
      ok = ok && s.clear && need <= b_top &&
           (nominal <= deepest ? (s.bird_off == fam.intro_off && !s.raised)
                               : (top == deepest && s.raised));
    } else {
      ok = ok && !s.clear && !s.raised && s.bird_off == fam.intro_off;
    }
    if (!ok && fails++ == 0)
      CHECK(false, "%s: for a %d px tallest bubble (top y %d) seat() puts the bird at y %d "
            "(%s, %s); its family's seat is y %d, the deepest clear one y %d, the hop "
            "needs %d", who, t, b_top, top, s.raised ? "raised" : "not raised",
            s.clear ? "clear" : "not clear", nominal, deepest, hop_reach);
  }
}

void check_canvas(const Canvas& c, int which, int hop_reach,
                  std::vector<std::string>* moved, std::vector<std::string>* raised,
                  std::vector<std::string>* unclear) {
  const sl::Family& fam = c.wide ? sl::kWideGlass : sl::kSmallGlass;
  const char* lad = which ? "heirloom" : "default";
  char who[96];
  std::snprintf(who, sizeof(who), "%s %dx%d/%s", c.what.c_str(), c.w, c.h,
                lad);
  const Face* f = face_of(g_ladder[c.fam][which][kLabel]);
  CHECK(f != nullptr, "%s: no label face carried", who);
  if (!f) return;
  // F160: the bubble as this canvas draws it — no wider than the canvas
  // less its margin a side, the family's width wherever that holds it.
  const sl::Family drawn = sized(fam, c.w);
  CHECK(drawn.bubble_w <= c.w - 2 * sl::kBubbleMargin &&
            (drawn.bubble_w == fam.bubble_w ||
             drawn.bubble_w == c.w - 2 * sl::kBubbleMargin) &&
            drawn.bubble_w - sl::kBubbleTextInset > 0,
        "%s: the bubble is %d px wide on the %d px canvas (the family's %d, "
        "the margin %d a side)", who, drawn.bubble_w, c.w, fam.bubble_w,
        sl::kBubbleMargin);
  const int tallest = tallest_bubble(*f, drawn, nullptr);
  const int tallest_family = tallest_bubble(*f, fam, nullptr);
  check_seat_rule(c, fam, who, hop_reach);
  // The script's own bubbles: each one-letter pseudonym's tallest, as
  // splash.cpp would measure it on this canvas.
  for (size_t k = 0; k < g_minted.alpha.size(); ++k) {
    const std::string subject((size_t)g_hex_len, g_minted.alpha[k]);
    const int t = tallest_for(*f, drawn, subject, nullptr);
    const sl::Seat s = sl::seat(c.h, fam, true, t);
    const int top = sl::bird_top(c.h, fam.bird, s.bird_off);
    if (!s.hang)
      CHECK(sl::centered_top(c.h, fam, t) - sl::kTailRise >= 0 &&
                sl::centered_top(c.h, fam, t) + t <= c.h,
            "%s, pseudonym \"%s\": the centered bubble (%d px tall) or its tail "
            "leaves the canvas", who, subject.c_str(), t);
    CHECK(s.clear == (s.hang ? false : top + fam.bird + sl::kBreath + sl::kGap +
                                           sl::kTailRise <= sl::centered_top(c.h, fam, t)),
          "%s, pseudonym \"%s\": seat() says the bird is %s the tail on its %d px "
          "bubble", who, subject.c_str(), s.clear ? "clear of" : "not clear of", t);
  }
  std::string summary;
  for (int fm = 1; fm >= 0; --fm) {
    const bool first = fm == 1;
    const sl::Seat s = sl::seat(c.h, fam, first, first ? tallest : 0);
    const int nominal_off = first ? fam.intro_off : fam.hello_off;
    const int nominal = sl::bird_top(c.h, fam.bird, nominal_off);
    const int top = sl::bird_top(c.h, fam.bird, s.bird_off);
    const int x = c.w / 2 - fam.bird / 2;
    const char* what = first ? "first meeting" : "hello again";
    // The whole bird on the canvas: its highest hop above, its breath below.
    CHECK(top - hop_reach >= 0 && top + fam.bird + sl::kBreath <= c.h && x >= 0 &&
              x + fam.bird <= c.w,
          "%s: the %s bird (%d px at y %d) leaves the canvas: y %d..%d over "
          "its hop and breath", who, what, fam.bird, top, top - hop_reach,
          top + fam.bird + sl::kBreath);
    // The seat it always had, wherever that holds the hop (risen clear of
    // the bubble's tallest form, F159, checked by check_seat_rule); moved
    // down just enough where it does not.
    if (nominal - hop_reach >= 0) {
      CHECK((s.bird_off == nominal_off || (first && s.raised)) && !s.hang,
            "%s: the %s seat moved from %d to %d though the canvas holds it",
            who, what, nominal_off, s.bird_off);
      if (s.raised) raised->push_back(std::string(who) + " " + what);
    } else {
      CHECK(top == hop_reach && s.hang == first,
            "%s: the %s seat is y %d; the hop needs %d (and only the first "
            "meeting's bubble hangs)", who, what, top, hop_reach);
      moved->push_back(std::string(who) + " " + what);
    }
    char one[260];
    if (!first) {
      std::snprintf(one, sizeof(one), "  hello again y %3d..%3d%s", top,
                    top + fam.bird, s.bird_off != nominal_off ? " (moved)" : "");
      summary += one;
      continue;
    }
    // The bubble: hung under a moved bird, its tallest form on the canvas
    // and clear of the bird's breath; centered otherwise (as it was).
    const int bird_bottom = top + fam.bird + sl::kBreath;
    int b_top, b_bottom;
    if (s.hang) {
      b_top = s.bubble_top;
      b_bottom = b_top + tallest;
      CHECK(b_top >= bird_bottom + sl::kGap && b_bottom <= c.h,
            "%s: the hung bubble (y %d..%d, its tallest) leaves the canvas or "
            "meets the bird (to y %d)", who, b_top, b_bottom, bird_bottom);
    } else {
      // LV_ALIGN_CENTER of a content-sized object.
      b_top = c.h / 2 - tallest / 2 + fam.bubble_off;
      b_bottom = b_top + tallest;
      CHECK(b_top >= 0 && b_bottom <= c.h,
            "%s: the centered bubble (y %d..%d, its tallest) leaves the "
            "canvas", who, b_top, b_bottom);
    }
    // Clear of the tail on the tallest bubble wherever seat() says so
    // (check_seat_rule holds when that is); printed, and pinned in main(),
    // where it is not.
    const int overlap = bird_bottom - b_top;
    const int tail_overlap = bird_bottom + sl::kGap - (b_top - sl::kTailRise);
    if (s.clear)
      CHECK(tail_overlap <= 0, "%s: the bird (to y %d over its breath) is within %d px of "
            "the tail on its tallest bubble (from y %d)", who, bird_bottom, sl::kGap,
            b_top - sl::kTailRise);
    else
      unclear->push_back(who);
    char tail[96] = "";
    if (overlap > 0)
      std::snprintf(tail, sizeof(tail), " (in the bird's box by %d px, its tail %d)",
                    overlap, overlap + sl::kTailRise);
    else if (!s.clear)
      std::snprintf(tail, sizeof(tail), " (its tail in the bird's box by %d px)",
                    bird_bottom - (b_top - sl::kTailRise));
    else
      std::snprintf(tail, sizeof(tail), " (its tail %d px under the bird's breath)",
                    (b_top - sl::kTailRise) - bird_bottom);
    char narrowed[64] = "";
    if (drawn.bubble_w != fam.bubble_w)
      std::snprintf(narrowed, sizeof(narrowed), " %d px wide (tallest %d at "
                    "%d)", drawn.bubble_w, tallest_family, fam.bubble_w);
    std::snprintf(one, sizeof(one),
                  "first meeting y %3d..%3d (hop to %3d)%s, bubble %s%s y "
                  "%3d..%3d%s;", top, top + fam.bird, top - hop_reach,
                  s.raised ? " (raised)" : s.bird_off != nominal_off ? " (moved)" : "",
                  s.hang ? "hung" : "centered", narrowed, b_top, b_bottom,
                  tail);
    summary = one + summary;
  }
  std::printf("  %-26s %s\n", who, summary.c_str());
}

}  // namespace

int main() {
  load_ladders();
  load_minted_core();
  load_hex_len();
  const std::vector<Env> envs = load_envs();
  const int rise = derived_tail_rise();
  CHECK(rise == sl::kTailRise, "splash_layout.h's kTailRise is %d; the turned tail reaches "
        "%d rows above the bubble", sl::kTailRise, rise);
  const int reach = derived_hop_reach();
  CHECK(reach == sl::kHopReach, "splash_layout.h's kHopReach is %d; canary_mark's "
        "hop reaches %d", sl::kHopReach, reach);
  check_wrap_model();
  std::printf("the splash on every canvas it runs on:\n");
  const std::vector<Canvas> cs = canvases(envs);
  std::vector<std::string> moved, raised, unclear;
  for (size_t i = 0; i < cs.size(); ++i)
    for (int which = 0; which < 2; ++which)
      check_canvas(cs[i], which, sl::kHopReach, &moved, &raised, &unclear);
  // Pinned: the only seats that moved are the landscape nightlight's first
  // meeting (both ladders). Anything else moving is a change to a canvas
  // whose splash was right.
  bool only_nightlight = moved.size() == 2;
  for (size_t i = 0; i < moved.size(); ++i)
    only_nightlight = only_nightlight &&
                      moved[i].find("nightlight-c3 320x180/") == 0 &&
                      moved[i].find("first meeting") != std::string::npos;
  CHECK(only_nightlight, "%d seats moved; only the 320x180 nightlight's first "
        "meeting should (first: %s)", (int)moved.size(),
        moved.empty() ? "none" : moved[0].c_str());
  CHECK(cs.size() >= 8, "only %d canvases", (int)cs.size());
  // Pinned (F159): with each canvas's tallest bubble (any pseudonym), the
  // first meetings whose bird rises clear of it, and the only ones it
  // cannot clear. Anything else rising is a canvas whose seat was clear.
  const auto joined = [](const std::vector<std::string>& v, const char* cut) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
      std::string one = v[i];
      const size_t at = one.find(cut);
      if (at != std::string::npos) one.resize(at);
      out += (out.empty() ? "" : ", ") + one;
    }
    return out;
  };
  const std::string rose = joined(raised, " first meeting");
  const std::string not_clear = joined(unclear, "\x01");
  std::printf("the first meetings whose bird rose clear of its bubble (F159): %s\n"
              "  and the only ones not clear of its tail: %s\n", rose.c_str(),
              not_clear.c_str());
  CHECK(rose == "dash 800x480/heirloom, dash 480x800/heirloom, nightstand-s3 172x320/default, "
                "nightstand-s3 172x320/heirloom, touch169 240x280/default, "
                "touch169 240x280/heirloom, amoled241 450x600/default, "
                "amoled241 450x600/heirloom, nightstand-c6 172x320/default, "
                "nightstand-c6 172x320/heirloom, nightlight-c3 180x320/default, "
                "nightlight-c3 180x320/heirloom",
        "the birds that rise clear of the bubble are \"%s\"", rose.c_str());
  CHECK(not_clear == "watch 240x240/default, watch 240x240/heirloom, "
                     "nightlight-c3 320x180/default, nightlight-c3 320x180/heirloom",
        "the first meetings not clear of the tail are \"%s\"; only the round watch's "
        "and the landscape nightlight's should be", not_clear.c_str());
  // Pinned (F160): the bubble narrows on the 172 and 180 px portrait glass
  // alone (164 and 172 px), and only the 172 px glass under Heirloom wraps
  // its tallest line once more (94 px to 112, with a wide-lettered
  // pseudonym). Anything else moving is a canvas whose bubble was right.
  std::string narrowed, grew;
  for (size_t i = 0; i < cs.size(); ++i) {
    const sl::Family& fam = cs[i].wide ? sl::kWideGlass : sl::kSmallGlass;
    const sl::Family drawn = sized(fam, cs[i].w);
    char one[96];
    if (drawn.bubble_w != fam.bubble_w) {
      std::snprintf(one, sizeof(one), "%s%dx%d:%d", narrowed.empty() ? "" : " ",
                    cs[i].w, cs[i].h, drawn.bubble_w);
      if (narrowed.find(one + (narrowed.empty() ? 0 : 1)) == std::string::npos)
        narrowed += one;
    }
    for (int which = 0; which < 2; ++which) {
      const Face* f = face_of(g_ladder[cs[i].fam][which][kLabel]);
      if (f == nullptr) continue;
      const int was = tallest_bubble(*f, fam, nullptr);
      const int now = tallest_bubble(*f, drawn, nullptr);
      if (was == now) continue;
      std::snprintf(one, sizeof(one), "%s%dx%d/%s:%d->%d",
                    grew.empty() ? "" : " ", cs[i].w, cs[i].h,
                    which ? "heirloom" : "default", was, now);
      if (grew.find(one + (grew.empty() ? 0 : 1)) == std::string::npos)
        grew += one;
    }
  }
  std::printf("the bubble narrowed (F160): %s; its tallest grew: %s\n",
              narrowed.c_str(), grew.c_str());
  CHECK(narrowed == "172x320:164 180x320:172",
        "the bubble narrows on \"%s\"; only the 172 and 180 px portrait "
        "glass should (164 and 172 px)", narrowed.c_str());
  CHECK(grew == "172x320/heirloom:94->112",
        "the narrowed bubble's tallest grew on \"%s\"; only the 172 px "
        "glass under Heirloom should (94 to 112 px)", grew.c_str());
  if (g_fail == 0) {
    std::printf("ALL SPLASH LAYOUT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
