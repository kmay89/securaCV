// tests_host/splash_test_env.h — what the boot splash's two host tests share
// (F88): the speech bubble's measure, the pseudonym's length, and the
// canvases the splash runs on.
//
// test_splash_layout.cpp holds splash_layout.h's seats on every canvas;
// test_splash_scenes.cpp compiles the real splash.cpp against
// fake_lvgl/lvgl.h and holds where it puts the bird and the bubble on the
// same canvases. The bubble's measure is LVGL 8.4's label wrap
// (lv_txt_wrap.h), held to a native harness by test_splash_layout.
#pragma once
#include "canary/ui/splash_layout.h"
#include "lv_txt_wrap.h"
#include "onboard_test_env.h"
#include "story/story_scripts.h"

namespace sl = canary::ui::splashlayout;

// The wrapped text's height (lv_txt_get_size, line_space 0).
inline int text_h(const char* txt, const Face& f, int max_w) {
  GlyphW g = {&f};
  return lvwrap::line_count(txt, g, max_w) * f.line_height;
}

// The speech bubble's height around a line (splash.cpp's frame).
inline int bubble_h(const char* txt, const Face& f, const sl::Family& fam) {
  return text_h(txt, f, fam.bubble_w - sl::kBubbleTextInset) + 2 * sl::kBubblePad +
         2 * sl::kBubbleBorder;
}

// The family as splash.cpp draws it on a canvas `canvas_w` px wide: its
// bubble no wider than the canvas less splash_layout.h's margin (F160).
inline sl::Family sized(const sl::Family& f, int canvas_w) {
  sl::Family s = f;
  s.bubble_w = sl::bubble_w(canvas_w, f);
  return s;
}

// ── the wordmark (F373) ──────────────────────────────────────────────────
// The text splash.cpp sets the wordmark to.
const char* const kWordmark = "SecuraCV";

// LVGL's measure of the wordmark in face `f` at `spacing` px of letter
// spacing (lv_txt_get_width: the spacing after each glyph that has a width,
// the last one's taken off), from LVGL's own font data. The LVGL 8.4
// numbers test_splash_layout pins it to are the ones the native renders
// read off lv_txt_get_size (and LVGL 9.5's lv_text_get_size gives too).
inline int word_px(const Face& f, int spacing) {
  int glyphs = 0;
  for (const char* p = kWordmark; *p; ++p)
    if (glyph_px(f, (unsigned char)*p, (unsigned char)p[1]) > 0) glyphs++;
  const int w = text_px(f, kWordmark);
  return w > 0 ? w + (glyphs - 1) * spacing : 0;
}

// The wordmark as splash.cpp's ladder fits it on a canvas `canvas_w` px
// wide, the Character's title face `title` px (F373): that face at
// kWordSpacing where word_fits() holds it; else Montserrat 28 where it is the
// narrower, at kWordSpacing; else that face at 0; else not shown.
struct WordSet {
  int face;      // the face's px size (lv_font_montserrat_<face>)
  int spacing;   // its letter spacing
  int w;         // its laid-out width
  bool shown;    // false: no rung fits, and the wordmark is not shown
};

inline WordSet word_set(int canvas_w, int title) {
  const Face* t = face_of(title);
  const Face* f28 = face_of(28);
  WordSet s = {title, sl::kWordSpacing, t ? word_px(*t, sl::kWordSpacing) : 0, true};
  if (t == nullptr || f28 == nullptr) {
    s.shown = false;  // a face the metrics do not carry: the caller fails it
    return s;
  }
  if (sl::word_fits(canvas_w, s.w)) return s;
  const int w28 = word_px(*f28, sl::kWordSpacing);
  if (w28 < s.w) {
    s.face = 28;
    s.w = w28;
  }
  if (sl::word_fits(canvas_w, s.w)) return s;
  s.spacing = 0;
  s.w = word_px(*face_of(s.face), 0);
  s.shown = sl::word_fits(canvas_w, s.w);
  return s;
}

// The pseudonym's length (device_pseudonym.h: HEX_LEN = TOKEN_BYTES * 2;
// read, since its host branch wants OpenSSL).
static int g_hex_len = 0;

inline void load_hex_len() {
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
inline int tallest_for(const Face& f, const sl::Family& fam, const std::string& subject,
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
inline int tallest_bubble(const Face& f, const sl::Family& fam, std::string* which) {
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

// ── the canvases the splash runs on ──────────────────────────────────────
struct Canvas {
  std::string what;
  int w, h;
  bool wide;      // splash.cpp's wide branch (the dash line)
  int fam;        // character.cpp's ladder family
  bool turned;    // the panel a quarter turned (a saved rotation)
  bool round;     // the round watch (its config's own build of splash.cpp)
};

// main.cpp's saved rotations before the splash (onboard_test_env.h).
inline void check_boot_rotation(bool* nightlight, bool* dash) {
  check_boot_rotation("canary::ui::splash_play(", nightlight, dash);
}

inline std::vector<Canvas> canvases(const std::vector<Env>& envs) {
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
      c.round = e.watch && !e.nightstand;
      bool seen = false;
      for (size_t k = 0; k < out.size(); ++k)
        seen = seen || (out[k].w == c.w && out[k].h == c.h &&
                        out[k].wide == c.wide && out[k].fam == c.fam &&
                        out[k].round == c.round);
      if (!seen) out.push_back(c);
    }
  }
  return out;
}

