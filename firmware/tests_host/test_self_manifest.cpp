/* Host tests for the device self-manifest builder (firmware/common/attest).
 *
 * The manifest is a wire format the website parses over WebSerial, so these
 * tests pin the two things that matter:
 *   1. It is well-formed, single-line JSON with the pinned schema and every
 *      required key — the website can rely on the shape.
 *   2. String values are escaped and the writer is bounded — a hostile or
 *      oversized input can never produce truncated/garbage JSON or overflow.
 *
 * Build & run (via firmware/tests_host/Makefile, mirrors the CI contract):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common test_self_manifest.cpp
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

#include "attest/self_manifest.h"

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

static const char* const kFeatures[] = { "sd_storage", "wifi_ap", "gnss",
                                         "ble_status", "console_theme" };

static manifest::Facts sample() {
  manifest::Facts f{};
  f.board = "canary";
  f.firmware = "2.2.0";
  f.git = "abc1234";
  f.protocol = "pwk:v0.3.0";
  f.device_id = "canary-7fA3";
  f.pubkey_hex = "9b2cff0144a07e33aa11bb22cc33dd44ee55ff66007788990011223344556677";
  f.pubkey_fp_hex = "9b2cff0144a07e33";
  f.chain_head_hex = "3f9ac10b00000000000000000000000000000000000000000000000000000000";
  f.seq = 41;
  f.boots = 12;
  f.health = 100;
  f.tamper = false;
  f.features = kFeatures;
  f.feature_count = sizeof(kFeatures) / sizeof(kFeatures[0]);
  f.help_url = "https://securacv.com/canary";
  return f;
}

// ── well-formed, one line, pinned schema, every required key present ────────
static void test_shape_and_keys() {
  char buf[1024];
  manifest::Facts f = sample();
  size_t n = manifest::build(f, buf, sizeof buf);
  std::string s(buf);
  CHECK(n == s.size());
  CHECK(!s.empty());
  CHECK(s.front() == '{');
  CHECK(s.back() == '}');
  CHECK(s.find('\n') == std::string::npos);   // single line
  CHECK(s.find('\r') == std::string::npos);

  CHECK(has(s, "\"schema\":\"securacv.canary.manifest/v1\""));
  const char* keys[] = { "\"board\"", "\"firmware\"", "\"git\"", "\"protocol\"",
                         "\"device_id\"", "\"pubkey\"", "\"pubkey_fp\"",
                         "\"chain_head\"", "\"seq\"", "\"boots\"", "\"health\"",
                         "\"tamper\"", "\"features\"", "\"help\"" };
  for (const char* k : keys) CHECK(has(s, k));

  // The pubkey — the randomart source — must be present verbatim (64 hex).
  CHECK(has(s, f.pubkey_hex));
  CHECK(std::strlen(f.pubkey_hex) == 64);
  // Numbers are bare, booleans are bare, the feature array renders.
  CHECK(has(s, "\"seq\":41"));
  CHECK(has(s, "\"boots\":12"));
  CHECK(has(s, "\"tamper\":false"));
  CHECK(has(s, "\"health\":100"));
  CHECK(has(s, "\"features\":[\"sd_storage\",\"wifi_ap\",\"gnss\",\"ble_status\",\"console_theme\"]"));
}

// ── unknown health is JSON null, tamper flips to a bare true ─────────────────
static void test_unknown_health_and_tamper() {
  char buf[1024];
  manifest::Facts f = sample();
  f.health = -1;
  f.tamper = true;
  manifest::build(f, buf, sizeof buf);
  std::string s(buf);
  CHECK(has(s, "\"health\":null"));
  CHECK(has(s, "\"tamper\":true"));
  CHECK(!has(s, "\"health\":-1"));   // never a raw negative
  // health is also clamped to 100 on the high side.
  f.health = 250; f.tamper = false;
  manifest::build(f, buf, sizeof buf);
  CHECK(has(std::string(buf), "\"health\":100"));
}

// ── string values are escaped (never trust an input into a wire format) ─────
static void test_escaping() {
  char buf[1024];
  manifest::Facts f = sample();
  f.device_id = "canary\"\\\n\tx";   // quote, backslash, newline, tab
  manifest::build(f, buf, sizeof buf);
  std::string s(buf);
  CHECK(has(s, "\"device_id\":\"canary\\\"\\\\\\n\\tx\""));
  CHECK(s.find('\n') == std::string::npos);  // the raw newline never leaks
  CHECK(s.find('\t') == std::string::npos);
}

// ── empty feature set renders an empty array, not a dangling comma ──────────
static void test_no_features() {
  char buf[1024];
  manifest::Facts f = sample();
  f.features = nullptr; f.feature_count = 0;
  manifest::build(f, buf, sizeof buf);
  CHECK(has(std::string(buf), "\"features\":[]"));
}

// ── null pointers are safe (a half-initialised device must not crash) ───────
static void test_null_fields_safe() {
  char buf[1024];
  manifest::Facts f{};   // everything zero/null
  size_t n = manifest::build(f, buf, sizeof buf);
  std::string s(buf);
  CHECK(n > 0);
  CHECK(s.front() == '{' && s.back() == '}');
  CHECK(has(s, "\"device_id\":\"\""));      // null → empty string, not a crash
  CHECK(has(s, "\"features\":[]"));
}

// ── a too-small buffer fails closed: empty string + 0, never a truncation ───
static void test_bounded_no_overflow() {
  manifest::Facts f = sample();
  for (size_t cap = 1; cap < 400; ++cap) {
    char buf[512];
    for (size_t i = 0; i < sizeof buf; ++i) buf[i] = (char)0x7f;  // canary bytes
    size_t n = manifest::build(f, buf, cap);
    // Whatever happens, it stays inside cap and stays NUL-terminated.
    CHECK(buf[cap - 1] == '\0' || n == 0);
    if (n == 0) {
      CHECK(buf[0] == '\0');                 // fail-closed: explicit empty
    } else {
      CHECK(n < cap);
      CHECK(buf[n] == '\0');
      CHECK(std::strlen(buf) == n);          // no interior NUL, no overrun
      CHECK(buf[0] == '{');
    }
    // The bytes past cap must be untouched (no overflow write).
    for (size_t i = cap; i < sizeof buf; ++i) CHECK(buf[i] == (char)0x7f);
  }
  // With ample room it always succeeds and is complete.
  char big[1024];
  CHECK(manifest::build(f, big, sizeof big) > 0);
}

int main() {
  test_shape_and_keys();
  test_unknown_health_and_tamper();
  test_escaping();
  test_no_features();
  test_null_fields_safe();
  test_bounded_no_overflow();

  if (g_failures == 0) { std::printf("ALL self-manifest tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
