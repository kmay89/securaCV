/* Host tests for the console scene engine + randomart (firmware/common/ui).
 *
 * Proves the two properties that make a themed serial console safe rather than
 * fragile:
 *   1. RANDOMART is a faithful, deterministic drunken-bishop walk — the trust
 *      centrepiece must be stable and bounded.
 *   2. The ASCII tier NEVER emits an escape byte or a non-7-bit byte, and every
 *      framed line aligns to the same width — so on an unknown terminal the
 *      banner degrades to clean text instead of `^[[..m` garbage (which our own
 *      flasher would flag as wrong-baud).
 *
 * Build & run (CI firmware host tests):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common \
 *       test_console_scene.cpp -o /tmp/t && /tmp/t
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/randomart.h"
#include "ui/console_theme.h"
#include "ui/console_scenes.h"
#include "ui/console_wake.h"

using namespace scene;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

// ── a sink that collects into a std::string ─────────────────────────────────
struct Sink { std::string s; };
static void collect(void* ctx, const char* str) { ((Sink*)ctx)->s += str; }

static std::vector<std::string> split_crlf(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find("\r\n", i);
    if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
    out.push_back(s.substr(i, j - i));
    i = j + 2;
  }
  return out;
}

static const uint8_t KEY_A[8] = { 0x9b, 0x2c, 0xff, 0x01, 0x44, 0xa0, 0x7e, 0x33 };
static const uint8_t KEY_B[8] = { 0x9b, 0x2c, 0xff, 0x01, 0x44, 0xa0, 0x7e, 0x32 }; // 1 bit diff

static TrustInfo sample(const uint8_t* key, size_t len) {
  TrustInfo t{};
  t.device_id = "canary-7fA3";
  t.firmware = "2.2.0";
  t.git = "abc1234";
  t.built = "2026-07-21";
  t.chain_head_hex = "3f9ac10b";
  t.key_fp_hex = "9b2cff0144a07e33";
  t.seq = 41; t.boots = 12; t.health = 100; t.tamper = false;
  t.key_bytes = key; t.key_len = len;
  return t;
}

// ── randomart ────────────────────────────────────────────────────────────────
static void test_randomart_walk() {
  uint8_t f1[RANDOMART_H][RANDOMART_W], f2[RANDOMART_H][RANDOMART_W];
  randomart_field(KEY_A, 8, f1);
  randomart_field(KEY_A, 8, f2);
  // Deterministic.
  CHECK(memcmp(f1, f2, sizeof f1) == 0);

  // Start marker at the centre; End marker exists exactly once elsewhere-or-not.
  CHECK(f1[RANDOMART_H / 2][RANDOMART_W / 2] == (uint8_t)(RANDOMART_LEN - 1)); // 'S'
  int ends = 0, starts = 0;
  for (int y = 0; y < RANDOMART_H; ++y)
    for (int x = 0; x < RANDOMART_W; ++x) {
      if (f1[y][x] == (uint8_t)RANDOMART_LEN) ends++;
      if (f1[y][x] == (uint8_t)(RANDOMART_LEN - 1)) starts++;
      // every value maps into the ramp
      CHECK(randomart_glyph(f1[y][x]) != '\0');
    }
  CHECK(starts == 1);
  CHECK(ends == 1);

  // A one-bit change in the key visibly changes the art.
  uint8_t fb[RANDOMART_H][RANDOMART_W];
  randomart_field(KEY_B, 8, fb);
  CHECK(memcmp(f1, fb, sizeof f1) != 0);

  // Glyph ramp: 0=space, 1='.', markers 'S'/'E'.
  CHECK(randomart_glyph(0) == ' ');
  CHECK(randomart_glyph(1) == '.');
  CHECK(randomart_glyph(RANDOMART_LEN - 1) == 'S');
  CHECK(randomart_glyph(RANDOMART_LEN) == 'E');
  CHECK(randomart_glyph(200) == 'E'); // clamped, never out of bounds
}

// Trailing-space-insensitive compare: the row is always RANDOMART_W wide, so we
// pin the significant (non-trailing-space) prefix and the fixed width.
static std::string rstrip(const std::string& s) {
  size_t e = s.size();
  while (e > 0 && s[e - 1] == ' ') --e;
  return s.substr(0, e);
}

static void test_randomart_golden() {
  // The GOLDEN vector the website pins in tests/randomart.test.mjs. Key = the
  // 32 bytes 0x00..0x1f. If this shape changes here, the browser-drawn randomart
  // (js/randomart.js) stops matching what the device draws — the whole trust
  // handshake breaks — so both repos assert this identical output.
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
  uint8_t f[RANDOMART_H][RANDOMART_W];
  randomart_field(key, 32, f);

  static const char* const prefix[RANDOMART_H] = {
    "^^O@@E.", "@@O++..", "o+.. ..", "       .", "        S", "", "", "", ""
  };
  for (int y = 0; y < RANDOMART_H; ++y) {
    char row[RANDOMART_W + 1];
    randomart_row(f, y, row);
    CHECK(std::strlen(row) == (size_t)RANDOMART_W);   // always the full width
    CHECK(rstrip(row) == prefix[y]);                  // ...and the exact shape
  }
}

static void test_randomart_all_zero() {
  // All-zero bytes: every step is up-left → the bishop clamps into the top-left
  // corner. Start stays centred; End lands at (0,0).
  uint8_t z[16]; memset(z, 0, sizeof z);
  uint8_t f[RANDOMART_H][RANDOMART_W];
  randomart_field(z, sizeof z, f);
  CHECK(f[RANDOMART_H / 2][RANDOMART_W / 2] == (uint8_t)(RANDOMART_LEN - 1)); // 'S' centre
  CHECK(f[0][0] == (uint8_t)RANDOMART_LEN);                                   // 'E' corner
  // Empty input is safe and degenerate: with no walk, start == end == centre,
  // so the End marker (written last) sits on the centre. Just must not crash.
  uint8_t f0[RANDOMART_H][RANDOMART_W];
  randomart_field(nullptr, 0, f0);
  CHECK(f0[RANDOMART_H / 2][RANDOMART_W / 2] == (uint8_t)RANDOMART_LEN); // 'E'
}

// ── the ASCII tier is escape-free, 7-bit, and width-aligned ──────────────────
static void test_ascii_tier_is_safe() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  TrustInfo t = sample(KEY_A, 8);
  trust_card(r, t);

  // 1. No ESC byte, and every byte is 7-bit ASCII (no UTF-8 borders leaked).
  for (unsigned char c : sk.s) {
    CHECK(c != 0x1b);
    CHECK(c < 0x80);
  }
  // 2. Every framed line is the same visible width (content is ASCII → bytes ==
  //    columns, so string length is the width).
  auto lines = split_crlf(sk.s);
  size_t w = 0;
  for (auto& ln : lines) {
    if (ln.empty()) continue;
    if (!w) w = ln.size();
    CHECK(ln.size() == w);
  }
  CHECK(w == (size_t)(TRUST_INNER + 2)); // 54
  // 3. The identity actually appears.
  CHECK(sk.s.find("canary-7fA3") != std::string::npos);
  CHECK(sk.s.find("9b2cff0144a07e33") != std::string::npos);
  CHECK(sk.s.find("3f9ac10b") != std::string::npos);
  CHECK(sk.s.find("2.2.0") != std::string::npos);
  // 4. ASCII borders, not Unicode.
  CHECK(sk.s.find('+') != std::string::npos);
  CHECK(sk.s.find("\xe2\x94\x8c") == std::string::npos); // no ┌
  // 5. The randomart Start marker is present.
  CHECK(sk.s.find('S') != std::string::npos);
}

// ── the full tier does light up (colour + Unicode) ──────────────────────────
static void test_full_tier_lights_up() {
  Sink sk;
  Renderer r{collect, &sk, caps_full(100, 40)};
  TrustInfo t = sample(KEY_A, 8);
  trust_card(r, t);
  CHECK(sk.s.find("\x1b[") != std::string::npos);          // has SGR/escapes
  CHECK(sk.s.find("\xe2\x94\x8c") != std::string::npos);   // has ┌ (Unicode border)
  CHECK(sk.s.find("\x1b[0m") != std::string::npos);        // resets colour
  CHECK(sk.s.find("canary-7fA3") != std::string::npos);
  // Cursor is never left hidden by a static scene.
  CHECK(sk.s.find("\x1b[?25l") == std::string::npos);
}

// ── health colours carry a word too (WCAG 1.4.1) ────────────────────────────
static void test_health_has_a_word_not_just_colour() {
  for (int h : {100, 70, -1}) {
    Sink sk;
    Renderer r{collect, &sk, caps_full(90, 30)};
    TrustInfo t = sample(KEY_A, 8);
    t.health = h;
    trust_card(r, t);
    const char* word = h < 0 ? "unknown" : (h >= 100 ? "nominal" : "degraded");
    CHECK(sk.s.find(word) != std::string::npos);
  }
}

// ── the tamper state is loud in text, not colour alone ──────────────────────
static void test_tamper_is_worded() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  TrustInfo t = sample(KEY_A, 8);
  t.tamper = true;
  trust_card(r, t);
  CHECK(sk.s.find("TAMPER") != std::string::npos);
}

// ── the animated wake: real per-probe reveal, robust in both tiers ──────────
static const WakeProbe kProbes[10] = {
  {"NVS read/write", ProbeState::Pass, 12}, {"Free heap", ProbeState::Pass, 1},
  {"PSRAM", ProbeState::Pass, 3},           {"Device keys", ProbeState::Pass, 40},
  {"SD card", ProbeState::Fail, 5},         {"Wi-Fi radio", ProbeState::Pass, 8},
  {"Die temp", ProbeState::Pass, 2},        {"Uptime clock", ProbeState::Pass, 1},
  {"Watchdog", ProbeState::Pass, 1},        {"Witness chain", ProbeState::Pass, 30},
};

static void test_wake_markers_carry_words() {
  // Every state has a distinct, ASCII, meaning-bearing marker (not colour only).
  CHECK(std::string(probe_marker(ProbeState::Pending)) == "[..]");
  CHECK(std::string(probe_marker(ProbeState::Running)) == "[~~]");
  CHECK(std::string(probe_marker(ProbeState::Pass)) == "[OK]");
  CHECK(std::string(probe_marker(ProbeState::Fail)) == "[!!]");
}

static void test_wake_ascii_tier_is_safe_and_aligned() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  wake_frame(r, TRUST_INNER, kProbes, 10, 90, /*done=*/true);
  for (unsigned char c : sk.s) { CHECK(c != 0x1b); CHECK(c < 0x80); }
  auto lines = split_crlf(sk.s);
  size_t w = 0;
  for (auto& ln : lines) { if (ln.empty()) continue; if (!w) w = ln.size(); CHECK(ln.size() == w); }
  CHECK(w == (size_t)(TRUST_INNER + 2));
  // real per-probe truth is shown: the failing SD probe reads [!!], others [OK].
  CHECK(sk.s.find("[!!] SD card") != std::string::npos);
  CHECK(sk.s.find("[OK] NVS read/write") != std::string::npos);
  CHECK(sk.s.find("90%") != std::string::npos);
  CHECK(sk.s.find("9/10 probes passed") != std::string::npos);
}

static void test_wake_running_frame_does_not_spoil_score() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  WakeProbe p[10];
  for (int i = 0; i < 10; ++i) { p[i] = kProbes[i]; p[i].state = ProbeState::Pending; }
  wake_frame(r, TRUST_INNER, p, 10, 90, /*done=*/false);
  CHECK(sk.s.find("checking") != std::string::npos);
  CHECK(sk.s.find("0/10 reported") != std::string::npos);
  CHECK(sk.s.find("probes passed") == std::string::npos); // final score hidden until done
}

static void test_wake_cursor_control_is_gated() {
  // ANSI tier: cursor moves are emitted; ASCII tier: nothing (can't reposition).
  { Sink sk; Renderer r{collect, &sk, caps_full(90, 30)};
    cursor_up(r, 13); hide_cursor(r); show_cursor(r);
    CHECK(sk.s.find("\x1b[13A") != std::string::npos);
    CHECK(sk.s.find("\x1b[?25l") != std::string::npos);
    CHECK(sk.s.find("\x1b[?25h") != std::string::npos); }
  { Sink sk; Renderer r{collect, &sk, caps_ascii()};
    cursor_up(r, 13); hide_cursor(r); show_cursor(r);
    CHECK(sk.s.empty()); }
  // Height accounts for top rule + n probes + status + bottom rule.
  CHECK(wake_height(10) == 13);
}

static void test_wake_full_tier_lights_up() {
  Sink sk;
  Renderer r{collect, &sk, caps_full(100, 40)};
  wake_frame(r, TRUST_INNER, kProbes, 10, 90, true);
  CHECK(sk.s.find("\x1b[") != std::string::npos);        // colour/escapes
  CHECK(sk.s.find("\x1b[0m") != std::string::npos);      // resets
  CHECK(sk.s.find("\xe2\x94\x8c") != std::string::npos); // ┌ Unicode border
}

static void test_welcome_card() {
  // ASCII tier: escape-free, aligned, and it points at the help site + is warm.
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  welcome_card(r, "canary-7fA3", "https://securacv.com/canary");
  for (unsigned char c : sk.s) { CHECK(c != 0x1b); CHECK(c < 0x80); }
  auto lines = split_crlf(sk.s);
  size_t w = 0;
  for (auto& ln : lines) { if (ln.empty()) continue; if (!w) w = ln.size(); CHECK(ln.size() == w); }
  CHECK(w == (size_t)(TRUST_INNER + 2));
  CHECK(sk.s.find("https://securacv.com/canary") != std::string::npos); // leads somewhere
  CHECK(sk.s.find("canary-7fA3") != std::string::npos);
  CHECK(sk.s.find("Canary") != std::string::npos);
  // A null device id / url must not crash and still shows a default URL.
  Sink sk2; Renderer r2{collect, &sk2, caps_full(90, 30)};
  welcome_card(r2, nullptr, nullptr);
  CHECK(sk2.s.find("securacv.com/canary") != std::string::npos);
  CHECK(sk2.s.find("\x1b[") != std::string::npos); // colour at the confirmed tier
}

static void test_mood_cards_are_ascii_and_privacy_safe() {
  for (int i = 0; i <= (int)CanaryScene::Night; ++i) {
    Sink sk;
    Renderer r{collect, &sk, caps_ascii()};
    mood_card(r, (CanaryScene)i, "operator-visible context only");
    for (unsigned char c : sk.s) { CHECK(c != 0x1b); CHECK(c < 0x80); }
    CHECK(sk.s.find("privacy-safe local scene") != std::string::npos);
    CHECK(sk.s.find("person:") == std::string::npos);
    CHECK(sk.s.find("license plate") == std::string::npos);
  }
}

static void test_logo_is_single_sourced() {
  // The Canary logo is ONE silhouette across every scene (and matches the site).
  // Render both cards and assert each logo row appears in both — so nobody can
  // quietly re-draw a different bird in one place and let them drift apart.
  Sink wc, tc;
  Renderer rw{collect, &wc, caps_ascii()};
  Renderer rt{collect, &tc, caps_ascii()};
  welcome_card(rw, "canary-7fA3", "https://securacv.com/canary");
  trust_card(rt, sample(KEY_A, 8));
  for (const char* logo_row : CANARY_LOGO) {
    CHECK(wc.s.find(logo_row) != std::string::npos);
    CHECK(tc.s.find(logo_row) != std::string::npos);
  }
  // The canonical bird is exactly these three 5-wide rows.
  CHECK(std::string(CANARY_LOGO[0]) == ",___,");
  CHECK(std::string(CANARY_LOGO[1]) == "(o.o)");
  CHECK(std::string(CANARY_LOGO[2]) == "/)_/)");
}

int main() {
  test_welcome_card();
  test_logo_is_single_sourced();
  test_mood_cards_are_ascii_and_privacy_safe();
  test_randomart_golden();
  test_randomart_walk();
  test_randomart_all_zero();
  test_ascii_tier_is_safe();
  test_full_tier_lights_up();
  test_health_has_a_word_not_just_colour();
  test_tamper_is_worded();
  test_wake_markers_carry_words();
  test_wake_ascii_tier_is_safe_and_aligned();
  test_wake_running_frame_does_not_spoil_score();
  test_wake_cursor_control_is_gated();
  test_wake_full_tier_lights_up();

  if (g_failures == 0) { std::printf("PASS test_console_scene (all assertions)\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
