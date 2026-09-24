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
  int branch = -1;
  bool armed = false, done = false;
  std::string body[3];
  for (size_t i = 0; i < ls.size() && !done; i++) {
    const std::string t = trim(ls[i]);
    if (t == "ctx.stuck_hinted = true;") {
      armed = true;
    } else if (armed && branch < 0 &&
               t == "#if defined(CD_FLAVOR_WATCH) && "
                    "!defined(CD_FLAVOR_NIGHTSTAND)") {
      branch = 0;
    } else if (branch == 0 && t == "#elif defined(CD_FLAVOR_WATCH)") {
      branch = 1;
    } else if (branch == 1 && t == "#else") {
      branch = 2;
    } else if (branch == 2 && t == "#endif") {
      done = true;
    } else if (branch >= 0 && !starts_with(t, "//")) {
      body[branch] += t + "\n";
    }
  }
  CHECK(done, "provision.cpp's stuck-phone hint branches (round / portrait / "
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
  branch = -1;
  armed = false;
  done = false;
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
  std::printf("  %20s row %3d  credentials <= %3d px  hint %3d px  coach "
              "lines <= %3d px\n", "", row, widest, hint_w, coach_w);
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
    check_rows(e, which, s, r, round, fam);
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
  test_short_glass();
  test_join_payload();
  if (g_fail == 0) {
    std::printf("ALL ONBOARD LAYOUT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
