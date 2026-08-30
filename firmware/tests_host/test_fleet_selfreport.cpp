/* Host tests for the shared /api/fleet self-report builder
 * (firmware/common/fleet_selfreport/fleet_selfreport.h).
 *
 * This is the "parity by architecture" core: every networked Canary answers
 * GET /api/fleet from THIS one builder, so a change to the wire shape is a
 * change to this one header — and these tests pin that shape. They mirror the
 * discipline of test_self_manifest.cpp (a bounded, escaping JSON writer):
 *
 *   1. The body is well-formed, single-line JSON matching the contract in
 *      tvos/discovery/DISCOVERY.md — every required key, in the shape the
 *      Witness Wall emulator's "connect" and the Flasher's post-flash LAN
 *      discovery read.
 *   2. String values are JSON-escaped — an attacker-influenced device name can
 *      never break out of the JSON (the same discipline as the web UI's
 *      HTML-escaping; see the CodeQL XSS fix).
 *   3. The writer is bounded — a too-small buffer truncates safely, always
 *      NUL-terminates, and never writes past `cap`.
 *   4. The open/append/close variants compose into a multi-device (hub) body.
 *
 * Build & run (via firmware/tests_host/Makefile, mirrors the CI contract):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common test_fleet_selfreport.cpp
 */
#include <cstdio>
#include <cstring>
#include <string>

/* ── Arduino macro-namespace simulation (regression guard) ────────────────────
 *
 * The header ships on ESP32 boards, where Arduino's Print.h has already done
 * this to the preprocessor before any sketch include is reached. A local
 * variable named HEX therefore expands to `static const char* 16 = ...` and
 * EVERY Arduino/PlatformIO build fails to compile — while a plain g++ host
 * test, which has no such macros, passes happily. That is exactly the gap that
 * let a broken header through host tests and turn the whole firmware CI matrix
 * red, so the macros are reproduced HERE, before the include, and the host
 * build now fails the same way the device build would.
 *
 * Keep this block immediately above the include. Adding a name Arduino defines
 * (see cores/esp32/Print.h and Arduino.h) is a feature, not noise.
 */
#define HEX 16
#define DEC 10
#define OCT 8
#define BIN 2

#include "fleet_selfreport/fleet_selfreport.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

static bool has(const std::string& s, const char* sub) {
  return s.find(sub) != std::string::npos;
}

// Build a single-device body and hand it back as a string — the shape most of
// these tests assert against, without a buffer at every call site.
static std::string build(const FleetSelfDevice& d) {
  char buf[512];
  fleet_selfreport_build(buf, sizeof buf, &d);
  return std::string(buf);
}

static FleetSelfDevice sample() {
  FleetSelfDevice d{};
  d.name = "Front Door";
  d.product = "canary-wap";
  d.online = 1;
  d.chain_ok = 1;
  d.chain_height = 4210;
  return d;
}

// ── a single-device body is well-formed, one line, contract shape ───────────
static void test_single_device_shape() {
  char buf[512];
  FleetSelfDevice d = sample();
  size_t n = fleet_selfreport_build(buf, sizeof buf, &d);
  std::string s(buf);
  CHECK(n == s.size());
  CHECK(!s.empty());
  CHECK(s.front() == '{');
  CHECK(s.back() == '}');
  CHECK(s.find('\n') == std::string::npos);   // single line
  CHECK(s.find('\r') == std::string::npos);

  // The exact contract shape from DISCOVERY.md.
  CHECK(has(s, "\"kernel\":\"Front Door\""));
  CHECK(has(s, "\"verified_through\":\"now\""));
  CHECK(has(s, "\"devices\":["));
  CHECK(has(s, "\"name\":\"Front Door\""));
  CHECK(has(s, "\"online\":true"));
  CHECK(has(s, "\"chain\":\"ok\""));
  CHECK(has(s, "\"product\":\"canary-wap\""));
  CHECK(has(s, "\"chain_height\":4210"));
  // The single-device body is exactly one object inside the array.
  CHECK(has(s, "\"devices\":[{\"name\":\"Front Door\",\"online\":true,"
              "\"chain\":\"ok\",\"product\":\"canary-wap\",\"chain_height\":4210}]}"));
}

// ── offline / degraded / no-height renders honestly, no dangling keys ───────
static void test_offline_degraded() {
  char buf[512];
  FleetSelfDevice d{};
  d.name = "Driveway";
  d.product = "canary-vision";
  d.online = 0;
  d.chain_ok = 0;
  d.chain_height = -1;          // < 0 ⇒ omit chain_height entirely
  fleet_selfreport_build(buf, sizeof buf, &d);
  std::string s(buf);
  CHECK(has(s, "\"online\":false"));
  CHECK(has(s, "\"chain\":\"unknown\""));
  CHECK(!has(s, "chain_height"));   // omitted, not "chain_height":-1
  CHECK(s.back() == '}');
}

// ── null / empty fields are safe (a half-initialized device must not crash) ─
static void test_null_fields_safe() {
  char buf[512];
  FleetSelfDevice d{};             // everything zero/null
  d.chain_height = -1;
  size_t n = fleet_selfreport_build(buf, sizeof buf, &d);
  std::string s(buf);
  CHECK(n > 0);
  CHECK(s.front() == '{' && s.back() == '}');
  CHECK(has(s, "\"kernel\":\"Canary\""));      // null name → default, not a crash
  CHECK(has(s, "\"name\":\"Canary\""));
  CHECK(has(s, "\"product\":\"\""));           // null product → empty string
  CHECK(has(s, "\"online\":false"));

  // A wholly-null pointer must also be safe.
  n = fleet_selfreport_build(buf, sizeof buf, nullptr);
  CHECK(n > 0);
  CHECK(std::string(buf).front() == '{');
}

// ── string values are JSON-escaped (never trust a name into a wire format) ──
static void test_escaping() {
  char buf[512];
  FleetSelfDevice d = sample();
  d.name = "Bar\"; <script>\n\tx";   // quote, angle brackets, newline, tab
  size_t n = fleet_selfreport_build(buf, sizeof buf, &d);
  std::string s(buf);
  CHECK(n == s.size());
  // The quote/backslash/control chars are escaped; the raw bytes never leak.
  CHECK(has(s, "\\\""));                        // the embedded quote is escaped
  CHECK(has(s, "\\n"));
  CHECK(has(s, "\\t"));
  CHECK(s.find('\n') == std::string::npos);     // no raw newline in the body
  CHECK(s.find('\t') == std::string::npos);     // no raw tab in the body
  // A control byte becomes a \u00XX escape, not a raw byte.
  FleetSelfDevice d2 = sample();
  char name[] = { 'a', (char)0x01, 'b', '\0' };
  d2.name = name;
  fleet_selfreport_build(buf, sizeof buf, &d2);
  CHECK(has(std::string(buf), "\\u0001"));
}

// ── open/append/close compose into a multi-device (hub/aggregator) body ─────
static void test_multi_device_compose() {
  char buf[1024];
  FleetSelfDevice a = sample();          // Front Door
  FleetSelfDevice b{};
  b.name = "Studio"; b.product = "canary"; b.online = 1; b.chain_ok = 1;
  b.chain_height = -1;

  size_t o = fleet_selfreport_open(buf, sizeof buf, "kitchen-hub");
  o = fleet_selfreport_append_device(buf, sizeof buf, o, &a);
  o = fsr__raw(buf, sizeof buf, o, ",");       // caller joins peer rows
  o = fleet_selfreport_append_device(buf, sizeof buf, o, &b);
  o = fleet_selfreport_close(buf, sizeof buf, o);

  std::string s(buf);
  CHECK(o == s.size());
  CHECK(s.front() == '{' && s.back() == '}');
  CHECK(has(s, "\"kernel\":\"kitchen-hub\""));
  CHECK(has(s, "\"name\":\"Front Door\""));
  CHECK(has(s, "\"name\":\"Studio\""));
  // Two device objects, comma-joined, inside one array.
  CHECK(has(s, "},{"));
  // Studio omits chain_height; Front Door keeps it.
  CHECK(has(s, "\"name\":\"Studio\",\"online\":true,\"chain\":\"ok\","
              "\"product\":\"canary\"}"));
}

// ── a too-small buffer fails safe: stays inside cap, always NUL-terminated ──
static void test_bounded_no_overflow() {
  FleetSelfDevice d = sample();
  for (size_t cap = 1; cap < 200; ++cap) {
    char buf[256];
    for (size_t i = 0; i < sizeof buf; ++i) buf[i] = (char)0x7f;  // canary bytes
    size_t n = fleet_selfreport_build(buf, cap, &d);
    // Whatever happens, it stays inside cap and stays NUL-terminated at the
    // write offset, and the returned length is the real string length (no
    // interior NUL, no overrun) — the writer truncates rather than fails closed.
    CHECK(n < cap);
    CHECK(buf[n] == '\0');
    CHECK(std::strlen(buf) == n);
    // The bytes at/after cap must be untouched (no overflow write).
    for (size_t i = cap; i < sizeof buf; ++i) CHECK(buf[i] == (char)0x7f);
  }
  // With ample room the whole body is present and complete.
  char big[512];
  size_t n = fleet_selfreport_build(big, sizeof big, &d);
  CHECK(n > 0);
  CHECK(big[n] == '\0');
  CHECK(std::strlen(big) == n);
  CHECK(big[0] == '{' && big[n - 1] == '}');
}

// ── the birth day: absent until real, and never overstated ──────────────────
//
// The app decides between "Born" and "Paired" on these two keys, so the two
// ways this can lie are both tested: claiming a day the device doesn't have,
// and calling a first-dated day a birthday.
static void test_born_day_is_omitted_until_the_device_has_one() {
  char buf[512];
  FleetSelfDevice d = sample();          // sample() leaves born_day at 0
  fleet_selfreport_build(buf, sizeof buf, &d);
  std::string s(buf);
  CHECK(!has(s, "born_day"));
  CHECK(!has(s, "born_exact"));

  // A device with no witness key of its own — a display — is the same case,
  // and must not grow a "born_day":0 that a reader could render as 1970.
  CHECK(!has(s, "\"born_day\":0"));
}

static void test_born_day_reports_the_day_and_its_confidence() {
  char buf[512];
  FleetSelfDevice d = sample();
  d.born_day = 20673;                    // 2026-08-07
  d.born_exact = 1;
  fleet_selfreport_build(buf, sizeof buf, &d);
  std::string s(buf);
  CHECK(has(s, "\"born_day\":20673"));
  CHECK(has(s, "\"born_exact\":true"));

  // The same day, learned too late to be called a birthday. Same number,
  // different claim — a reader that ignored the flag would promote a shelf
  // into a birth date.
  d.born_exact = 0;
  fleet_selfreport_build(buf, sizeof buf, &d);
  s = buf;
  CHECK(has(s, "\"born_day\":20673"));
  CHECK(has(s, "\"born_exact\":false"));
}

// ── the board id: exact about the shape, absent when there is none ──────────
static void test_hardware_is_omitted_until_the_board_names_itself() {
  FleetSelfDevice d = sample();          // sample() leaves hardware null
  std::string s = build(d);
  // A build with no pins header of its own says nothing rather than "". An
  // empty string would read as a board id nobody can draw, and a reader
  // would spend a lookup proving it isn't one.
  CHECK(!has(s, "\"hw\""));

  d.hardware = "";                       // present-but-empty is the same answer
  CHECK(!has(build(d), "\"hw\""));
}

static void test_hardware_reports_the_board_not_the_product() {
  FleetSelfDevice d = sample();
  // The 7" glass: ONE board, two products. What it publishes as `product` is
  // the product; what it publishes here is the board — and the reader draws
  // the shape from this one precisely because the product cannot pin it.
  d.product = "canary-nightstand7";
  d.hardware = "waveshare-esp32s3-lcd7";
  const std::string s = build(d);
  CHECK(has(s, "\"product\":\"canary-nightstand7\""));
  CHECK(has(s, "\"hw\":\"waveshare-esp32s3-lcd7\""));
  // Ordering is part of the wire shape: hw closes the object, after birth.
  CHECK(has(s, "\"hw\":\"waveshare-esp32s3-lcd7\"}"));
}

// ── the hub state: three answers, and silence is not one of the three ───────
static void test_hub_is_omitted_when_the_device_has_no_opinion() {
  FleetSelfDevice d = sample();          // sample() leaves hub at UNKNOWN (0)
  CHECK(!has(build(d), "\"hub\""));
}

static void test_hub_distinguishes_none_from_down() {
  FleetSelfDevice d = sample();
  // The distinction that makes this field worth having: "nobody has given me
  // a hub" and "my hub is unreachable" are different problems with different
  // fixes, and a bool would have collapsed them into one unhelpful warning.
  d.hub = FSR_HUB_NONE;
  CHECK(has(build(d), "\"hub\":\"none\""));
  d.hub = FSR_HUB_DOWN;
  CHECK(has(build(d), "\"hub\":\"down\""));
  d.hub = FSR_HUB_OK;
  CHECK(has(build(d), "\"hub\":\"ok\""));
}

// ── the wellbeing keys: absent until someone can say, words when they can ───
//
// The two ways this surface could lie, both tested: a device that knows
// nothing growing keys a reader renders as an empty calm room, and a device
// that knows something publishing it as numbers instead of the fallback-safe
// words. (The keys ride the same anyone-who-asks body as name/hub — coarse
// presence only; every vital-sign NUMBER stays off it by construction.)
static void test_wellbeing_is_omitted_until_someone_can_say() {
  std::string s = build(sample());       // sample() leaves all wellbeing unset
  CHECK(!has(s, "presence"));
  CHECK(!has(s, "occupants"));
  CHECK(!has(s, "breathing"));
  CHECK(!has(s, "seeing"));

  // A zero-filled struct — every glue that predates the keys — must serve
  // exactly the old wire: nothing new to parse, nothing new to misread.
  FleetSelfDevice d{};
  d.chain_height = -1;
  s = build(d);
  CHECK(!has(s, "presence") && !has(s, "occupants"));
  CHECK(!has(s, "breathing") && !has(s, "seeing"));
}

static void test_wellbeing_reports_words_and_the_lock() {
  FleetSelfDevice d = sample();
  d.presence = "present";
  d.occupants = "2+";
  d.breathing = FSR_BREATHING_LOCK;
  std::string s = build(d);
  // Words in the sense line's own MQTT vocabulary, in wire order after the
  // health keys — the ordering is part of the shape, like hw's.
  CHECK(has(s, "\"presence\":\"present\",\"occupants\":\"2+\",\"breathing\":true}"));

  // A lock that lapses reports false — a different answer from silence.
  d.breathing = FSR_BREATHING_QUIET;
  CHECK(has(build(d), "\"breathing\":false}"));
}

static void test_seeing_claim_carries_its_score_only_when_scored() {
  FleetSelfDevice d = sample();
  d.seeing = "person";
  d.seeing_score = 88;
  CHECK(has(build(d), "\"seeing\":\"person\",\"seeing_score\":88}"));

  // Unscored (or out-of-range) keeps the word and drops the number: a claim
  // at zero confidence is not a claim, and 101 is not a percentage.
  d.seeing_score = 0;
  CHECK(has(build(d), "\"seeing\":\"person\"}"));
  d.seeing_score = 101;
  CHECK(has(build(d), "\"seeing\":\"person\"}"));

  // And the score never appears without the word — a confidence in nothing.
  d.seeing = nullptr;
  d.seeing_score = 88;
  std::string s = build(d);
  CHECK(!has(s, "seeing"));
}

// ── FLEET_SELFREPORT_BODY_CAP really covers the worst case ──────────────────
static void test_body_cap_covers_worst_case() {
  // The failure this guards (Codex P2 on #1226): a device ACCEPTS a name whose
  // bytes all need escaping, the glue's fixed buffer is sized for the friendly
  // case, and /api/fleet serves a silently truncated body with a 200 — clients
  // fail to parse exactly the device that most needs escaping. The macro exists
  // so glue sizes its buffer from the name/product bounds; prove the worst case
  // (every byte a control char → 6-byte \u00XX escapes, name written twice,
  // widest chain_height) fits and terminates as complete JSON.
  const size_t NAME_MAX = 47, PROD_MAX = 16;   // display device_id[48] bound
  char name[NAME_MAX + 1], prod[PROD_MAX + 1];
  for (size_t i = 0; i < NAME_MAX; ++i) name[i] = '\x01';   // escapes to \\u0001, 6 bytes
  name[NAME_MAX] = '\0';
  for (size_t i = 0; i < PROD_MAX; ++i) prod[i] = '\x02';
  prod[PROD_MAX] = '\0';
  FleetSelfDevice d{};
  d.name = name;
  d.product = prod;
  d.online = 1;
  d.chain_ok = 1;
  d.chain_height = 2147483647;                 // widest legal height
  // Widest birth day too, or the cap stops covering the worst case the moment
  // a device starts reporting one. A day is unix_s / 86400, so even a uint32
  // clock maxes out near 49710 — five digits is the true ceiling, but pin the
  // widest value the writer would accept rather than the widest that can occur.
  d.born_day = 2147483647u;
  d.born_exact = 0;                            // "false" is the longer literal
  // And the widest hardware id, all-escaping. Real board ids are lowercase
  // ASCII out of the generator's vocabulary and could never escape — but the
  // cap's promise is "truncation is impossible by construction", and a
  // constant proven only against friendly bytes does not keep it.
  char hw[FLEET_SELFREPORT_HW_MAX + 1];
  for (size_t i = 0; i < FLEET_SELFREPORT_HW_MAX; ++i) hw[i] = '\x03';
  hw[FLEET_SELFREPORT_HW_MAX] = '\0';
  d.hardware = hw;
  d.hub = FSR_HUB_UNKNOWN + 99;                // an unknown value -> "unknown",
                                               // the longest literal this emits
  // The wellbeing/seeing words at their widest vocabulary length (7 bytes:
  // "present", "vehicle"/"package"), every byte escaping — real values are
  // firmware vocabulary and could never escape, but the cap's promise is
  // unconditional, so the proof is too (the hw rationale, again).
  char word7[8];
  for (size_t i = 0; i < 7; ++i) word7[i] = '\x04';
  word7[7] = '\0';
  d.presence = word7;
  char occ[3] = { '\x05', '\x06', '\0' };      // "2+" is the widest bucket
  d.occupants = occ;
  d.breathing = FSR_BREATHING_QUIET;           // "false" is the longer literal
  d.seeing = word7;
  d.seeing_score = 100;                        // the widest legal score
  char body[FLEET_SELFREPORT_BODY_CAP(NAME_MAX, PROD_MAX)];
  size_t n = fleet_selfreport_build(body, sizeof body, &d);
  CHECK(n > 0);
  CHECK(n < sizeof body);                      // no truncation at the cap
  CHECK(body[0] == '{' && body[n - 1] == '}'); // complete JSON, closed array
  CHECK(std::strlen(body) == n);
  // And the canary-wap bound (name ≤ 32) fits its cap the same way.
  name[32] = '\0';
  char wap_body[FLEET_SELFREPORT_BODY_CAP(32, 16)];
  n = fleet_selfreport_build(wap_body, sizeof wap_body, &d);
  CHECK(n > 0 && n < sizeof wap_body && wap_body[n - 1] == '}');
}

// ── cap==0 / null out are no-ops, never a write ─────────────────────────────
static void test_degenerate_caps() {
  FleetSelfDevice d = sample();
  char sentinel = (char)0x7f;
  // cap == 0: nothing may be written.
  CHECK(fleet_selfreport_build(&sentinel, 0, &d) == 0);
  CHECK(sentinel == (char)0x7f);
  // null out: returns 0, no crash.
  CHECK(fleet_selfreport_build(nullptr, 64, &d) == 0);
}

int main() {
  test_single_device_shape();
  test_offline_degraded();
  test_null_fields_safe();
  test_escaping();
  test_multi_device_compose();
  test_bounded_no_overflow();
  test_born_day_is_omitted_until_the_device_has_one();
  test_born_day_reports_the_day_and_its_confidence();
  test_hardware_is_omitted_until_the_board_names_itself();
  test_hardware_reports_the_board_not_the_product();
  test_hub_is_omitted_when_the_device_has_no_opinion();
  test_hub_distinguishes_none_from_down();
  test_wellbeing_is_omitted_until_someone_can_say();
  test_wellbeing_reports_words_and_the_lock();
  test_seeing_claim_carries_its_score_only_when_scored();
  test_body_cap_covers_worst_case();
  test_degenerate_caps();

  if (g_failures == 0) { std::printf("ALL fleet-selfreport tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
