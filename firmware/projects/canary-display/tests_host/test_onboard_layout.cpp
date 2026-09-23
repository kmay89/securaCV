// Host test for the Join scene's stack (canary/ui/onboard_layout.h): on every
// display panel the PlatformIO tree builds, with both type ladders, the
// first-boot title, QR card, credentials line and hint line never cross one
// another or leave the glass — the F43 defect was the dash's "or join ...
// password" line drawn 14 px inside the QR card (the round watch's network
// name, 7 px), with each glass placing card and captions by its own
// literals.
//
// Nothing about the glass is retyped here. The panels come from each env's
// board pins.h via envs/platformio/canary-display.ini; the flavor bits
// (dash / nightstand / watch / AMOLED, and the lean build) from the env's
// config.h and flags; the font sizes from character.cpp's type ladders
// (parsed in order, role by role); only the Montserrat line heights are a
// table — they are LVGL's font data, not ours.
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
#include "network/provision_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

// ── the stack's invariants on one glass ───────────────────────────────────
static int band_chord_px(int dia, int top, int h) {
  return roundframe::band_chord(dia, roundframe::kEdgeMargin, top, h);
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
              "creds %3d  hint %3d..%3d\n",
              who, e.w, e.h, round ? "round" : "rect ", lad_name, s.title_top,
              s.card_top, s.card_top + s.card, s.qr, s.creds_top, s.hint_top,
              s.hint_top + r.hint_h);
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
            w.qr == 112 && w.creds_top == 180 && w.hint_top == 195,
        "watch stack %d/%d/%d/%d/%d", w.title_top, w.card_top, w.card,
        w.creds_top, w.hint_top);
  CHECK(round_low_row_bottom(240, 15, kRoundLowRowW) == 210,
        "low row bottom %d", round_low_row_bottom(240, 15, kRoundLowRowW));
  CHECK(band_chord_px(240, 195, 15) >= kRoundLowRowW &&
            band_chord_px(240, 196, 15) < kRoundLowRowW,
        "210 is not the LOWEST band holding the budget");
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
  test_short_glass();
  test_join_payload();
  if (g_fail == 0) {
    std::printf("ALL ONBOARD LAYOUT TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_fail);
  return 1;
}
