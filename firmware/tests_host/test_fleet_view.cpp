/* Host tests for the fleet view (firmware/common/ui/fleet_view.h).
 *
 * The console face of the self-* roadmap's Fleet map: "which other Canaries can
 * this one hear, and how are they doing." Proves the properties that make it
 * safe and honest rather than fragile:
 *   1. The ASCII tier NEVER emits an escape byte or a non-7-bit byte, and every
 *      framed line aligns to the same width — so on an unknown terminal it
 *      degrades to clean text, not `^[[..m` garbage (which our own flasher would
 *      flag as wrong-baud). Same contract as the trust card.
 *   2. The card is HONEST: it says presence is UNSIGNED, and every attention
 *      state (tamper/alert/degraded) is worded, not colour-only (WCAG 1.4.1).
 *   3. The pure formatters (age / percent / flag words) are exact and bounded.
 *   4. A GOLDEN render is pinned, so an accidental format change fails loudly.
 *      (Device-console stability pin — not a cross-repo wire contract like the
 *      randomart golden; the website reads the fleet from the manifest, not this
 *      ASCII card.)
 *
 * Build & run (CI: `make -C firmware/tests_host`):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common \
 *       test_fleet_view.cpp -o /tmp/t && /tmp/t
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/console_theme.h"
#include "ui/fleet_view.h"
#include "fleet_link/fleet_beacon.h"
#include "fleet_link/fleet_roster.h"  // FLEET_ROSTER_MAX — the capacity we report

using namespace scene;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

// ── a sink that collects into a std::string (same as test_console_scene) ─────
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

static std::string rstrip(const std::string& s) {
  size_t e = s.size();
  while (e > 0 && s[e - 1] == ' ') --e;
  return s.substr(0, e);
}

// A stable three-peer fleet: a healthy peer, a peer under attention (degraded +
// alert) with an unknown battery, and a stale peer that has gone quiet.
static const FleetPeer PEERS[3] = {
  { "a1b2",   3, 100,  87, 0x4210, 0,                                                        0 },
  { "c3d4",  47,  72,  -1, 0x0031, (uint8_t)(FLEET_BEACON_FLAG_DEGRADED | FLEET_BEACON_FLAG_ALERT), 0 },
  { "ef56", 130,  -1,  40, 0x1000, FLEET_BEACON_FLAG_MIC_MUTED,                              0 },
};

static FleetView sample() {
  FleetView v{};
  v.self_id = "canary-7fA3";
  v.self_fp4 = "9e33";
  v.peers = PEERS;
  v.count = 3;
  v.capacity = FLEET_ROSTER_MAX;
  return v;
}

// ── 1. the ASCII tier is escape-free, 7-bit, and width-aligned ──────────────
static void test_ascii_tier_is_safe_and_aligned() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  fleet_card(r, sample());

  for (unsigned char c : sk.s) { CHECK(c != 0x1b); CHECK(c < 0x80); }
  auto lines = split_crlf(sk.s);
  size_t w = 0;
  for (auto& ln : lines) {
    if (ln.empty()) continue;
    if (!w) w = ln.size();
    CHECK(ln.size() == w);
  }
  CHECK(w == (size_t)(FLEET_INNER + 2));  // 54, aligned with the trust card
  // ASCII borders, not Unicode.
  CHECK(sk.s.find('+') != std::string::npos);
  CHECK(sk.s.find("\xe2\x94\x8c") == std::string::npos);  // no U+250C
}

// ── 2. the card is honest and shows the fleet ───────────────────────────────
static void test_card_is_honest_and_shows_the_fleet() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  fleet_card(r, sample());

  CHECK(sk.s.find("canary-7fA3") != std::string::npos);   // this device
  CHECK(sk.s.find("a1b2") != std::string::npos);          // a peer id
  CHECK(sk.s.find("Heard 3 of 16 nearby") != std::string::npos);
  CHECK(sk.s.find("UNSIGNED") != std::string::npos);       // the honesty line
  // Attention states are WORDED, not colour-only.
  CHECK(sk.s.find("degraded") != std::string::npos);
  CHECK(sk.s.find("alert") != std::string::npos);
  CHECK(sk.s.find("muted") != std::string::npos);
  CHECK(sk.s.find("ok") != std::string::npos);             // the healthy peer
  // Unknown battery/health render as "--", not a bogus number.
  CHECK(sk.s.find("--") != std::string::npos);
}

// ── 3. the empty roster is friendly and still safe ──────────────────────────
static void test_empty_roster() {
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  FleetView v{};
  v.self_id = "canary-7fA3";
  v.peers = nullptr;
  v.count = 0;
  v.capacity = FLEET_ROSTER_MAX;
  fleet_card(r, v);

  CHECK(sk.s.find("No other Canaries heard yet") != std::string::npos);
  CHECK(sk.s.find("UNSIGNED") != std::string::npos);
  for (unsigned char c : sk.s) { CHECK(c != 0x1b); CHECK(c < 0x80); }
  auto lines = split_crlf(sk.s);
  size_t w = 0;
  for (auto& ln : lines) { if (ln.empty()) continue; if (!w) w = ln.size(); CHECK(ln.size() == w); }
  CHECK(w == (size_t)(FLEET_INNER + 2));
}

// ── 4. the full tier lights up (colour + Unicode) ───────────────────────────
static void test_full_tier_lights_up() {
  Sink sk;
  Renderer r{collect, &sk, caps_full(100, 40)};
  fleet_card(r, sample());
  CHECK(sk.s.find("\x1b[") != std::string::npos);          // has SGR/escapes
  CHECK(sk.s.find("\xe2\x94\x8c") != std::string::npos);   // has U+250C border
  CHECK(sk.s.find("\x1b[0m") != std::string::npos);        // resets colour
  CHECK(sk.s.find("canary-7fA3") != std::string::npos);
  CHECK(sk.s.find("\x1b[?25l") == std::string::npos);      // never hides the cursor
}

// ── 5. the pure formatters are exact and bounded ────────────────────────────
static void test_formatters() {
  char b[40];
  fleet_fmt_age(0, b, sizeof b);     CHECK(std::string(b) == "0s");
  fleet_fmt_age(3, b, sizeof b);     CHECK(std::string(b) == "3s");
  fleet_fmt_age(59, b, sizeof b);    CHECK(std::string(b) == "59s");
  fleet_fmt_age(60, b, sizeof b);    CHECK(std::string(b) == "1m");
  fleet_fmt_age(3599, b, sizeof b);  CHECK(std::string(b) == "59m");
  fleet_fmt_age(3600, b, sizeof b);  CHECK(std::string(b) == "1h");
  fleet_fmt_age(90000, b, sizeof b); CHECK(std::string(b) == ">1d");

  fleet_fmt_pct(-1, b, sizeof b);  CHECK(std::string(b) == "--");
  fleet_fmt_pct(0, b, sizeof b);   CHECK(std::string(b) == "0%");
  fleet_fmt_pct(72, b, sizeof b);  CHECK(std::string(b) == "72%");
  fleet_fmt_pct(100, b, sizeof b); CHECK(std::string(b) == "100%");
  fleet_fmt_pct(150, b, sizeof b); CHECK(std::string(b) == "100%");  // clamped

  fleet_fmt_flags(0, b, sizeof b); CHECK(std::string(b) == "ok");
  fleet_fmt_flags(FLEET_BEACON_FLAG_TAMPER, b, sizeof b);
  CHECK(std::string(b) == "tamper");
  fleet_fmt_flags((uint8_t)(FLEET_BEACON_FLAG_TAMPER | FLEET_BEACON_FLAG_ALERT), b, sizeof b);
  CHECK(std::string(b) == "tamper, alert");  // worst-first
  CHECK(fleet_flags_alarming(FLEET_BEACON_FLAG_TAMPER));
  CHECK(fleet_flags_alarming(FLEET_BEACON_FLAG_ALERT));
  CHECK(fleet_flags_alarming(FLEET_BEACON_FLAG_DEGRADED));
  CHECK(!fleet_flags_alarming(FLEET_BEACON_FLAG_MIC_MUTED));
  CHECK(!fleet_flags_alarming(FLEET_BEACON_FLAG_ON_WIFI_STA));
  CHECK(!fleet_flags_alarming(0));

  // Bounded: a tiny buffer never overflows and always NUL-terminates.
  char tiny[4];
  fleet_fmt_flags((uint8_t)(FLEET_BEACON_FLAG_TAMPER | FLEET_BEACON_FLAG_ALERT), tiny, sizeof tiny);
  CHECK(tiny[3] == '\0');
  CHECK(std::strlen(tiny) <= 3);
}

// ── 5b. the roster-entry → view-peer bridge (the drift-prone millis math) ───
static void test_peer_from_entry() {
  FleetRosterEntry e{};
  std::strcpy(e.fp4, "a1b2");
  e.last_seen_ms = 1000;
  e.health_pct = 88;
  e.battery_pct = 55;
  e.chain_lo = 0x1234;
  e.flags = FLEET_BEACON_FLAG_ALERT;
  e.rssi = -60;
  e.used = 1;

  FleetPeer p = fleet_peer_from_entry(e, 4000);  // 3 s later
  CHECK(std::string(p.fp4) == "a1b2");
  CHECK(p.age_s == 3);
  CHECK(p.health_pct == 88);
  CHECK(p.battery_pct == 55);
  CHECK(p.chain_lo == 0x1234);
  CHECK(p.flags == FLEET_BEACON_FLAG_ALERT);
  CHECK(p.rssi == -60);

  // Sub-second age floors to 0.
  CHECK(fleet_peer_from_entry(e, 1500).age_s == 0);

  // millis() wrap: now < last_seen must not produce a giant bogus age.
  FleetRosterEntry w{};
  std::strcpy(w.fp4, "c3d4");
  w.last_seen_ms = 0xFFFFFF00u;  // just before wrap
  w.used = 1;
  FleetPeer pw = fleet_peer_from_entry(w, 0x00000064u);  // 356 ms after wrap
  CHECK(pw.age_s == 0);          // small, correct — not ~4.29e6 s
}

// ── 6. the golden render (device-console stability pin) ─────────────────────
static void test_golden() {
  // The exact ASCII-tier shape for the sample fleet. If this changes, an
  // operator's `n`/"nearby" console view silently reformats — pin it so a
  // format change is a deliberate, reviewed edit. Trailing-space-insensitive:
  // every framed line is FLEET_INNER+2 wide (asserted separately above).
  Sink sk;
  Renderer r{collect, &sk, caps_ascii()};
  fleet_card(r, sample());

  static const char* const GOLDEN[] = {
    "+- Fleet  -  nearby Canaries ------------------------+",
    "| This Canary  canary-7fA3   fp 9e33                 |",
    "| Heard 3 of 16 nearby   (within ~2 min)             |",
    "+- peers --------------------------------------------+",
    "| peer  age hlth batt chain  status                  |",
    "| a1b2   3s 100%  87% #4210  ok                      |",
    "| c3d4  47s  72%   -- #0031  alert, degraded         |",
    "| ef56   2m   --  40% #1000  muted                   |",
    "+----------------------------------------------------+",
    "| Presence is UNSIGNED - liveness only, never a      |",
    "| verified trust claim (like the beacon it reads).   |",
    "+----------------------------------------------------+",
  };
  auto lines = split_crlf(sk.s);
  // Drop a trailing empty element from the final CRLF, if present.
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  const size_t n = sizeof(GOLDEN) / sizeof(GOLDEN[0]);
  CHECK(lines.size() == n);
  for (size_t i = 0; i < n && i < lines.size(); ++i)
    CHECK(rstrip(lines[i]) == rstrip(std::string(GOLDEN[i])));
}

int main() {
  test_ascii_tier_is_safe_and_aligned();
  test_card_is_honest_and_shows_the_fleet();
  test_empty_roster();
  test_full_tier_lights_up();
  test_formatters();
  test_peer_from_entry();
  test_golden();

  if (g_failures == 0) { std::printf("PASS test_fleet_view (all assertions)\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
