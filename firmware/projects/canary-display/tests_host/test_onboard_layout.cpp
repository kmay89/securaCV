// Host test for the Join scene's stack (canary/ui/onboard_layout.h): on every
// display panel the PlatformIO tree builds, with both type ladders, the
// first-boot title, QR card, credentials line and hint line never cross one
// another or leave the glass — the F43 defect was the dash's "or join ...
// password" line drawn 14 px inside the QR card (the round watch's network
// name, 7 px), with each glass placing card and captions by its own
// literals.
//
// And what those rows SAY is never cut (F45): on the 172 px nightstand the
// joined "SecuraCV-XXXX  •  <key>" line and the stuck-phone hint ended in an
// ellipsis. For every glass and ladder, join_lines() is run on the widest
// network name and key the unit can mint (a search over the minting
// alphabet, kerning included) and on the stuck-phone hint provision.cpp
// actually sets, and every row must fit the width the glass fits it to.
// The name and the key must be on the glass in every one of those cases —
// with the stuck-phone hint standing too: that hint is exactly when the
// phone needs the key again, and when the QR does not scan (or never
// rendered: join_lines does not depend on it) the text is the only way in.
//
// And the coach line of the scenes without credentials is never cut either
// (F50): every Fail hint (join_failure_hint, 175-219 px at 12 px) and the
// PhoneJoined hint (182 px under Heirloom) went on one row and ended in an
// ellipsis on the round watch's 142 px band and the 156/164 px portrait rows.
// For every glass and ladder, hint_lines() is run on each of them, with the
// narrow form the glass is handed, and each row must fit its width.
// And no scene's title or body is cut (F65): they kept LONG_DOT at the
// width they were first fitted to, so "Nice - check your phone" and "No
// address from the router" were cut on the round watch and the 156/164 px
// portrait glass, and under Heirloom eleven more lines. For every glass and
// ladder, each line scene_copy() holds (and each Fail reason with its narrow
// label, as provision.cpp hands them) goes through fit_line() at the width
// scene_line_w() gives its latitude, and must read whole; a network name
// goes through name_line(). And nothing crosses the halo (F66): the 236 px
// ring ran through the hint rows of the 172/180x320 and 240x280 glass. Every
// row of every scene and the bird's seats must be wholly inside the ring's
// stroke or wholly outside it.
// Text is measured with LVGL's own Montserrat data (montserrat_metrics.h,
// generated from the pinned LVGL by firmware/scripts/gen_montserrat_metrics.py)
// the way lv_font_get_glyph_width reads it — never an estimated width.
//
// Nothing about the glass is retyped here. The panels come from each env's
// board pins.h via envs/platformio/canary-display.ini; the flavor bits
// (dash / nightstand / watch / AMOLED, and the lean build) from the env's
// config.h and flags; the font sizes from character.cpp's type ladders
// (parsed in order, role by role); the stuck-phone and PhoneJoined hints,
// the key length and the network-name shapes from provision.cpp; the Fail
// hints from wifi_join_policy.h itself (the table provision.cpp hands the
// glass); the minting alphabet from device_pseudonym.h and provision_core.h. Only LVGL's font data is a table
// (the line heights below, and the generated glyph metrics) — theirs, not
// ours.
//
// Built at -std=gnu++11 ON PURPOSE (see the Makefile rule): the Arduino
// parity sketch compiles the header on esp32 core 2.0.17 as gnu++11.
//
// Prints "ALL ONBOARD LAYOUT TESTS PASSED" on success. Build (repo root):
//
//   g++ -std=gnu++11 -Wall -Wextra -I firmware/projects/canary-display/include
//     -I firmware/common '-DFW_DIR="firmware"'
//     firmware/projects/canary-display/tests_host/test_onboard_layout.cpp
//     -o t && ./t

#include "canary/ui/onboard_layout.h"
#include "montserrat_metrics.h"
#include "network/provision_core.h"
#include "network/wifi_join_policy.h"

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

static std::string slurp(const std::string& path) {
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

static std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

static bool starts_with(const std::string& s, const char* p) {
  return s.compare(0, std::strlen(p), p) == 0;
}

// `#define NAME <int>` in a header, -1 when absent.
static int define_int(const std::string& text, const char* name) {
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
static int montserrat_line_h(int size) {
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

static void load_ladders() {
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

static std::vector<Env> load_envs() {
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

static const Face* face_of(int size) {
  for (int i = 0; i < montserrat_metrics::kFaceCount; i++) {
    if (montserrat_metrics::kFaces[i].size == size)
      return &montserrat_metrics::kFaces[i];
  }
  return nullptr;
}

// The carried glyph index of a code point (montserrat_metrics.h's order).
static int glyph_index(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return (int)cp - 0x20;
  if (cp == 0xB0) return 95;
  if (cp == 0x2022) return 96;
  return -1;
}

// lv_font_get_glyph_width(font, a, b): lv_font_get_glyph_dsc_fmt_txt's
// advance, class kerning against the next letter and its rounding.
static int glyph_px(const Face& f, uint32_t a, uint32_t b) {
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

static int text_px(const Face& f, const char* text) {
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

static std::string slots(int n) { return std::string((size_t)n, kSlot); }

// printf a line template (kJoinedFmt and friends) around slot runs.
static std::string fmt2(const char* fmt, const std::string& a,
                        const std::string& b) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, a.c_str(), b.c_str());
  return buf;
}
static std::string fmt1(const char* fmt, const std::string& a) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, a.c_str());
  return buf;
}

struct Worst {
  int w;                 // the widest the line can lay out in the face
  std::string ssid_or_key;  // the slot characters that make it that wide
  std::string text;      // the whole line, slots filled
};

// The widest a line template can lay out when every slot is any character of
// `alpha`: a search over each slot's choice, where a glyph's width depends on
// the letter after it (kerning), so the widest string is not simply the
// widest letter repeated. Exact, not a bound.
static Worst worst_line(const Face& f, const std::string& tmpl,
                        const std::string& alpha) {
  std::vector<std::vector<uint32_t> > cand;
  int i = 0;
  for (uint32_t cp = utf8_next(tmpl.c_str(), &i); cp != 0;
       cp = utf8_next(tmpl.c_str(), &i)) {
    std::vector<uint32_t> c;
    if (cp == (uint32_t)kSlot) {
      for (size_t k = 0; k < alpha.size(); k++) c.push_back((uint8_t)alpha[k]);
    } else {
      c.push_back(cp);
    }
    cand.push_back(c);
  }
  const size_t n = cand.size();
  std::vector<std::vector<int> > best(n), from(n);
  for (size_t p = 0; p < n; p++) {
    best[p].assign(cand[p].size(), 0);
    from[p].assign(cand[p].size(), 0);
    if (p == 0) continue;
    for (size_t m = 0; m < cand[p].size(); m++) {
      int top = -1;
      for (size_t k = 0; k < cand[p - 1].size(); k++) {
        const int v = best[p - 1][k] + glyph_px(f, cand[p - 1][k], cand[p][m]);
        if (v > top) {
          top = v;
          from[p][m] = (int)k;
        }
      }
      best[p][m] = top;
    }
  }
  Worst out;
  out.w = 0;
  if (n == 0) return out;
  int end = 0, top = -1;
  for (size_t k = 0; k < cand[n - 1].size(); k++) {
    const int v = best[n - 1][k] + glyph_px(f, cand[n - 1][k], 0);
    if (v > top) {
      top = v;
      end = (int)k;
    }
  }
  out.w = top;
  std::vector<uint32_t> pick(n);
  for (size_t p = n; p-- > 0;) {
    pick[p] = cand[p][end];
    if (p > 0) end = from[p][end];
  }
  for (size_t p = 0; p < n; p++) {
    const uint32_t cp = pick[p];
    if (cp < 0x80) {
      out.text += (char)cp;
      if (cand[p].size() > 1) out.ssid_or_key += (char)cp;
    } else if (cp < 0x800) {
      out.text += (char)(0xC0 | (cp >> 6));
      out.text += (char)(0x80 | (cp & 0x3F));
    } else {
      out.text += (char)(0xE0 | (cp >> 12));
      out.text += (char)(0x80 | ((cp >> 6) & 0x3F));
      out.text += (char)(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

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
static std::vector<std::string> literals(const std::string& text) {
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
static bool glass_branches(const std::vector<std::string>& ls,
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

static void load_minted() {
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

  // The Fail hint: provision.cpp hands the glass wifi_join_policy.h's hint
  // AND its narrow form, in one call — the narrow form is the rung
  // hint_lines() falls back to (F50), and a call that drops it leaves a
  // narrow row nothing to show but a cut.
  const size_t call = prov.find("ui_hint(canary::net::join_failure_hint(");
  const std::string stmt =
      call == std::string::npos ? std::string()
                                : prov.substr(call, prov.find(';', call) - call);
  CHECK(stmt.find("canary::net::join_failure_hint_narrow(") != std::string::npos,
        "provision.cpp's Fail hint no longer hands the glass "
        "join_failure_hint_narrow() with join_failure_hint(): \"%s\"",
        stmt.c_str());

  // The brand mark's watch-family square, for the Join-scene seat check.
  const std::string obui =
      slurp(fw + "/projects/canary-display/src/ui/onboard_ui.cpp");
  g_minted.bird_px = -1;
  const size_t bat = obui.find("constexpr int BIRD_PX = ");
  if (bat != std::string::npos)
    g_minted.bird_px = std::atoi(obui.c_str() + bat + 24);
  CHECK(g_minted.bird_px > 0, "onboard_ui.cpp's BIRD_PX not found");

  // F65: the Fail title is handed with its narrow form, the way the hint is
  // (the narrow label is a rung fit_line falls back to).
  const size_t fcall = prov.find("ui_stage(canary::ui::ObStage::Fail,");
  const std::string fstmt =
      fcall == std::string::npos
          ? std::string()
          : prov.substr(fcall, prov.find(';', fcall) - fcall);
  CHECK(fstmt.find("canary::net::join_failure_label_narrow(") !=
            std::string::npos,
        "provision.cpp's Fail stage no longer hands the glass "
        "join_failure_label_narrow() with the reason: \"%s\"",
        fstmt.c_str());
  // F65: every title and body the glass sets comes from onboard_layout.h
  // (scene_copy / join_title, fitted by scene_text), which this test
  // measures. A literal set straight on the title or body label bypasses
  // the fit and the measure — only the wide glass's Join titles (content-
  // sized on an 800 px panel) and empty lines are set that way.
  {
    const char* const kWideJoin[] = {"Scan with your phone camera",
                                     "On your phone, join this network"};
    const char* const kSetters[] = {"lv_label_set_text(s_title, ",
                                    "lv_label_set_text(s_body, "};
    for (int k = 0; k < 2; ++k) {
      for (size_t at = obui.find(kSetters[k]); at != std::string::npos;
           at = obui.find(kSetters[k], at + 1)) {
        const std::string stmt =
            obui.substr(at, obui.find(';', at) - at);
        const std::vector<std::string> lits = literals(stmt);
        for (size_t i = 0; i < lits.size(); ++i) {
          bool ok = lits[i].empty();
          for (int w = 0; w < 2; ++w) ok = ok || lits[i] == kWideJoin[w];
          CHECK(ok, "onboard_ui.cpp sets \"%s\" on a scene label directly "
                    "(%s...): scene copy lives in onboard_layout.h",
                lits[i].c_str(), kSetters[k]);
        }
      }
    }
    CHECK(obui.find("lv_label_set_text_fmt(s_title") == std::string::npos &&
              obui.find("lv_label_set_text_fmt(s_body") == std::string::npos,
          "onboard_ui.cpp formats a scene title or body itself (the old "
          "\"%%.28s\" network-name clip) instead of fitting it");
    // F66: the small-glass halo is onboard_layout.h's, not a constant.
    CHECK(obui.find("onboardlayout::halo_ring(") != std::string::npos &&
              obui.find("RING_D = 236") == std::string::npos,
          "onboard_ui.cpp no longer seats the small-glass halo with "
          "onboardlayout::halo_ring()");
  }
  std::printf("  key %d of %d chars; hints: \"%s\" | \"%s\"%s%s%s | \"%s\"\n",
              g_minted.key_len, (int)g_minted.alpha.size(),
              g_minted.hint_round.empty() ? "" : g_minted.hint_round[0].c_str(),
              g_minted.hint_small.empty() ? "" : g_minted.hint_small[0].c_str(),
              g_minted.hint_small.size() > 1 ? " / \"" : "",
              g_minted.hint_small.size() > 1 ? g_minted.hint_small[1].c_str()
                                             : "",
              g_minted.hint_small.size() > 1 ? "\"" : "",
              g_minted.hint_wide.empty() ? "" : g_minted.hint_wide[0].c_str());
}

// LVGL's own line heights must be the table above (they are the same data).
static void test_metrics_table() {
  std::printf("montserrat_metrics.h:\n");
  CHECK(montserrat_metrics::kFaceCount > 0, "no faces carried");
  for (int i = 0; i < montserrat_metrics::kFaceCount; i++) {
    const Face& f = montserrat_metrics::kFaces[i];
    CHECK(montserrat_line_h(f.size) == f.line_height,
          "montserrat_%d: the line-height table says %d, LVGL says %d",
          f.size, montserrat_line_h(f.size), f.line_height);
  }
  // Spot-check the measure against LVGL's own arithmetic, one pair by hand:
  // montserrat_12 'W' is adv 216/16 px, and "AV" kerns tighter than "AA".
  const Face* f12 = face_of(12);
  CHECK(f12 != nullptr, "montserrat_12 is not carried");
  if (f12 != nullptr) {
    CHECK(glyph_px(*f12, 'W', 0) == (216 + 8) >> 4, "W is %d px",
          glyph_px(*f12, 'W', 0));
    CHECK(glyph_px(*f12, 'A', 'V') < glyph_px(*f12, 'A', 'A'),
          "no kerning between A and V");
  }
}

// ── the stack's invariants on one glass ───────────────────────────────────
static int band_chord_px(int dia, int top, int h) {
  return roundframe::band_chord(dia, roundframe::kEdgeMargin, top, h);
}

static const char* face_name(bool floor) { return floor ? "floor" : "own"; }

// Every JoinFailure, in enum order (the Fail hints the glass can show).
static const canary::net::JoinFailure kFailures[] = {
    canary::net::JoinFailure::NotFound, canary::net::JoinFailure::BadPassword,
    canary::net::JoinFailure::NoAddress, canary::net::JoinFailure::Unknown};
static const int kFailureCount = (int)(sizeof(kFailures) / sizeof(kFailures[0]));

// One coach line and the forms the glass is handed for it.
struct CoachLine {
  const char* what;    // for the messages
  const char* hint;    // the whole hint
  const char* narrow;  // its narrow form (may be null)
};

// Every coach line a scene without credentials sets on small glass: each
// Fail hint (wifi_join_policy.h, with its narrow form, as provision.cpp hands
// them over) and the PhoneJoined hint (provision.cpp's small-glass branch).
static std::vector<CoachLine> small_coach_lines() {
  std::vector<CoachLine> out;
  for (int i = 0; i < kFailureCount; i++) {
    CoachLine c = {canary::net::join_failure_label(kFailures[i]),
                   canary::net::join_failure_hint(kFailures[i]),
                   canary::net::join_failure_hint_narrow(kFailures[i])};
    out.push_back(c);
  }
  if (!g_minted.phone_small.empty()) {
    CoachLine c = {"PhoneJoined", g_minted.phone_small[0].c_str(),
                   g_minted.phone_small.size() > 1
                       ? g_minted.phone_small[1].c_str()
                       : nullptr};
    out.push_back(c);
  }
  return out;
}

// F50 on small glass: the coach line of a scene without credentials
// (PhoneJoined, Fail) fits the two rows the credentials leave — the
// credentials row (creds_w) over the hint row (low_w) — in the row's own
// face or the floor face, and what the rows say is the hint or its narrow
// form, word for word. Returns a one-line summary of the rung each took.
static std::string check_hint_rows(const Env& e, const char* lad_name,
                                   int creds_w, int low_w, const Measure& m) {
  const char* n = e.name.c_str();
  const std::vector<CoachLine> lines = small_coach_lines();
  CHECK(lines.size() == (size_t)kFailureCount + 1,
        "%s/%s: %d coach lines, want every Fail hint and PhoneJoined's", n,
        lad_name, (int)lines.size());
  std::string summary;
  for (size_t i = 0; i < lines.size(); i++) {
    const CoachLine& c = lines[i];
    const HintLines h = hint_lines(creds_w, low_w, c.hint, c.narrow, m);
    CHECK(h.upper.fits && m(h.upper.text, h.upper.floor) <= creds_w,
          "%s/%s: %s: the upper row \"%s\" (%s face) is cut on a %d px row",
          n, lad_name, c.what, h.upper.text, face_name(h.upper.floor),
          creds_w);
    CHECK(h.lower.fits && m(h.lower.text, h.lower.floor) <= low_w,
          "%s/%s: %s: the hint row \"%s\" (%s face) is cut on a %d px row",
          n, lad_name, c.what, h.lower.text, face_name(h.lower.floor), low_w);
    CHECK(h.split || h.upper.text[0] == '\0',
          "%s/%s: %s: one-row hint, yet the upper row says \"%s\"", n,
          lad_name, c.what, h.upper.text);
    CHECK(!h.split || h.upper.floor == h.lower.floor,
          "%s/%s: %s: a split hint in two faces", n, lad_name, c.what);
    // Word for word: the rows read back as the hint or its narrow form.
    const std::string said =
        h.split ? std::string(h.upper.text) + " " + h.lower.text
                : std::string(h.lower.text);
    const bool whole = said == c.hint;
    const bool narrow = c.narrow != nullptr && said == c.narrow;
    CHECK(whole || narrow,
          "%s/%s: %s: the rows say \"%s\" — neither \"%s\" nor \"%s\"", n,
          lad_name, c.what, said.c_str(), c.hint, c.narrow ? c.narrow : "");
    // And on every display env it is the whole hint, on one row or two: the
    // narrow form is a rung no shipped glass reaches (test_f50_pins holds
    // it), the docs say the fixes read whole, and onboard_probe.mjs reads
    // the whole form off the emulator's glass word for word. A glass that
    // needs the shorter copy is a copy change: word it, then relax this.
    CHECK(whole,
          "%s/%s: %s: the glass falls to the narrow form \"%s\" (the whole "
          "hint \"%s\" fits neither row nor both)",
          n, lad_name, c.what, said.c_str(), c.hint);
    // The narrow form is a real rung: on its own, in the floor face, it
    // fits the hint row, whatever hint_lines() picked above it.
    CHECK(c.narrow == nullptr || m(c.narrow, true) <= low_w,
          "%s/%s: %s: the narrow form \"%s\" is %d px on a %d px row", n,
          lad_name, c.what, c.narrow ? c.narrow : "",
          c.narrow ? m(c.narrow, true) : 0, low_w);
    char one[64];
    std::snprintf(one, sizeof(one), "%s%s %s%s", i ? ", " : "", c.what,
                  whole ? (h.split ? "split" : "whole")
                        : (h.split ? "narrow split" : "narrow"),
                  h.lower.floor ? " (floor)" : "");
    summary += one;
  }
  return summary;
}

// F45 on small glass: what join_lines() puts on the text rows, for the
// widest network name and key the unit can mint and for the stuck-phone
// hint, fits the rows the labels are fitted to — and the name and the key
// are always among them.
static void check_rows(const Env& e, int which, const Stack& s, const Rows& r,
                       bool round, int fam) {
  const char* n = e.name.c_str();
  const char* lad_name = which == 0 ? "default" : "heirloom";
  const Face* own = face_of(g_ladder[fam][which][kCaption]);
  const Face* floor = face_of(g_ladder[fam][0][kCaption]);
  CHECK(own != nullptr && floor != nullptr,
        "%s/%s: montserrat_%d or _%d not in montserrat_metrics.h (re-run "
        "gen_montserrat_metrics.py)", n, lad_name,
        g_ladder[fam][which][kCaption], g_ladder[fam][0][kCaption]);
  if (own == nullptr || floor == nullptr) return;
  CHECK(floor->size <= own->size,
        "%s/%s: the floor face (%d) is larger than the row's own (%d)", n,
        lad_name, floor->size, own->size);
  const int dia = e.w < e.h ? e.w : e.h;
  const int y0 = (e.h - dia) / 2;
  const int creds_w =
      round ? band_chord_px(dia, s.creds_top - y0, r.creds_h)
            : e.w - 2 * roundframe::kRectSidePad;
  const int low_w = round ? band_chord_px(dia, s.hint_top - y0, r.hint_h)
                          : e.w - 2 * roundframe::kRectSidePad;
  // The note row carries the caption face too (onboard_ui's s_note).
  const int note_w = round ? band_chord_px(dia, s.note_top - y0, r.hint_h)
                           : e.w - 2 * roundframe::kRectSidePad;
  const Measure m = {own, floor};
  const std::vector<std::string>& hints =
      round ? g_minted.hint_round : g_minted.hint_small;
  const char* hint = hints.empty() ? "" : hints[0].c_str();
  const char* narrow = hints.size() > 1 ? hints[1].c_str() : nullptr;

  // The guarantee: the last thing each row can fall back to fits whatever
  // the unit minted — the network name and the bare key in the floor face.
  const std::string& A = g_minted.alpha;
  const Worst ssid_p = worst_line(*floor, g_minted.ssid_plain, A);
  const Worst ssid_t = worst_line(*floor, g_minted.ssid_tagged, A);
  const Worst key = worst_line(*floor, slots(g_minted.key_len), A);
  CHECK(ssid_p.w <= creds_w && ssid_t.w <= creds_w,
        "%s/%s: a network name can be %d px (\"%s\") on a %d px row", n,
        lad_name, ssid_t.w > ssid_p.w ? ssid_t.w : ssid_p.w,
        (ssid_t.w > ssid_p.w ? ssid_t : ssid_p).text.c_str(), creds_w);
  CHECK(key.w <= low_w, "%s/%s: a key can be %d px (\"%s\") on a %d px row",
        n, lad_name, key.w, key.text.c_str(), low_w);

  // join_lines() itself, on a typical unit and on the widest ones (the
  // witnesses of the search in the row's own face), with and without the
  // stuck-phone hint standing.
  const Worst wide_ssid_p = worst_line(*own, g_minted.ssid_plain, A);
  const Worst wide_ssid_t = worst_line(*own, g_minted.ssid_tagged, A);
  const Worst wide_key = worst_line(*own, fmt1(kPassFmt, slots(g_minted.key_len)), A);
  const char* ssids[3] = {"SecuraCV-A7K2", wide_ssid_p.text.c_str(),
                          wide_ssid_t.text.c_str()};
  const char* keys[3] = {"p7Rm2Kqf", wide_key.ssid_or_key.c_str(),
                         wide_key.ssid_or_key.c_str()};
  JoinLines typ = {};
  JoinLines typ_hint = {};
  for (int u = 0; u < 3; u++) {
    for (int h = 0; h < 2; h++) {
      const JoinLines j =
          join_lines(round, creds_w, low_w, note_w, ssids[u], keys[u],
                     h ? hint : "", h ? narrow : nullptr, m);
      CHECK(j.creds.fits && m(j.creds.text, j.creds.floor) <= creds_w,
            "%s/%s: credentials row \"%s\" (%s face) is cut on a %d px row",
            n, lad_name, j.creds.text, face_name(j.creds.floor), creds_w);
      CHECK(j.low.fits && m(j.low.text, j.low.floor) <= low_w,
            "%s/%s: the row under it, \"%s\" (%s face), is cut on a %d px row",
            n, lad_name, j.low.text, face_name(j.low.floor), low_w);
      CHECK(j.note.fits && m(j.note.text, j.note.floor) <= note_w,
            "%s/%s: the note row, \"%s\" (%s face), is cut on a %d px row", n,
            lad_name, j.note.text, face_name(j.note.floor), note_w);
      CHECK(round ? j.split : true, "%s: round glass must split", n);
      if (!j.split) {
        CHECK(m(j.creds.text, false) <= creds_w && !j.creds.floor,
              "%s/%s: joined line kept though it does not fit", n, lad_name);
      }
      // The name and the key are on the glass, hint or no hint: joined on
      // the credentials row, or split over it and the row under it.
      CHECK(std::strstr(j.creds.text, ssids[u]) != nullptr &&
                std::strstr(j.split ? j.low.text : j.creds.text, keys[u]) !=
                    nullptr,
            "%s/%s: %s, the rows say \"%s\" | \"%s\" — the name or the key "
            "(%s) is not on the glass",
            n, lad_name, h ? "with the stuck-phone hint up" : "no hint",
            j.creds.text, j.low.text, keys[u]);
      // The hint, when one stands, is on the glass too: the row under a
      // joined line, the note row on split glass. Nothing else is.
      const char* hint_row = j.split ? j.note.text : j.low.text;
      const char* free_row = j.split ? "" : j.note.text;
      if (h == 0) {
        CHECK(hint_row[0] == '\0' && free_row[0] == '\0',
              "%s/%s: no hint, yet a row says \"%s\"", n, lad_name,
              hint_row[0] ? hint_row : free_row);
      } else {
        CHECK((std::strcmp(hint_row, hint) == 0 ||
               (narrow != nullptr && std::strcmp(hint_row, narrow) == 0)) &&
                  free_row[0] == '\0',
              "%s/%s: the hint row shows \"%s\"", n, lad_name, hint_row);
      }
      if (u == 0 && h == 0) typ = j;
      if (u == 0 && h == 1) typ_hint = j;
    }
  }
  char joined[kLineCap];
  std::snprintf(joined, sizeof(joined), kJoinedFmt, ssids[0], keys[0]);
  const Line& typ_hint_line = typ_hint.split ? typ_hint.note : typ_hint.low;
  std::printf("  %20s rows %3d/%3d/%3d  joined %3d px -> %-6s \"%s\" | \"%s\"%s"
              "  hint \"%s\"%s on the %s row\n",
              "", creds_w, low_w, note_w, text_px(*own, joined),
              typ.split ? "split" : "joined", typ.creds.text, typ.low.text,
              typ.low.floor ? " (floor)" : "", typ_hint_line.text,
              typ_hint_line.floor ? " (floor)" : "",
              typ_hint.split ? "note" : "hint");
  // F50: the scenes without credentials hand the same two rows to their
  // coach line.
  const std::string coach = check_hint_rows(e, lad_name, creds_w, low_w, m);
  std::printf("  %20s coach lines: %s\n", "", coach.c_str());
}

// Wide glass keeps one worded credentials line and a separate hint line,
// content-sized (not fitted): each must stay on the panel.
static void check_wide_rows(const Env& e, int which, int fam) {
  const char* n = e.name.c_str();
  const char* lad_name = which == 0 ? "default" : "heirloom";
  const Face* label = face_of(g_ladder[fam][which][kLabel]);
  const Face* caption = face_of(g_ladder[fam][which][kCaption]);
  CHECK(label != nullptr && caption != nullptr,
        "%s/%s: a wide face is not in montserrat_metrics.h", n, lad_name);
  if (label == nullptr || caption == nullptr) return;
  const int row = e.w - 2 * roundframe::kRectSidePad;
  const std::string key = slots(g_minted.key_len);
  const char* fmts[2] = {kWideScanFmt, kWideTypeFmt};
  int widest = 0;
  for (int f = 0; f < 2; f++) {
    const Worst p = worst_line(*label, fmt2(fmts[f], g_minted.ssid_plain, key),
                               g_minted.alpha);
    const Worst t = worst_line(*label, fmt2(fmts[f], g_minted.ssid_tagged, key),
                               g_minted.alpha);
    CHECK(p.w <= row && t.w <= row,
          "%s/%s: the credentials line can be %d px (\"%s\") on %d px", n,
          lad_name, t.w > p.w ? t.w : p.w, (t.w > p.w ? t : p).text.c_str(),
          row);
    if (t.w > widest) widest = t.w;
    if (p.w > widest) widest = p.w;
  }
  const int hint_w = g_minted.hint_wide.empty()
                         ? 0
                         : text_px(*caption, g_minted.hint_wide[0].c_str());
  CHECK(hint_w <= row, "%s/%s: the stuck-phone hint is %d px on %d px", n,
        lad_name, hint_w, row);
  // F50: the scenes without credentials set their coach line whole on the
  // hint row here (the wide branch of refresh_bottom): every Fail hint and
  // the wide PhoneJoined hint must fit it.
  int coach_w = 0;
  for (int i = 0; i < kFailureCount; i++) {
    const char* h = canary::net::join_failure_hint(kFailures[i]);
    const int w = text_px(*caption, h);
    CHECK(w <= row, "%s/%s: the Fail hint \"%s\" is %d px on %d px", n,
          lad_name, h, w, row);
    if (w > coach_w) coach_w = w;
  }
  const int phone_w = g_minted.phone_wide.empty()
                          ? 0
                          : text_px(*caption, g_minted.phone_wide[0].c_str());
  CHECK(!g_minted.phone_wide.empty() && phone_w <= row,
        "%s/%s: the PhoneJoined hint is %d px on %d px", n, lad_name, phone_w,
        row);
  if (phone_w > coach_w) coach_w = phone_w;
  // F65: the Connecting scene's network name, in the body face, never past
  // the panel's row: whole, or cut around "..." (name_line).
  const Face* body = face_of(g_ladder[fam][which][kBody]);
  const Face* body_floor = face_of(g_ladder[fam][0][kBody]);
  CHECK(body && body_floor, "%s/%s: a wide body face is not carried", n,
        lad_name);
  if (body && body_floor) {
    const Measure bm = {body, body_floor};
    const std::string names[2] = {"HomeNet", std::string(32, 'W')};
    for (int k = 0; k < 2; ++k) {
      Line line;
      name_line(line, names[k].c_str(), row, bm);
      CHECK(line.fits && bm(line.text, line.floor) <= row &&
                (k == 1 || !line.cut),
            "%s/%s: the network name \"%s\" is \"%s\" on %d px", n, lad_name,
            names[k].c_str(), line.text, row);
    }
  }
  std::printf("  %20s row %3d  credentials <= %3d px  hint %3d px  coach "
              "lines <= %3d px\n", "", row, widest, hint_w, coach_w);
}

// ── F65 / F66: the scenes' lines and the halo on small glass ─────────────

// A box on the glass: [x0, x0 + w) x [y0, y0 + h), what for the messages.
struct Box {
  std::string what;
  int x0, y0, w, h;
};

static const char* stage_name(ObStage st) {
  switch (st) {
    case ObStage::Hello: return "Hello";
    case ObStage::Join: return "Join";
    case ObStage::PhoneJoined: return "PhoneJoined";
    case ObStage::Connecting: return "Connecting";
    case ObStage::Fail: return "Fail";
    case ObStage::Success: return "Success";
  }
  return "?";
}

// One centered line a scene sets on small glass, and the forms it is handed.
struct SceneLine {
  std::string what;
  const char* full;
  const char* narrow;
  int off;     // its center's offset from the panel's center
  bool title;  // the body face (title) or the caption face (body)
  bool name;   // a network name (name_line), not our copy
};

// Every centered line of every scene but Join (whose title rides the
// stack's title row): scene_copy's words, each Fail reason with its narrow
// label as provision.cpp hands them, and a typical network name.
static std::vector<SceneLine> scene_lines() {
  static const ObStage kStages[] = {ObStage::Hello, ObStage::PhoneJoined,
                                    ObStage::Connecting, ObStage::Fail,
                                    ObStage::Success};
  std::vector<SceneLine> out;
  for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); ++i) {
    const ObStage st = kStages[i];
    const SceneCopy c = scene_copy(st);
    const std::string n = stage_name(st);
    CHECK(c.title.full != nullptr && c.title.full[0] != '\0',
          "%s has no title of its own", n.c_str());
    SceneLine t = {n + " title", c.title.full, c.title.narrow, c.title_off,
                   true, false};
    out.push_back(t);
    if (st == ObStage::Fail) {
      for (int f = 0; f < kFailureCount; ++f) {
        SceneLine l = {n + " title (" +
                           canary::net::join_failure_label(kFailures[f]) + ")",
                       canary::net::join_failure_label(kFailures[f]),
                       canary::net::join_failure_label_narrow(kFailures[f]),
                       c.title_off, true, false};
        out.push_back(l);
      }
    }
    if (c.body.full == nullptr) {
      SceneLine b = {n + " body (a network name)", "HomeNet", nullptr,
                     c.body_off, false, true};
      out.push_back(b);
    } else {
      CHECK(c.body.full[0] != '\0', "%s has an empty body", n.c_str());
      SceneLine b = {n + " body", c.body.full, c.body.narrow, c.body_off,
                     false, false};
      out.push_back(b);
    }
  }
  return out;
}

// F65 on small glass: each scene's title and body reads whole — one of its
// forms, uncut, in the line's face or the floor face — in the width the
// glass fits it to (scene_line_w); and the Join title on its row. The boxes
// the labels take are appended to `boxes` for the halo check.
static void check_scenes(const Env& e, int which, const Stack& s,
                         const Rows& r, const Glass& g, const Ring& ring,
                         int fam, std::vector<Box>* boxes) {
  const char* n = e.name.c_str();
  const char* lad_name = which == 0 ? "default" : "heirloom";
  const Face* t_own = face_of(g_ladder[fam][which][kBody]);
  const Face* t_floor = face_of(g_ladder[fam][0][kBody]);
  const Face* b_own = face_of(g_ladder[fam][which][kCaption]);
  const Face* b_floor = face_of(g_ladder[fam][0][kCaption]);
  CHECK(t_own && t_floor && b_own && b_floor,
        "%s/%s: a scene face is not in montserrat_metrics.h (re-run "
        "gen_montserrat_metrics.py)", n, lad_name);
  if (!t_own || !t_floor || !b_own || !b_floor) return;
  const Measure tm = {t_own, t_floor};
  const Measure bm = {b_own, b_floor};
  std::string summary;
  const std::vector<SceneLine> lines = scene_lines();
  for (size_t i = 0; i < lines.size(); ++i) {
    const SceneLine& sl = lines[i];
    const Measure& m = sl.title ? tm : bm;
    const int h = montserrat_line_h(sl.title ? t_own->size : b_own->size);
    const int y_top = e.h / 2 + sl.off - h / 2;
    const int w = scene_line_w(g, ring, y_top, h);
    Line line;
    if (sl.name) {
      name_line(line, sl.full, w, m);
    } else {
      const char* forms[2] = {sl.full, sl.narrow};
      fit_line(line, forms, 2, w, m);
    }
    const bool whole = std::strcmp(line.text, sl.full) == 0;
    const bool narrow = sl.narrow != nullptr &&
                        std::strcmp(line.text, sl.narrow) == 0;
    CHECK(w > 0 && line.fits && !line.cut && (whole || narrow) &&
              m(line.text, line.floor) <= w,
          "%s/%s: %s: \"%s\" (%s face, %d px) on a %d px line — not one of "
          "\"%s\" / \"%s\" read whole", n, lad_name, sl.what.c_str(),
          line.text, face_name(line.floor), m(line.text, line.floor), w,
          sl.full, sl.narrow ? sl.narrow : "");
    // The label's box: LV_ALIGN_CENTER of a w x (chosen face) label.
    const int hc = montserrat_line_h(
        line.floor ? (sl.title ? t_floor->size : b_floor->size)
                   : (sl.title ? t_own->size : b_own->size));
    Box b = {sl.what, e.w / 2 - w / 2, e.h / 2 - hc / 2 + sl.off, w, hc};
    boxes->push_back(b);
    if (!whole || line.floor) {
      char one[160];
      std::snprintf(one, sizeof(one), "%s%s -> \"%s\"%s", summary.empty() ? "" : ", ",
                    sl.what.c_str(), line.text, line.floor ? " (floor)" : "");
      summary += one;
    }
  }
  // The widest name a network can have (32 bytes, every one 'W'): cut
  // around "...", never past its line (name_line).
  {
    const SceneCopy c = scene_copy(ObStage::Connecting);
    const int h = montserrat_line_h(b_own->size);
    const int w = scene_line_w(g, ring, e.h / 2 + c.body_off - h / 2, h);
    const std::string widest(32, 'W');
    Line line;
    name_line(line, widest.c_str(), w, bm);
    CHECK(line.fits && bm(line.text, line.floor) <= w,
          "%s/%s: a 32-byte network name is \"%s\" (%d px) on a %d px line",
          n, lad_name, line.text, bm(line.text, line.floor), w);
  }
  // The Join title, on the stack's title row (set_join_title).
  const int dia = e.w < e.h ? e.w : e.h;
  const int y0 = (e.h - dia) / 2;
  const int title_w = g.round ? band_chord_px(dia, s.title_top - y0, r.title_h)
                              : e.w - 2 * roundframe::kRectSidePad;
  for (int qr = 0; qr < 2; ++qr) {
    const char* forms[1] = {join_title(qr != 0)};
    Line line;
    fit_line(line, forms, 1, title_w, tm);
    CHECK(line.fits && std::strcmp(line.text, forms[0]) == 0,
          "%s/%s: the Join title \"%s\" is cut on its %d px row", n,
          lad_name, forms[0], title_w);
    if (line.floor) {
      char one[160];
      std::snprintf(one, sizeof(one), "%sJoin title -> \"%s\" (floor)",
                    summary.empty() ? "" : ", ", line.text);
      summary += one;
    }
  }
  std::printf("  %20s scenes: %s\n", "",
              summary.empty() ? "every line whole in its own face"
                              : summary.c_str());
}

// Where a box sits against a halo of diameter d whose top is at `top`, on a
// panel w px wide (the arc is lv_obj_align'ed TOP_MID): 1 wholly inside the
// stroke's inner circle, 2 wholly outside its outer circle, 0 across it.
static int ring_side(int w, int d, int top, const Box& b) {
  const double cx = (w / 2 - d / 2) + d / 2.0;
  const double cy = top + d / 2.0;
  const double r_out = d / 2.0;
  const double r_in = r_out - kRingStroke;
  const double x0 = b.x0, x1 = b.x0 + b.w, ya = b.y0, yb = b.y0 + b.h;
  double far2 = 0;
  const double xs[2] = {x0, x1}, ys[2] = {ya, yb};
  for (int a = 0; a < 2; ++a) {
    for (int c = 0; c < 2; ++c) {
      const double d2 =
          (xs[a] - cx) * (xs[a] - cx) + (ys[c] - cy) * (ys[c] - cy);
      if (d2 > far2) far2 = d2;
    }
  }
  const double nx = cx < x0 ? x0 : cx > x1 ? x1 : cx;
  const double ny = cy < ya ? ya : cy > yb ? yb : cy;
  const double near2 = (nx - cx) * (nx - cx) + (ny - cy) * (ny - cy);
  if (far2 <= r_in * r_in) return 1;
  if (near2 >= r_out * r_out) return 2;
  return 0;
}

// F66: no row of any scene, nor the bird at either of its seats, crosses the
// halo's stroke on small glass: each is wholly inside the stroke's inner
// circle or wholly outside its outer one.
static void check_ring(const Env& e, int which, const Stack& s,
                       const Rows& r, const Glass& g, const Ring& ring,
                       std::vector<Box> boxes) {
  const char* n = e.name.c_str();
  const char* lad_name = which == 0 ? "default" : "heirloom";
  // The arc object: lv_obj_align(TOP_MID, 0, top) of a d x d box.
  const double cx = (e.w / 2 - ring.d / 2) + ring.d / 2.0;
  const double cy = ring.top + ring.d / 2.0;
  const double r_out = ring.d / 2.0;
  const double r_in = r_out - kRingStroke;
  CHECK(ring.d > 2 * kRingStroke, "%s/%s: a %d px halo", n, lad_name,
        ring.d);
  // The Join stack's rows, as the glass fits them.
  const int dia = e.w < e.h ? e.w : e.h;
  const int y0 = (e.h - dia) / 2;
  struct Row {
    const char* what;
    int top, h;
  };
  const Row rows[] = {{"the Join title row", s.title_top, r.title_h},
                      {"the credentials row", s.creds_top, r.creds_h},
                      {"the hint row", s.hint_top, r.hint_h},
                      {"the note row", s.note_top, r.hint_h}};
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
    const int w = g.round ? band_chord_px(dia, rows[i].top - y0, rows[i].h)
                          : e.w - 2 * roundframe::kRectSidePad;
    Box b = {rows[i].what, e.w / 2 - w / 2, rows[i].top, w, rows[i].h};
    boxes.push_back(b);
  }
  // The bird, breathing (+-2 px) at each seat: the scenes' and the Join
  // scene's while the QR is away.
  const int bird = g_minted.bird_px;
  const int seats[2] = {scene_bird_top(g, ring, bird),
                        join_bird_top(s, bird)};
  // The seat is CENTER kSceneBirdOff unless the halo needs it lower, and
  // then by no more than a few px.
  const int nominal = e.h / 2 - bird / 2 + kSceneBirdOff;
  CHECK(seats[0] >= nominal && seats[0] <= nominal + 4,
        "%s/%s: the bird's seat moved from %d to %d", n, lad_name, nominal,
        seats[0]);
  const char* seat_name[2] = {"the bird's seat", "the bird's Join seat"};
  for (int k = 0; k < 2; ++k) {
    for (int bob = -2; bob <= 2; bob += 4) {
      Box b = {seat_name[k], e.w / 2 - bird / 2, seats[k] + bob, bird, bird};
      boxes.push_back(b);
    }
  }
  int inside = 0, outside = 0;
  for (size_t i = 0; i < boxes.size(); ++i) {
    const Box& b = boxes[i];
    const int side = ring_side(e.w, ring.d, ring.top, b);
    if (side == 1) inside++;
    if (side == 2) outside++;
    CHECK(side != 0,
          "%s/%s: %s (%d..%d x %d..%d) crosses the halo's stroke (center "
          "%.1f,%.1f, radius %.1f..%.1f)", n, lad_name, b.what.c_str(),
          b.x0, b.x0 + b.w, b.y0, b.y0 + b.h, cx, cy, r_in, r_out);
  }
  std::printf("  %20s halo %3d px at y %3d..%3d: %d boxes inside, %d outside;"
              " bird at %d%s\n", "", ring.d, ring.top, ring.top + ring.d,
              inside, outside, seats[0],
              seats[0] == nominal ? "" : " (nudged into the halo)");
}

static void check_glass(const Env& e, int which, int also) {
  // onboard_ui.cpp: the nightstand renders through the watch branch, and
  // circular glass is the watch that is not a nightstand (RF_GLASS_ROUND).
  const bool small = e.watch || e.nightstand;
  const bool round = e.watch && !e.nightstand;
  // character.cpp's #if ladder: dash or AMOLED, else lean, else standard.
  const int fam = (e.dash || e.amoled) ? kBig : e.lean ? kLean : kStd;
  const int* lad = g_ladder[fam][which];
  // The Join scene's fonts, as onboard_ui_create() picks them per branch.
  const int title_f = small ? lad[kBody] : lad[kTitle];
  const int creds_f = small ? lad[kCaption] : lad[kLabel];
  const int hint_f = lad[kCaption];

  Glass g;
  g.w = e.w;
  g.h = e.h;
  g.round = round;
  Rows r;
  r.title_h = montserrat_line_h(title_f);
  r.card = small ? kSmallGlassCard : kWideGlassCard;
  r.creds_h = montserrat_line_h(creds_f);
  r.hint_h = montserrat_line_h(hint_f);
  const Stack s = join_stack(g, r);

  const char* lad_name = which == 0 ? "default" : "heirloom";
  char who[64];
  std::snprintf(who, sizeof(who), also ? "%s +%d" : "%s", e.name.c_str() + 15,
                also);
  std::printf("  %-18s %3dx%-3d %s %-8s title %3d  card %3d..%3d (qr %3d)  "
              "creds %3d  hint %3d..%3d  note %3d\n",
              who, e.w, e.h, round ? "round" : "rect ", lad_name, s.title_top,
              s.card_top, s.card_top + s.card, s.qr, s.creds_top, s.hint_top,
              s.hint_top + r.hint_h, s.note_top);
  const char* n = e.name.c_str();
  CHECK(s.fits, "%s/%s: the stack does not fit its window", n, lad_name);
  CHECK(s.title_top >= 0, "%s/%s: title above the glass", n, lad_name);
  CHECK(s.title_top + r.title_h + kMinGap <= s.card_top,
        "%s/%s: the title crosses the QR card", n, lad_name);
  CHECK(s.card_top + s.card + kMinGap <= s.creds_top,
        "%s/%s: the credentials line crosses the QR card", n, lad_name);
  CHECK(s.creds_top + r.creds_h <= s.hint_top,
        "%s/%s: the hint line crosses the credentials line", n, lad_name);
  CHECK(s.hint_top + r.hint_h <= e.h, "%s/%s: the hint runs off the glass",
        n, lad_name);
  CHECK(s.card <= e.w, "%s/%s: the card is wider than the glass", n, lad_name);
  CHECK(s.card == s.qr + 2 * r.card.pad, "%s/%s: the card lost its pad", n,
        lad_name);
  CHECK(s.qr <= r.card.qr && s.qr >= qr_floor(r.card.qr),
        "%s/%s: QR canvas %d outside [%d, %d]", n, lad_name, s.qr,
        qr_floor(r.card.qr), r.card.qr);
  CHECK(s.qr / kJoinQrModules == r.card.qr / kJoinQrModules,
        "%s/%s: the join code's module pitch changed", n, lad_name);
  // The note row (a standing hint on split small glass, onboard_ui's
  // s_note): the title's band on round glass, the row under the hint row
  // on rectangular glass — never crossing a row, never off the glass.
  if (round) {
    CHECK(s.note_top == s.title_top, "%s/%s: round note row at %d, title at %d",
          n, lad_name, s.note_top, s.title_top);
  } else if (small) {
    CHECK(s.note_top >= s.hint_top + r.hint_h,
          "%s/%s: the note row crosses the hint line", n, lad_name);
    CHECK(s.note_top + r.hint_h + kMinGap <= e.h,
          "%s/%s: the note row (%d..%d) leaves the %d px glass", n, lad_name,
          s.note_top, s.note_top + r.hint_h, e.h);
  }
  if (round) {
    const int dia = e.w < e.h ? e.w : e.h;
    // The card's corners stay inside the disc (house rim margin).
    CHECK(band_chord_px(dia, s.card_top, s.card) >= s.card,
          "%s/%s: the card's corners leave the round glass", n, lad_name);
    // Every line keeps the low-row budget (the window is symmetric, so the
    // title's band is as wide as the hint's).
    CHECK(band_chord_px(dia, s.hint_top, r.hint_h) >= kRoundLowRowW,
          "%s/%s: hint band chord %d < %d", n, lad_name,
          band_chord_px(dia, s.hint_top, r.hint_h), kRoundLowRowW);
    CHECK(band_chord_px(dia, s.creds_top, r.creds_h) >= kRoundLowRowW,
          "%s/%s: credentials band too narrow", n, lad_name);
    CHECK(band_chord_px(dia, s.title_top, r.title_h) >= kRoundLowRowW,
          "%s/%s: title band too narrow", n, lad_name);
    CHECK(band_chord_px(dia, s.note_top, r.hint_h) >= kRoundLowRowW,
          "%s/%s: note band too narrow", n, lad_name);
  } else {
    // Rectangular glass: the stack sits centered (the hint line reserved).
    const int top = s.title_top;
    const int bottom = e.h - (s.hint_top + r.hint_h);
    CHECK(top - bottom <= 1 && bottom - top <= 1,
          "%s/%s: stack off center (top %d, bottom %d)", n, lad_name, top,
          bottom);
    // ...and the card keeps the same air above and below it.
    const int above = s.card_top - (s.title_top + r.title_h);
    const int below = s.creds_top - (s.card_top + s.card);
    CHECK(above == below, "%s/%s: card gaps %d above, %d below", n, lad_name,
          above, below);
  }
  if (small) {
    // F50: the bird's Join-scene seat (the hidden card's, while no QR is
    // up — onboard_ui's join_bird_top) stays clear of the title band above
    // it and the credentials line under it, on every glass.
    const int bird = g_minted.bird_px;
    const int seat = join_bird_top(s, bird);
    CHECK(bird > 0 && bird <= s.card,
          "%s/%s: the bird (%d px) no longer fits the card's seat (%d px)",
          n, lad_name, bird, s.card);
    CHECK(seat >= s.title_top + r.title_h && seat + bird <= s.creds_top,
          "%s/%s: the bird's seat (%d..%d) crosses the title (..%d) or the "
          "credentials (%d..)", n, lad_name, seat, seat + bird,
          s.title_top + r.title_h, s.creds_top);
    check_rows(e, which, s, r, round, fam);
    // F65 and F66: every scene's lines read whole, and nothing crosses the
    // halo.
    const Ring ring = halo_ring(g, s, r);
    std::vector<Box> boxes;
    check_scenes(e, which, s, r, g, ring, fam, &boxes);
    check_ring(e, which, s, r, g, ring, boxes);
  } else {
    check_wide_rows(e, which, fam);
  }
}

// ── the two F43 panels, pinned (default ladder) ───────────────────────────
static void test_f43_pins() {
  std::printf("F43 panels, pinned:\n");
  // 800x480 dash: title 36 (40), card 208+2*12, label 20 (22), caption 16
  // (18). Air 168 -> 42 per share: the card sits dead center (124..356,
  // concentric with the 300 px halo) and the "or join" line starts 42 px
  // below it, where it used to start 14 px inside it.
  Glass dash = {800, 480, false};
  Rows dr = {40, kWideGlassCard, 22, 18};
  Stack d = join_stack(dash, dr);
  CHECK(d.fits && d.title_top == 42 && d.card_top == 124 && d.card == 232 &&
            d.qr == 208 && d.creds_top == 398 && d.hint_top == 420,
        "dash stack %d/%d/%d/%d/%d", d.title_top, d.card_top, d.card,
        d.creds_top, d.hint_top);
  // 240 round watch: body 16 (18), caption 12 (15). The low line's band is
  // 195..210 (chord 142 — the one provision.cpp's round hints are measured
  // against), mirrored to 30 above: card 50..178, network name from 180.
  Glass watch = {240, 240, true};
  Rows wr = {18, kSmallGlassCard, 15, 15};
  Stack w = join_stack(watch, wr);
  CHECK(w.fits && w.title_top == 30 && w.card_top == 50 && w.card == 128 &&
            w.qr == 112 && w.creds_top == 180 && w.hint_top == 195 &&
            w.note_top == 30,
        "watch stack %d/%d/%d/%d/%d/%d", w.title_top, w.card_top, w.card,
        w.creds_top, w.hint_top, w.note_top);
  CHECK(round_low_row_bottom(240, 15, kRoundLowRowW) == 210,
        "low row bottom %d", round_low_row_bottom(240, 15, kRoundLowRowW));
  CHECK(band_chord_px(240, 195, 15) >= kRoundLowRowW &&
            band_chord_px(240, 196, 15) < kRoundLowRowW,
        "210 is not the LOWEST band holding the budget");
}

// ── the F45 glass, pinned (LVGL's metrics, the real hint copy) ──────────
static void test_f45_pins() {
  std::printf("F45 rows, pinned:\n");
  const Face* f12 = face_of(12);
  const Face* f14 = face_of(14);
  if (f12 == nullptr || f14 == nullptr || g_minted.hint_small.size() != 2 ||
      g_minted.hint_round.size() != 1) {
    CHECK(false, "F45 pins need montserrat_12/14 and both portrait hints");
    return;
  }
  const char* ssid = "SecuraCV-A7K2";
  const char* key = "p7Rm2Kqf";
  const char* hint = g_minted.hint_small[0].c_str();
  const char* narrow = g_minted.hint_small[1].c_str();
  const int ns_row = 172 - 2 * roundframe::kRectSidePad;  // 156
  const Measure std12 = {f12, f12};
  // The defect: the joined line is wider than the nightstand's row.
  char joined[kLineCap];
  std::snprintf(joined, sizeof(joined), kJoinedFmt, ssid, key);
  CHECK(ns_row == 156 && text_px(*f12, joined) == 174,
        "nightstand joined line %d px on %d", text_px(*f12, joined), ns_row);
  CHECK(text_px(*f12, hint) == 205 && text_px(*f12, narrow) == 140,
        "portrait hints %d / %d px", text_px(*f12, hint),
        text_px(*f12, narrow));
  // 172 px nightstand, default ladder: split, name then key; the note row
  // is empty.
  JoinLines j =
      join_lines(false, ns_row, ns_row, ns_row, ssid, key, "", nullptr, std12);
  CHECK(j.split && std::strcmp(j.creds.text, ssid) == 0 &&
            std::strcmp(j.low.text, "pass  p7Rm2Kqf") == 0 && !j.creds.floor &&
            !j.low.floor && j.note.text[0] == '\0',
        "nightstand rows \"%s\" | \"%s\" | \"%s\"", j.creds.text,
        j.low.text, j.note.text);
  // ...and 45 s on, with the stuck-phone hint up (the review's case: it
  // used to take the key's row, so a phone told to forget the network lost
  // the key it needs to rejoin): the name and the key keep their rows, and
  // the hint's narrow form takes the note row in the row's own face.
  j = join_lines(false, ns_row, ns_row, ns_row, ssid, key, hint, narrow, std12);
  CHECK(j.split && std::strcmp(j.creds.text, ssid) == 0 &&
            std::strcmp(j.low.text, "pass  p7Rm2Kqf") == 0 &&
            std::strcmp(j.note.text, narrow) == 0 && !j.note.floor,
        "nightstand with the hint \"%s\" | \"%s\" | \"%s\"", j.creds.text,
        j.low.text, j.note.text);
  // 240 px touch169: the joined line fits (174 <= 224) and stays; the whole
  // hint fits too, on the row under it.
  j = join_lines(false, 224, 224, 224, ssid, key, hint, narrow, std12);
  CHECK(!j.split && std::strcmp(j.creds.text, joined) == 0 &&
            std::strcmp(j.low.text, hint) == 0 && j.note.text[0] == '\0',
        "touch169 rows \"%s\" | \"%s\"", j.creds.text, j.low.text);
  // ...but the widest unit splits there, and the key still keeps its row.
  j = join_lines(false, 224, 224, 224, "SecuraCV-WWWW", "WWWWWWWW", hint,
                 narrow, std12);
  CHECK(j.split && std::strcmp(j.creds.text, "SecuraCV-WWWW") == 0 &&
            std::strstr(j.low.text, "WWWWWWWW") != nullptr &&
            std::strcmp(j.note.text, hint) == 0,
        "touch169 widest unit with the hint \"%s\" | \"%s\" | \"%s\"",
        j.creds.text, j.low.text, j.note.text);
  // Heirloom on the nightstand (14 px caption): neither hint form fits in
  // 14 px, so the narrow one steps down to the default Character's 12 px.
  const Measure heir = {f14, f12};
  j = join_lines(false, ns_row, ns_row, ns_row, ssid, key, hint, narrow, heir);
  CHECK(j.note.fits && j.note.floor && std::strcmp(j.note.text, narrow) == 0 &&
            std::strstr(j.low.text, key) != nullptr,
        "heirloom nightstand hint \"%s\" (%s), key row \"%s\"", j.note.text,
        face_name(j.note.floor), j.low.text);
  // The round watch: the hint used to take the password's low band; now the
  // title's band (the same 142 px) carries it and "pass  <key>" stays.
  const Measure watch12 = {f12, f12};
  j = join_lines(true, 174, kRoundLowRowW, kRoundLowRowW, ssid, key,
                 g_minted.hint_round[0].c_str(), nullptr, watch12);
  CHECK(j.split && std::strcmp(j.low.text, "pass  p7Rm2Kqf") == 0 &&
            std::strcmp(j.note.text, g_minted.hint_round[0].c_str()) == 0,
        "round watch with the hint \"%s\" | \"%s\"", j.low.text,
        j.note.text);
  // A key too wide for "pass  " drops the label before the face: 8 x 'W'
  // on the round watch's 142 px low band.
  j = join_lines(true, 174, kRoundLowRowW, kRoundLowRowW, ssid, "WWWWWWWW", "",
                 nullptr, watch12);
  CHECK(j.split && std::strcmp(j.low.text, "WWWWWWWW") == 0 && !j.low.floor,
        "wide key on round glass \"%s\"", j.low.text);
  // And the degenerate case says so rather than claiming a fit.
  j = join_lines(false, 40, 40, 40, ssid, key, hint, narrow, std12);
  CHECK(!j.creds.fits && !j.low.fits && !j.note.fits,
        "a 40 px row claimed to fit");
}

// ── the F50 glass, pinned (LVGL's metrics, the real hint copy) ──────────
static void test_f50_pins() {
  std::printf("F50 coach lines, pinned:\n");
  using canary::net::JoinFailure;
  using canary::net::join_failure_hint;
  using canary::net::join_failure_hint_narrow;
  const Face* f12 = face_of(12);
  const Face* f14 = face_of(14);
  if (f12 == nullptr || f14 == nullptr || g_minted.phone_small.size() != 2) {
    CHECK(false, "F50 pins need montserrat_12/14 and both PhoneJoined forms");
    return;
  }
  const char* phone = g_minted.phone_small[0].c_str();
  const char* phone_narrow = g_minted.phone_small[1].c_str();
  // The defect: every Fail hint is 175-219 px at 12 px, wider than the
  // round watch's 142 px band and the 156/164 px portrait rows, and the
  // PhoneJoined hint is 182 px in Heirloom's 14 px caption. One row, whole,
  // in the row's face, and LONG_DOT cut every one of them.
  int lo = 1 << 30, hi = 0;
  for (int i = 0; i < kFailureCount; i++) {
    const int w = text_px(*f12, join_failure_hint(kFailures[i]));
    if (w < lo) lo = w;
    if (w > hi) hi = w;
  }
  CHECK(lo == 175 && hi == 219 &&
            text_px(*f12, join_failure_hint(JoinFailure::NoAddress)) == 219,
        "Fail hints %d..%d px at 12 px", lo, hi);
  CHECK(text_px(*f14, phone) == 182, "PhoneJoined hint %d px at 14 px",
        text_px(*f14, phone));
  const Measure std12 = {f12, f12};
  const Measure heir = {f14, f12};
  // The round watch (the credentials band 174 px over the 142 px low band):
  // the whole hint over both rows, broken where the halves are most even
  // ("your router may be" over "out of addresses", 117/99 px, ties "your
  // router may" over "be out of addresses", 99/117: the later break wins,
  // the head on the wider upper row) — and never between a number and its
  // unit: "it only sees 2.4" over "GHz wifi - not 5" (87/92) would be more
  // even than the break it takes (66/113).
  HintLines h = hint_lines(174, kRoundLowRowW,
                           join_failure_hint(JoinFailure::NoAddress),
                           join_failure_hint_narrow(JoinFailure::NoAddress),
                           std12);
  CHECK(h.split && std::strcmp(h.upper.text, "your router may be") == 0 &&
            std::strcmp(h.lower.text, "out of addresses") == 0 &&
            !h.upper.floor && !h.lower.floor,
        "round NoAddress \"%s\" | \"%s\"", h.upper.text, h.lower.text);
  h = hint_lines(174, kRoundLowRowW, join_failure_hint(JoinFailure::NotFound),
                 join_failure_hint_narrow(JoinFailure::NotFound), std12);
  CHECK(h.split && std::strcmp(h.upper.text, "it only sees") == 0 &&
            std::strcmp(h.lower.text, "2.4 GHz wifi - not 5") == 0,
        "round NotFound \"%s\" | \"%s\"", h.upper.text, h.lower.text);
  // A clause break wins over a more even one: "no page? open" over
  // "192.168.4.1" is 90/58 px, "no page?" over "open 192.168.4.1" 56/92 px.
  h = hint_lines(174, kRoundLowRowW, phone, phone_narrow, std12);
  CHECK(h.split && std::strcmp(h.upper.text, "no page?") == 0 &&
            std::strcmp(h.lower.text, "open 192.168.4.1") == 0,
        "round PhoneJoined \"%s\" | \"%s\"", h.upper.text, h.lower.text);
  // The 172 px nightstand (156 px rows): the PhoneJoined hint fits one row
  // at 12 px (151 px), so it stays whole there; Heirloom's 14 px (182)
  // takes both rows, still in 14 px.
  h = hint_lines(156, 156, phone, phone_narrow, std12);
  CHECK(!h.split && std::strcmp(h.lower.text, phone) == 0 &&
            h.upper.text[0] == '\0' && !h.lower.floor,
        "nightstand PhoneJoined \"%s\" | \"%s\"", h.upper.text, h.lower.text);
  h = hint_lines(156, 156, phone, phone_narrow, heir);
  CHECK(h.split && std::strcmp(h.upper.text, "no page?") == 0 &&
            std::strcmp(h.lower.text, "open 192.168.4.1") == 0 &&
            !h.upper.floor && !h.lower.floor,
        "heirloom nightstand PhoneJoined \"%s\" | \"%s\"", h.upper.text,
        h.lower.text);
  // An upper row too narrow to take any head: the narrow form, on the hint
  // row, in the row's own face — before any smaller face.
  h = hint_lines(40, kRoundLowRowW, join_failure_hint(JoinFailure::NoAddress),
                 join_failure_hint_narrow(JoinFailure::NoAddress), std12);
  CHECK(!h.split && h.upper.text[0] == '\0' &&
            std::strcmp(h.lower.text,
                        join_failure_hint_narrow(JoinFailure::NoAddress)) == 0 &&
            h.lower.fits && !h.lower.floor,
        "narrow rung \"%s\" (%s)", h.lower.text, face_name(h.lower.floor));
  // No hint: both rows empty. And the degenerate case says so rather than
  // claiming a fit.
  h = hint_lines(174, kRoundLowRowW, "", nullptr, std12);
  CHECK(!h.split && h.upper.text[0] == '\0' && h.lower.text[0] == '\0' &&
            h.upper.fits && h.lower.fits,
        "an empty coach line drew \"%s\" | \"%s\"", h.upper.text,
        h.lower.text);
  h = hint_lines(20, 20, join_failure_hint(JoinFailure::Unknown),
                 join_failure_hint_narrow(JoinFailure::Unknown), std12);
  CHECK(!h.lower.fits, "a 20 px row claimed to fit \"%s\"", h.lower.text);
  // The bird's Join seat (#1755): its old seat (a fixed -64 from the 240
  // disc's center) put its top at 36, inside the title band (30..48); the
  // card's seat starts at 94, under it (the F43 stack's card is 50..178).
  // The glass draws the bird at these numbers since F64 (canary_mark reads
  // the host's offset, not the laid-out box: test_canary_mark_seat).
  Glass watch = {240, 240, true};
  Rows wr = {18, kSmallGlassCard, 15, 15};
  Stack w = join_stack(watch, wr);
  const int old_top = 240 / 2 - 64 - g_minted.bird_px / 2;
  CHECK(old_top == 36 && old_top < w.title_top + wr.title_h,
        "the old seat (top %d) no longer documents the defect", old_top);
  CHECK(join_bird_top(w, g_minted.bird_px) == 94 &&
            join_bird_top(w, g_minted.bird_px) >= w.title_top + wr.title_h,
        "the round watch's bird seat is %d", join_bird_top(w, g_minted.bird_px));
}

// ── the F65 lines, pinned (LVGL's metrics, the real copy) ──────────────
static void test_f65_pins() {
  std::printf("F65 scene lines, pinned:\n");
  const Face* f12 = face_of(12);
  const Face* f14 = face_of(14);
  const Face* f16 = face_of(16);
  const Face* f20 = face_of(20);
  if (!f12 || !f14 || !f16 || !f20) {
    CHECK(false, "F65 pins need montserrat_12/14/16/20");
    return;
  }
  // The defect: the lines kept the width they were first fitted to. The
  // round watch's title, 138 px (fitted at y 28) in Hello and 142 px (the
  // Join title row) after it, and every portrait line, the panel less 16 px
  // (156 on the nightstand): these are the lines that measured wider.
  CHECK(band_chord_px(240, 28, 18) == 138 && band_chord_px(240, 30, 18) == 142,
        "round title widths %d / %d", band_chord_px(240, 28, 18),
        band_chord_px(240, 30, 18));
  const struct {
    const Face* f;
    const char* text;
    int px;
  } kOld[] = {
      {f16, "Nice - check your phone", 197},
      {f16, "No address from the router", 221},
      {f16, "Network not found", 154},
      {f20, "On your phone", 154},
      {f20, "Wrong password", 175},
      {f20, "Couldn't connect", 176},
      {f20, "That didn't work", 167},
      {f14, "Let's get you connected.", 179},
      {f14, "a setup page is opening", 176},
      {f14, "try again on your phone", 176},
      {f14, "looking for your canaries", 180},
  };
  for (size_t i = 0; i < sizeof(kOld) / sizeof(kOld[0]); ++i) {
    CHECK(text_px(*kOld[i].f, kOld[i].text) == kOld[i].px,
          "\"%s\" at %d px is %d px", kOld[i].text, kOld[i].f->size,
          text_px(*kOld[i].f, kOld[i].text));
  }
  // The fix, on the same glass: the round watch's PhoneJoined title is
  // fitted at its own latitude (95..113: 224 px), so the whole line holds;
  // on the nightstand's 156 px it takes the narrow form, still 16 px.
  Glass watch = {240, 240, true};
  Rows wr = {18, kSmallGlassCard, 15, 15};
  const Stack ws = join_stack(watch, wr);
  const Ring wring = halo_ring(watch, ws, wr);
  CHECK(wring.d == 236 && wring.top == 2, "round halo %d at %d", wring.d,
        wring.top);
  const int title_top = 120 + kSceneTitleOff - 9;
  CHECK(scene_line_w(watch, wring, title_top, 18) == 224,
        "round title line %d px", scene_line_w(watch, wring, title_top, 18));
  const Measure std16 = {f16, f16};
  const SceneCopy pj = scene_copy(ObStage::PhoneJoined);
  const char* pj_forms[2] = {pj.title.full, pj.title.narrow};
  Line line;
  fit_line(line, pj_forms, 2, 224, std16);
  CHECK(line.fits && !line.floor &&
            std::strcmp(line.text, "Nice - check your phone") == 0,
        "round PhoneJoined title \"%s\"", line.text);
  fit_line(line, pj_forms, 2, 156, std16);
  CHECK(line.fits && !line.floor &&
            std::strcmp(line.text, "Check your phone") == 0,
        "nightstand PhoneJoined title \"%s\"", line.text);
  // Heirloom on the nightstand: no form fits in 20 px, so the narrow one
  // steps down to the default Character's 16 px; a Fail reason with no
  // shorter true form keeps its words and steps down ("Wrong password",
  // 137 px at 16).
  const Measure heir_t = {f20, f16};
  fit_line(line, pj_forms, 2, 156, heir_t);
  CHECK(line.fits && line.floor &&
            std::strcmp(line.text, "Check your phone") == 0,
        "heirloom nightstand PhoneJoined title \"%s\" (%s)", line.text,
        face_name(line.floor));
  using canary::net::JoinFailure;
  const char* bad[2] = {
      canary::net::join_failure_label(JoinFailure::BadPassword),
      canary::net::join_failure_label_narrow(JoinFailure::BadPassword)};
  fit_line(line, bad, 2, 156, heir_t);
  CHECK(line.fits && line.floor &&
            std::strcmp(line.text, "Wrong password") == 0,
        "heirloom nightstand BadPassword title \"%s\"", line.text);
  const char* addr[2] = {
      canary::net::join_failure_label(JoinFailure::NoAddress),
      canary::net::join_failure_label_narrow(JoinFailure::NoAddress)};
  fit_line(line, addr, 2, 156, std16);
  CHECK(line.fits && std::strcmp(line.text, "No address") == 0,
        "nightstand NoAddress title \"%s\"", line.text);
}

// ── a network name that does not fit (name_line, F65) ─────────────────────
// A uniform measure: every code point 7 px in the row's face, 6 in the
// floor's, whatever the font carries (a name can hold any UTF-8).
struct Uniform {
  int operator()(const char* t, bool fl) const {
    int i = 0, n = 0;
    while (utf8_next(t, &i) != 0) ++n;
    return n * (fl ? 6 : 7);
  }
};

static void test_name_line() {
  std::printf("network names (name_line):\n");
  const Face* f12 = face_of(12);
  if (!f12) {
    CHECK(false, "name_line pins need montserrat_12");
    return;
  }
  const Measure std12 = {f12, f12};
  Line line;
  // A name that fits is the name, whole, in the row's face.
  name_line(line, "HomeNet", 156, std12);
  CHECK(line.fits && !line.cut && !line.floor &&
            std::strcmp(line.text, "HomeNet") == 0,
        "HomeNet -> \"%s\"", line.text);
  // One that fits only in the floor face is whole there.
  const Uniform u;
  name_line(line, "ABCDEFGHIJKLMNOPQRSTUVWX", 150, u);  // 168 / 144 px
  CHECK(line.fits && !line.cut && line.floor &&
            std::strcmp(line.text, "ABCDEFGHIJKLMNOPQRSTUVWX") == 0,
        "a floor-face name -> \"%s\"", line.text);
  // Wider than that: its head and its tail around "...", in the floor face
  // — the band suffix stays on the glass.
  const char* longname = "Kitchen-Mesh-Extender-Upstair-5G";  // 32 bytes
  name_line(line, longname, 156, std12);
  std::printf("  \"%s\" on 156 px -> \"%s\" (%d px)\n", longname, line.text,
              std12(line.text, true));
  CHECK(line.fits && line.cut && line.floor &&
            std::strcmp(line.text, "Kitchen-Mes...-Upstair-5G") == 0 &&
            std12(line.text, true) <= 156,
        "a long name -> \"%s\"", line.text);
  // The cut falls between code points (the old "%.28s" counted bytes).
  const char* utf = "Caf\xC3\xA9-\xC3\x9C" "ber-\xE2\x98\x95-Netz-\xC3\xB1\xC3\xB1\xC3\xB1";
  for (int row = 18; row <= 140; row += 1) {
    name_line(line, utf, row, u);
    int i = 0;
    bool broken = false;
    for (uint32_t cp = utf8_next(line.text, &i); cp != 0;
         cp = utf8_next(line.text, &i))
      if (cp == 0xFFFD) broken = true;
    CHECK(!broken && (line.cut ? u(line.text, true) <= row : true),
          "a %d px row cut \"%s\" inside a character: \"%s\"", row, utf,
          line.text);
  }
  // Nothing at all fits: it says so.
  CHECK(std12("...", true) == 9, "\"...\" is %d px", std12("...", true));
  name_line(line, longname, 8, std12);
  CHECK(!line.fits && line.cut, "an 8 px row claimed to fit \"%s\"",
        line.text);
}

// ── the F66 halo, pinned ────────────────────────────────────────────────
static void test_f66_pins() {
  std::printf("F66 halo, pinned:\n");
  // The defect: one 236 px ring, centered, on every small glass. On the
  // 172x320 portrait glass its bottom arc (y 275..278 at the center) runs
  // through the hint row (269..284); on the 240x280 touch169 (255..258)
  // through the hint row (239..254).
  const Box ns_hint = {"nightstand hint row", 8, 269, 156, 15};
  const Box t_hint = {"touch169 hint row", 8, 239, 224, 15};
  CHECK(ring_side(172, 236, (320 - 236) / 2, ns_hint) == 0 &&
            ring_side(240, 236, (280 - 236) / 2, t_hint) == 0,
        "the old 236 px ring no longer documents the crossing");
  // The fix: the band between the stack's title and credentials rows.
  // 172x320, default ladder: title 36..54, credentials from 254 -> a 196 px
  // halo from y 56 (concentric with the card, 90..218); 240x280: title
  // 26..44, credentials from 224 -> 176 px from y 46 (card 70..198).
  Glass ns = {172, 320, false};
  Rows nr = {18, kSmallGlassCard, 15, 15};
  const Stack nst = join_stack(ns, nr);
  const Ring nring = halo_ring(ns, nst, nr);
  CHECK(nring.d == 196 && nring.top == 56 &&
            nring.top + nring.d / 2 == nst.card_top + nst.card / 2,
        "nightstand halo %d at %d (card %d..%d)", nring.d, nring.top,
        nst.card_top, nst.card_top + nst.card);
  CHECK(ring_side(172, nring.d, nring.top, ns_hint) == 2,
        "the nightstand's hint row crosses its halo");
  Glass t = {240, 280, false};
  const Stack tst = join_stack(t, nr);
  const Ring tring = halo_ring(t, tst, nr);
  CHECK(tring.d == 176 && tring.top == 46,
        "touch169 halo %d at %d", tring.d, tring.top);
  const Box t_creds = {"touch169 credentials row", 8, 224, 224, 15};
  CHECK(ring_side(240, tring.d, tring.top, t_creds) == 2 &&
            ring_side(240, tring.d, tring.top, t_hint) == 2,
        "the touch169's low rows cross its halo");
}

// ── degenerate glass: the lines still never cross ─────────────────────────
static void test_short_glass() {
  std::printf("short glass:\n");
  Glass g = {240, 150, false};
  Rows r = {18, kSmallGlassCard, 15, 15};
  Stack s = join_stack(g, r);
  CHECK(!s.fits, "a 150 px glass cannot hold a 87 px QR and three lines");
  CHECK(s.qr == qr_floor(kSmallGlassCard.qr), "the QR shrank past its floor");
  CHECK(s.title_top + r.title_h + kMinGap <= s.card_top &&
            s.card_top + s.card + kMinGap <= s.creds_top &&
            s.creds_top + r.creds_h <= s.hint_top,
        "an overfull stack let two rows cross");
  // ...and the halo and the bird's seat stay sane on it: a ring no smaller
  // than the card it surrounds (the stack's gaps hold even here), and the
  // bird nudged by no more than half itself.
  {
    Glass tiny = {240, 60, false};
    const Stack ts = join_stack(tiny, r);
    const Ring ring = halo_ring(tiny, ts, r);
    CHECK(ring.d >= ts.card, "a %d px halo round a %d px card", ring.d,
          ts.card);
    const int seat = scene_bird_top(tiny, ring, 40);
    const int nominal = 60 / 2 - 20 + kSceneBirdOff;
    CHECK(seat >= nominal && seat <= nominal + 20,
          "the bird's seat ran to %d on a 60 px glass (authored %d)", seat,
          nominal);
  }
  // A round window a little short: the canvas gives up pixels, not pitch.
  Glass rg = {240, 240, true};
  Rows rr = {22, kSmallGlassCard, 16, 16};  // the heirloom watch
  Stack t = join_stack(rg, rr);
  CHECK(t.fits && t.qr < kSmallGlassCard.qr &&
            t.qr / kJoinQrModules == kSmallGlassCard.qr / kJoinQrModules,
        "heirloom watch qr %d", t.qr);
}

// ── the join code really is 29 modules ─────────────────────────────────────
static void test_join_payload() {
  std::printf("join payload:\n");
  // provision.cpp's AP: "SecuraCV-" + 4 pseudonym chars (+ "-" and a 2-char
  // tag when the password store refuses a durable key), AP_PASS_LEN 8.
  const char* ssids[] = {"SecuraCV-A7K2", "SecuraCV-A7K2-Xy"};
  for (int i = 0; i < 2; i++) {
    char payload[224];
    const size_t n =
        canary::net::wifi_qr_payload(ssids[i], "p7Rm2Kqf", payload, sizeof(payload));
    // Byte-mode capacity at ECC M: version 2 holds 26, version 3 holds 42.
    CHECK(n > 26 && n <= 42,
          "join payload for %s is %d bytes — not a version-3 code, so "
          "kJoinQrModules (29) is wrong", ssids[i], (int)n);
  }
}

int main() {
  load_ladders();
  load_minted();
  test_metrics_table();
  const std::vector<Env> envs = load_envs();
  // One glass per distinct (panel, config, lean) — the dash line alone is a
  // dozen envs on the same 800x480 panel and config.
  std::printf("join stacks (\"+N\": that many more envs, same glass):\n");
  for (size_t i = 0; i < envs.size(); i++) {
    bool seen = false;
    int also = 0;
    for (size_t j = 0; j < envs.size(); j++) {
      const bool same = envs[j].w == envs[i].w && envs[j].h == envs[i].h &&
                        envs[j].cfg == envs[i].cfg &&
                        envs[j].lean == envs[i].lean;
      if (same && j < i) seen = true;
      if (same && j > i) also++;
    }
    if (seen) continue;
    check_glass(envs[i], 0, also);
    check_glass(envs[i], 1, also);
  }
  test_f43_pins();
  test_f45_pins();
  test_f50_pins();
  test_f65_pins();
  test_name_line();
  test_f66_pins();
  test_short_glass();
  test_join_payload();
  if (g_fail == 0) {
    std::printf("ALL ONBOARD LAYOUT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
