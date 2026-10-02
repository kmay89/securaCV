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
//  * the seat is the one the splash always had wherever that holds: only
//    the canvases that needed it move (pinned: the landscape nightlight's
//    first meeting);
//  * where the bird moved, the speech bubble hangs from under it: its
//    tallest form (the script's longest line, wrapped the way LVGL wraps a
//    label, in each type ladder's label face) stays on the canvas and clear
//    of the bird's breath. The wrap model is held to the bubble heights the
//    real LVGL 8.4 drew in a native harness.
// The bird/bubble overlap on the canvases whose seats did not move is
// printed, not held: on the AMOLED the centered bubble's tallest form
// reaches into the bird's box (filed).
//
// Reads the tree through FW_DIR; C++11 like test_onboard_layout.cpp (the
// header is compiled by the core-2.0.17 sketch at gnu++11). Prints "ALL
// SPLASH LAYOUT TESTS PASSED" on success.

#include "canary/ui/splash_layout.h"
#include "onboard_test_env.h"
#include "story/story_scripts.h"

namespace sl = canary::ui::splashlayout;

namespace {

// ── LVGL 8.4's label wrap (lv_txt.c: lv_txt_get_next_word,
// _lv_txt_get_next_line, lv_txt_get_size) at letter_space 0, no recolor,
// LV_TXT_LINE_BREAK_LONG_LEN 0 and LV_TXT_BREAK_CHARS " ,.;:-_" (the
// display's lv_conf leaves both at LVGL's defaults). ASCII only: the
// script's lines are.
bool is_break(uint32_t c) {
  const char* b = " ,.;:-_";
  for (int i = 0; b[i]; ++i)
    if (c == (uint32_t)b[i]) return true;
  return false;
}

uint32_t next_word(const char* txt, const Face& f, int max_w, int* word_w,
                   bool force) {
  if (txt[0] == '\0') return 0;
  const uint32_t kNone = 0xFFFFFFFFu;
  uint32_t i = 0, word_len = 0, break_index = kNone;
  int cur_w = 0;
  uint32_t letter = (unsigned char)txt[0];
  uint32_t letter_next = 0;
  while (txt[i] != '\0') {
    letter_next = (unsigned char)txt[i + 1];
    word_len++;
    const int lw = glyph_px(f, letter, letter_next);
    cur_w += lw;
    if (break_index == kNone && cur_w > max_w) break_index = i;
    if (letter == '\n' || letter == '\r' || is_break(letter)) {
      if (i == 0 && break_index == kNone) *word_w = cur_w;
      word_len--;
      break;
    }
    if (break_index == kNone) *word_w = cur_w;
    i++;
    letter = letter_next;
  }
  if (break_index == kNone) {
    if (word_len == 0) i = i + 1;
    return i;
  }
  if (force) return break_index;
  *word_w = 0;
  return 0;
}

uint32_t next_line(const char* txt, const Face& f, int max_w) {
  uint32_t i = 0;
  while (txt[i] != '\0' && max_w > 0) {
    int word_w = 0;
    const uint32_t adv = next_word(txt + i, f, max_w, &word_w, i == 0);
    max_w -= word_w;
    if (adv == 0) break;
    i += adv;
    if (txt[i] == '\n' || txt[i] == '\r') {
      i++;
      break;
    }
  }
  if (i == 0) i = 1;
  return i;
}

// The wrapped text's height (lv_txt_get_size, line_space 0).
int text_h(const char* txt, const Face& f, int max_w) {
  int lines = 0;
  for (uint32_t at = 0; txt[at] != '\0';) {
    at += next_line(txt + at, f, max_w);
    lines++;
  }
  return (lines == 0 ? 1 : lines) * f.line_height;
}

// The speech bubble's height around a line (splash.cpp's frame).
int bubble_h(const char* txt, const Face& f, const sl::Family& fam) {
  return text_h(txt, f, fam.bubble_w - sl::kBubbleTextInset) + 2 * sl::kBubblePad +
         2 * sl::kBubbleBorder;
}

// The pseudonym's length (device_pseudonym.h: HEX_LEN = TOKEN_BYTES * 2;
// read, since its host branch wants OpenSSL).
int g_hex_len = 0;

void load_hex_len() {
  const std::string dp = slurp(std::string(FW_DIR) +
                               "/common/identity/device_pseudonym.h");
  const char* key = "constexpr size_t TOKEN_BYTES = ";
  const size_t at = dp.find(key);
  CHECK(at != std::string::npos &&
            dp.find("HEX_LEN     = TOKEN_BYTES * 2;") != std::string::npos,
        "device_pseudonym.h's HEX_LEN is no longer TOKEN_BYTES * 2");
  g_hex_len = at == std::string::npos
                  ? 16
                  : 2 * std::atoi(dp.c_str() + at + std::strlen(key));
}

// The tallest bubble the first meeting shows for one pseudonym: every line
// of story::kHello, `%s` expanded the way the StoryTeller does.
int tallest_for(const Face& f, const sl::Family& fam, const std::string& subject,
                std::string* which) {
  int best = 0;
  canary::story::StoryTeller teller;
  teller.set_subject(subject.c_str());
  for (uint8_t k = 0; k < canary::story::kHello.n; ++k) {
    const canary::story::Beat* b = &canary::story::kHello.beats[k];
    if (b->line == nullptr) continue;
    char buf[192];
    teller.expand(b, buf, sizeof(buf));
    const int h = bubble_h(buf, f, fam);
    if (h > best) {
      best = h;
      if (which) *which = b->line;
    }
  }
  return best;
}

// ...and for any pseudonym the unit can show: HEX_LEN of any one character
// of the minting alphabet it is drawn from (the widest such, or the one
// whose letters break the worst).
int tallest_bubble(const Face& f, const sl::Family& fam, std::string* which) {
  int best = 0;
  for (size_t c = 0; c < g_minted.alpha.size(); ++c) {
    std::string line;
    const int h = tallest_for(f, fam, std::string((size_t)g_hex_len,
                                                  g_minted.alpha[c]), &line);
    if (h > best) {
      best = h;
      if (which) *which = line;
    }
  }
  return best;
}

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

// ── the canvases the splash runs on ──────────────────────────────────────
struct Canvas {
  std::string what;
  int w, h;
  bool wide;      // splash.cpp's wide branch (the dash line)
  int fam;        // character.cpp's ladder family
  bool turned;    // the panel a quarter turned (a saved rotation)
};

// main.cpp applies a saved rotation before splash_play() only under these
// two flavors' guards; anything else turning the glass first is a canvas
// this test does not know.
void check_boot_rotation(bool* nightlight, bool* dash) {
  const std::string main_cpp = slurp(std::string(FW_DIR) +
                                     "/projects/canary-display/src/main.cpp");
  const size_t setup = main_cpp.find("void setup() {");
  const size_t splash = main_cpp.find("canary::ui::splash_play(", setup);
  CHECK(setup != std::string::npos && splash != std::string::npos,
        "main.cpp: setup() or its splash_play() call not found");
  *nightlight = *dash = false;
  if (setup == std::string::npos || splash == std::string::npos) return;
  const std::vector<std::string> ls =
      lines_of(main_cpp.substr(setup, splash - setup));
  std::vector<std::string> guards;
  for (size_t i = 0; i < ls.size(); ++i) {
    const std::string t = trim(ls[i]);
    if (starts_with(t, "#if")) guards.push_back(t);
    else if (starts_with(t, "#endif") && !guards.empty()) guards.pop_back();
    else if (t.find("set_rotation(") != std::string::npos ||
             t.find("set_panel_rotation(") != std::string::npos) {
      bool known = false;
      for (size_t g = 0; g < guards.size(); ++g) {
        if (guards[g] == "#ifdef CD_NIGHTLIGHT") *nightlight = known = true;
        if (guards[g] == "#ifdef CD_FLAVOR_DASH") *dash = known = true;
      }
      CHECK(known, "main.cpp turns the glass before the splash outside the "
                   "nightlight's and the dash's guards: %s", t.c_str());
    }
  }
  CHECK(*nightlight && *dash, "main.cpp no longer applies the nightlight's "
                              "(%d) and the dash's (%d) saved rotation before "
                              "splash_play()", *nightlight, *dash);
}

std::vector<Canvas> canvases(const std::vector<Env>& envs) {
  bool nl_turns = false, dash_turns = false;
  check_boot_rotation(&nl_turns, &dash_turns);
  std::vector<Canvas> out;
  for (size_t i = 0; i < envs.size(); ++i) {
    const Env& e = envs[i];
    const std::string cfg = slurp(std::string(FW_DIR) +
                                  "/configs/canary-display/" + e.cfg +
                                  "/config.h");
    const bool nightlight = define_int(cfg, "CD_NIGHTLIGHT") == 1;
    const int fam = (e.dash || e.amoled) ? kBig : e.lean ? kLean : kStd;
    const bool turns = (nightlight && nl_turns) || (e.dash && dash_turns);
    for (int t = 0; t < (turns ? 2 : 1); ++t) {
      Canvas c;
      c.what = e.name.substr(15);
      c.w = t ? e.h : e.w;
      c.h = t ? e.w : e.h;
      c.wide = e.dash;
      c.fam = fam;
      c.turned = t == 1;
      bool seen = false;
      for (size_t k = 0; k < out.size(); ++k)
        seen = seen || (out[k].w == c.w && out[k].h == c.h &&
                        out[k].wide == c.wide && out[k].fam == c.fam);
      if (!seen) out.push_back(c);
    }
  }
  return out;
}

// splash.cpp seats through this header: the family per branch, the bird at
// seat()'s offset, the bubble hung when seat() says so.
void check_splash_source() {
  std::printf("splash.cpp:\n");
  const std::string sp = slurp(std::string(FW_DIR) +
                               "/projects/canary-display/src/ui/splash.cpp");
  const char* const kNeeds[] = {
      "constexpr splashlayout::Family GLASS = splashlayout::kSmallGlass;",
      "constexpr splashlayout::Family GLASS = splashlayout::kWideGlass;",
      "splashlayout::seat(\n      (int)lv_disp_get_ver_res(NULL), GLASS, "
      "first_meeting);",
      "canary_mark_create(scr, BIRD);",
      "lv_obj_align(bird, LV_ALIGN_CENTER, 0, seat.bird_off);",
      "lv_obj_align(bub, LV_ALIGN_TOP_MID, 0, seat.bubble_top);",
      "lv_obj_align(bub, LV_ALIGN_CENTER, 0, BUB_Y);",
      "lv_obj_set_width(line, BUB_W - splashlayout::kBubbleTextInset);",
      "lv_obj_set_style_pad_all(bub, splashlayout::kBubblePad, 0);",
      "lv_obj_set_style_border_width(bub, splashlayout::kBubbleBorder, 0);",
  };
  for (size_t i = 0; i < sizeof(kNeeds) / sizeof(kNeeds[0]); ++i)
    CHECK(sp.find(kNeeds[i]) != std::string::npos,
          "splash.cpp no longer says: %s", kNeeds[i]);
  // The watch branch is small glass (the nightstand aliases into it).
  const size_t w = sp.find("#ifdef CD_FLAVOR_WATCH\n  // Intro: bird high");
  const size_t small = sp.find("splashlayout::kSmallGlass;");
  const size_t wide = sp.find("splashlayout::kWideGlass;");
  CHECK(w != std::string::npos && w < small && small < wide,
        "splash.cpp: kSmallGlass is no longer the CD_FLAVOR_WATCH branch's");
  CHECK(sp.find("#define CD_FLAVOR_WATCH 1") != std::string::npos,
        "splash.cpp no longer renders the nightstand line as small glass");
}

// The wrap model against what the real LVGL 8.4 drew: a native harness
// built splash.cpp's speech bubble (its width, padding, border, label width
// and long mode, the Character's label face) with the display's lv_conf
// and set every line of the first meeting in it, three pseudonyms each —
// "2222222222222222", "WWWWWWWWWWWWWWWW" (wide letters: the token breaks
// inside itself) and "iiiiiiiiiiiiiiii" — and these are the tallest
// bubbles it measured, per ladder family and type.
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

void check_canvas(const Canvas& c, int which, int hop_reach,
                  std::vector<std::string>* moved) {
  const sl::Family& fam = c.wide ? sl::kWideGlass : sl::kSmallGlass;
  const char* lad = which ? "heirloom" : "default";
  char who[96];
  std::snprintf(who, sizeof(who), "%s %dx%d/%s", c.what.c_str(), c.w, c.h,
                lad);
  const Face* f = face_of(g_ladder[c.fam][which][kLabel]);
  CHECK(f != nullptr, "%s: no label face carried", who);
  if (!f) return;
  const int tallest = tallest_bubble(*f, fam, nullptr);
  std::string summary;
  for (int fm = 1; fm >= 0; --fm) {
    const bool first = fm == 1;
    const sl::Seat s = sl::seat(c.h, fam, first);
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
    // The seat it always had, wherever that holds the hop; moved just
    // enough where it does not.
    if (nominal - hop_reach >= 0) {
      CHECK(s.bird_off == nominal_off && !s.hang,
            "%s: the %s seat moved from %d to %d though the canvas holds it",
            who, what, nominal_off, s.bird_off);
    } else {
      CHECK(top == hop_reach && s.hang == first,
            "%s: the %s seat is y %d; the hop needs %d (and only the first "
            "meeting's bubble hangs)", who, what, top, hop_reach);
      moved->push_back(std::string(who) + " " + what);
    }
    char one[200];
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
    // Printed, not held where the seat did not move (the AMOLED's).
    const int overlap = bird_bottom - b_top;
    char tail[64] = "";
    if (overlap > 0)
      std::snprintf(tail, sizeof(tail), " (in the bird's box by %d px)",
                    overlap);
    std::snprintf(one, sizeof(one),
                  "first meeting y %3d..%3d (hop to %3d)%s, bubble %s y "
                  "%3d..%3d%s;", top, top + fam.bird, top - hop_reach,
                  s.bird_off != nominal_off ? " (moved)" : "",
                  s.hang ? "hung" : "centered", b_top, b_bottom, tail);
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
  check_splash_source();
  const int reach = derived_hop_reach();
  CHECK(reach == sl::kHopReach, "splash_layout.h's kHopReach is %d; canary_mark's "
        "hop reaches %d", sl::kHopReach, reach);
  check_wrap_model();
  std::printf("the splash on every canvas it runs on:\n");
  const std::vector<Canvas> cs = canvases(envs);
  std::vector<std::string> moved;
  for (size_t i = 0; i < cs.size(); ++i)
    for (int which = 0; which < 2; ++which)
      check_canvas(cs[i], which, sl::kHopReach, &moved);
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
  if (g_fail == 0) {
    std::printf("ALL SPLASH LAYOUT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
