// tests_host/onboard_test_env.h — what the onboarding host tests read off
// the tree, shared by test_onboard_layout.cpp (onboard_layout.h's rules) and
// test_onboard_scenes.cpp (onboard_ui.cpp's glass, against fake_lvgl/).
//
// Nothing about the glass is retyped: the panels come from each env's board
// pins.h via envs/platformio/canary-display.ini, the flavor bits from its
// config.h and flags, the font sizes from character.cpp's type ladders, the
// hints and the minting shapes from provision.cpp, and text is measured with
// LVGL's own Montserrat data (montserrat_metrics.h) the way
// lv_font_get_glyph_width reads it. Only LVGL's font data is a table.
//
// One translation unit per test binary includes it, so its globals are
// plain statics. C++11, like test_onboard_layout.cpp.
#pragma once

#include "canary/ui/onboard_layout.h"
#include "montserrat_metrics.h"
#include "network/provision_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef FW_DIR
#define FW_DIR "../../.."  // firmware/, from tests_host (make -C runs here)
#endif

using namespace canary::ui;
using namespace canary::ui::onboardlayout;

static int g_fail = 0;

#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("  FAIL (%s:%d): ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
      g_fail++;                                           \
    }                                                     \
  } while (0)

inline std::string slurp(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) {
    std::printf("  FAIL: cannot open %s\n", path.c_str());
    g_fail++;
    return std::string();
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

inline bool starts_with(const std::string& s, const char* p) {
  return s.compare(0, std::strlen(p), p) == 0;
}

// `#define NAME <int>` in a header, -1 when absent.
inline int define_int(const std::string& text, const char* name) {
  const std::vector<std::string> ls = lines_of(text);
  for (size_t i = 0; i < ls.size(); i++) {
    std::istringstream in(ls[i]);
    std::string hash, key;
    int v = 0;
    if ((in >> hash >> key >> v) && hash == "#define" && key == name) return v;
  }
  return -1;
}

// ── Montserrat line heights (LVGL font data: each built-in face's
// .line_height, the value lv_font_get_line_height() returns) ───────────────
inline int montserrat_line_h(int size) {
  switch (size) {
    case 12: return 15;
    case 14: return 16;
    case 16: return 18;
    case 20: return 22;
    case 24: return 27;
    case 28: return 30;
    case 36: return 40;
    case 48: return 52;
  }
  return -1;
}

// ── character.cpp's ladders: [family][default|heirloom][role] font sizes ───
enum Family { kBig = 0, kLean = 1, kStd = 2 };
enum Role { kHero = 0, kTitle, kBody, kLabel, kCaption, kClock, kRoles };
static const char* const kRoleName[kRoles] = {"hero",  "title",   "body",
                                              "label", "caption", "clock"};
static int g_ladder[3][2][kRoles];

inline void load_ladders() {
  std::printf("type ladders (character.cpp):\n");
  const std::vector<std::string> ls =
      lines_of(slurp(std::string(FW_DIR) +
                     "/projects/canary-display/src/ui/character.cpp"));
  int fam = -1, which = -1, role = 0, seen = 0;
  bool done = false;
  for (size_t i = 0; i < ls.size() && !done; i++) {
    const std::string t = trim(ls[i]);
    if (starts_with(t,
                    "#if defined(CD_FLAVOR_DASH) || defined(CD_AMOLED_GLASS)")) {
      fam = kBig;
    } else if (fam == kBig && starts_with(t, "#elif defined(CD_LEAN_BUILD)")) {
      fam = kLean;
    } else if (fam == kLean && starts_with(t, "#else")) {
      fam = kStd;
    } else if (fam == kStd && starts_with(t, "#endif")) {
      done = true;
    } else if (fam >= 0 && t.find("TypeLadder k_ladder_") != std::string::npos) {
      which = t.find("k_ladder_default") != std::string::npos    ? 0
              : t.find("k_ladder_heirloom") != std::string::npos ? 1
                                                                 : -1;
      CHECK(which >= 0, "unknown ladder: %s", t.c_str());
      role = 0;
    } else if (fam >= 0 && which >= 0 &&
               t.find("&lv_font_montserrat_") != std::string::npos) {
      const size_t at = t.find("&lv_font_montserrat_") + 20;
      const int size = std::atoi(t.c_str() + at);
      const size_t c = t.find("//");
      const std::string name =
          c == std::string::npos ? std::string() : trim(t.substr(c + 2));
      CHECK(role < kRoles, "ladder has more than %d roles", (int)kRoles);
      if (role >= kRoles) continue;
      CHECK(starts_with(name, kRoleName[role]),
            "ladder role %d is '%s', expected '%s' (TypeLadder order)", role,
            name.c_str(), kRoleName[role]);
      CHECK(montserrat_line_h(size) > 0,
            "montserrat_%d has no line height in this test's table", size);
      g_ladder[fam][which][role] = size;
      role++;
      seen++;
    }
  }
  CHECK(done, "character.cpp's three ladder branches not found");
  CHECK(seen == 3 * 2 * kRoles, "parsed %d ladder fonts, expected %d", seen,
        3 * 2 * kRoles);
}

// ── the display envs: panel + flavor bits, from the ini/boards/configs ─────
struct Env {
  std::string name, board, cfg;
  bool lean;
  int w, h;
  bool dash, nightstand, watch, amoled;
};

inline std::vector<Env> load_envs() {
  std::printf("display envs (envs/platformio/canary-display.ini):\n");
  const std::vector<std::string> ls = lines_of(
      slurp(std::string(FW_DIR) + "/envs/platformio/canary-display.ini"));
  std::vector<Env> envs;
  Env cur;
  bool in_env = false;
  for (size_t i = 0; i <= ls.size(); i++) {
    const std::string t = i < ls.size() ? trim(ls[i]) : std::string("[end]");
    if (!t.empty() && (t[0] == ';' || t[0] == '#')) continue;
    if (!t.empty() && t[0] == '[') {
      // An env that names no board of its own extends one that does.
      if (in_env && !cur.board.empty() && !cur.cfg.empty()) envs.push_back(cur);
      in_env = starts_with(t, "[env:canary-display-");
      cur = Env();
      cur.lean = false;
      if (in_env) cur.name = t.substr(5, t.size() - 6);
      continue;
    }
    if (!in_env) continue;
    size_t at = t.find("boards/");
    if (at != std::string::npos && t.find("/pins") != std::string::npos) {
      at += 7;
      cur.board = t.substr(at, t.find("/pins", at) - at);
    }
    at = t.find("configs/canary-display/");
    if (at != std::string::npos) {
      at += 23;
      size_t end = t.find_first_of(" \t", at);
      cur.cfg = t.substr(at, end == std::string::npos ? end : end - at);
    }
    if (t.find("-DCD_LEAN_BUILD=1") != std::string::npos) cur.lean = true;
  }
  CHECK(envs.size() >= 8, "only %d display envs parsed", (int)envs.size());
  for (size_t i = 0; i < envs.size(); i++) {
    Env& e = envs[i];
    const std::string pins =
        slurp(std::string(FW_DIR) + "/boards/" + e.board + "/pins/pins.h");
    e.w = define_int(pins, "LCD_WIDTH");
    e.h = define_int(pins, "LCD_HEIGHT");
    if (e.w < 0) e.w = define_int(pins, "TFT_WIDTH");
    if (e.h < 0) e.h = define_int(pins, "TFT_HEIGHT");
    CHECK(e.w > 0 && e.h > 0, "%s: no panel size in %s's pins.h",
          e.name.c_str(), e.board.c_str());
    const std::string cfg = slurp(std::string(FW_DIR) +
                                  "/configs/canary-display/" + e.cfg +
                                  "/config.h");
    e.dash = define_int(cfg, "CD_FLAVOR_DASH") == 1;
    e.nightstand = define_int(cfg, "CD_FLAVOR_NIGHTSTAND") == 1;
    e.watch = define_int(cfg, "CD_FLAVOR_WATCH") == 1;
    e.amoled = define_int(cfg, "CD_AMOLED_GLASS") == 1;
    CHECK(e.dash + e.nightstand + e.watch == 1,
          "%s: config %s must pick exactly one flavor", e.name.c_str(),
          e.cfg.c_str());
  }
  return envs;
}

// ── LVGL's measure, from LVGL's font data (montserrat_metrics.h) ──────────
typedef montserrat_metrics::Face Face;

inline const Face* face_of(int size) {
  for (int i = 0; i < montserrat_metrics::kFaceCount; i++) {
    if (montserrat_metrics::kFaces[i].size == size)
      return &montserrat_metrics::kFaces[i];
  }
  return nullptr;
}

// The carried glyph index of a code point (montserrat_metrics.h's order).
inline int glyph_index(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return (int)cp - 0x20;
  if (cp == 0xB0) return 95;
  if (cp == 0x2022) return 96;
  return -1;
}

// lv_font_get_glyph_width(font, a, b): lv_font_get_glyph_dsc_fmt_txt's
// advance, class kerning against the next letter and its rounding.
inline int glyph_px(const Face& f, uint32_t a, uint32_t b) {
  const int ia = glyph_index(a);
  if (ia < 0) {
    std::printf("  FAIL: U+%04X has no glyph in montserrat_%d\n", (unsigned)a,
                f.size);
    g_fail++;
    return 0;
  }
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

struct GlyphW {
  const Face* f;
  int operator()(uint32_t a, uint32_t b) const { return glyph_px(*f, a, b); }
};

inline int text_px(const Face& f, const char* text) {
  GlyphW g = {&f};
  return text_width(text, g);
}

// join_lines()'s measure: the row's own face, or the floor face.
struct Measure {
  const Face* own;
  const Face* floor;
  int operator()(const char* text, bool fl) const {
    return text_px(fl ? *floor : *own, text);
  }
};

// A minted field in a line template: this byte stands for "any character of
// the minting alphabet" (0x01 decodes as itself, and no glyph is looked up
// for it — worst_line replaces it).
static const char kSlot = '\x01';

inline std::string slots(int n) { return std::string((size_t)n, kSlot); }

// ── what provision.cpp mints and says ────────────────────────────────────
struct Minted {
  std::string alpha;         // the minting alphabet (key, tag, pseudonym)
  int key_len;               // AP_PASS_LEN
  std::string ssid_plain;    // "SecuraCV-" + 4 slots
  std::string ssid_tagged;   // ... + "-" + 2 slots (the unwritable-store path)
  // The stuck-phone hint's forms per glass: round, small rectangular, wide.
  std::vector<std::string> hint_round, hint_small, hint_wide;
  // The PhoneJoined hint's forms: small glass (the hint, then its narrow
  // form), wide glass.
  std::vector<std::string> phone_small, phone_wide;
  int bird_px;               // onboard_ui.cpp's watch-family brand mark
};
static Minted g_minted;

// Every C string literal in `text` (enough for ui_hint arguments).
inline std::vector<std::string> literals(const std::string& text) {
  std::vector<std::string> out;
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] != '"') continue;
    std::string lit;
    for (i++; i < text.size() && text[i] != '"'; i++) {
      if (text[i] == '\\' && i + 1 < text.size()) i++;
      lit += text[i];
    }
    out.push_back(lit);
  }
  return out;
}

// The three #if CD_FLAVOR branches (round / portrait / wide) that follow
// the first line containing `marker`: each branch's non-comment lines are
// collected for literals(). False when the shape is not in the file.
inline bool glass_branches(const std::vector<std::string>& ls,
                           const char* marker, std::string body[3]) {
  int branch = -1;
  bool armed = false;
  for (size_t i = 0; i < ls.size(); i++) {
    const std::string t = trim(ls[i]);
    if (!armed) {
      if (t.find(marker) != std::string::npos) armed = true;
    } else if (branch < 0 && t == "#if defined(CD_FLAVOR_WATCH) && "
                                  "!defined(CD_FLAVOR_NIGHTSTAND)") {
      branch = 0;
    } else if (branch == 0 && t == "#elif defined(CD_FLAVOR_WATCH)") {
      branch = 1;
    } else if (branch == 1 && t == "#else") {
      branch = 2;
    } else if (branch == 2 && t == "#endif") {
      return true;
    } else if (branch >= 0 && !starts_with(t, "//")) {
      body[branch] += t + "\n";
    }
  }
  return false;
}

inline void load_minted_core() {
  std::printf("what the unit mints (provision.cpp, device_pseudonym.h):\n");
  const std::string fw = FW_DIR;
  const std::string prov =
      slurp(fw + "/projects/canary-display/src/net/provision.cpp");
  const std::vector<std::string> ls = lines_of(prov);

  // The key: AP_PASS_LEN characters of render_password's alphabet.
  g_minted.key_len = -1;
  for (size_t i = 0; i < ls.size(); i++) {
    const std::string t = trim(ls[i]);
    if (starts_with(t, "constexpr size_t AP_PASS_LEN = "))
      g_minted.key_len = std::atoi(t.c_str() + 31);
  }
  CHECK(g_minted.key_len > 0, "no AP_PASS_LEN in provision.cpp");

  // render_password's alphabet, read off its output for every byte value,
  // must be device_pseudonym's (the SSID's four characters).
  std::set<char> key_chars;
  for (int b = 0; b < 216; b++) {
    const uint8_t byte = (uint8_t)b;
    char out[2];
    canary::net::render_password(&byte, 1, out, sizeof(out));
    key_chars.insert(out[0]);
  }
  const std::string dp =
      slurp(fw + "/common/identity/device_pseudonym.h");
  const size_t at = dp.find("ALPHABET[]");
  const std::vector<std::string> lit =
      literals(at == std::string::npos ? std::string()
                                       : dp.substr(at, dp.find(';', at) - at));
  CHECK(lit.size() == 1, "device_pseudonym.h's ALPHABET not found");
  g_minted.alpha = lit.empty() ? std::string() : lit[0];
  const std::set<char> ssid_chars(g_minted.alpha.begin(), g_minted.alpha.end());
  CHECK(ssid_chars == key_chars && !key_chars.empty(),
        "the key and the network name no longer share one alphabet");

  // The network name's two shapes (the tag is render_password's 2 chars).
  CHECK(prov.find("\"SecuraCV-%.4s\"") != std::string::npos &&
            prov.find("\"SecuraCV-%.4s-%s\"") != std::string::npos &&
            prov.find("char tag[3];") != std::string::npos,
        "provision.cpp's network name is no longer SecuraCV-XXXX[-YY]");
  g_minted.ssid_plain = "SecuraCV-" + slots(4);
  g_minted.ssid_tagged = "SecuraCV-" + slots(4) + "-" + slots(2);

  // The stuck-phone hint, per glass, from the branch that sets it.
  std::string body[3];
  CHECK(glass_branches(ls, "ctx.stuck_hinted = true;", body),
        "provision.cpp's stuck-phone hint branches (round / portrait / "
        "wide) not found after ctx.stuck_hinted = true;");
  g_minted.hint_round = literals(body[0]);
  g_minted.hint_small = literals(body[1]);
  g_minted.hint_wide = literals(body[2]);
  CHECK(g_minted.hint_round.size() == 1 && g_minted.hint_small.size() >= 1 &&
            g_minted.hint_small.size() <= 2 && g_minted.hint_wide.size() == 1,
        "stuck-phone hint forms: %d round, %d portrait, %d wide",
        (int)g_minted.hint_round.size(), (int)g_minted.hint_small.size(),
        (int)g_minted.hint_wide.size());
  // The PhoneJoined hint, per glass, from the branch that sets it once the
  // phone has sat on the AP for HINT_AFTER_MS without opening the page.
  int branch = -1;
  bool armed = false, done = false;
  std::string pj[2];
  for (size_t i = 0; i < ls.size() && !done; i++) {
    const std::string t = trim(ls[i]);
    if (t.find("(int32_t)HINT_AFTER_MS") != std::string::npos) {
      armed = true;
    } else if (armed && branch < 0 && t == "#if defined(CD_FLAVOR_WATCH)") {
      branch = 0;
    } else if (branch == 0 && t == "#else") {
      branch = 1;
    } else if (branch == 1 && t == "#endif") {
      done = true;
    } else if (branch >= 0 && !starts_with(t, "//")) {
      pj[branch] += t + "\n";
    }
  }
  CHECK(done, "provision.cpp's PhoneJoined hint branches (small / wide) not "
              "found after HINT_AFTER_MS");
  g_minted.phone_small = literals(pj[0]);
  g_minted.phone_wide = literals(pj[1]);
  CHECK(g_minted.phone_small.size() == 2 && g_minted.phone_wide.size() == 1,
        "PhoneJoined hint forms: %d small (want the hint and its narrow "
        "form), %d wide", (int)g_minted.phone_small.size(),
        (int)g_minted.phone_wide.size());

  // The brand mark's watch-family square, for the Join-scene seat check.
  const std::string obui =
      slurp(fw + "/projects/canary-display/src/ui/onboard_ui.cpp");
  g_minted.bird_px = -1;
  const size_t bat = obui.find("constexpr int BIRD_PX = ");
  if (bat != std::string::npos)
    g_minted.bird_px = std::atoi(obui.c_str() + bat + 24);
  CHECK(g_minted.bird_px > 0, "onboard_ui.cpp's BIRD_PX not found");
}
