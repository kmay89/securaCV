/**
 * @file test_device_signature_common.cpp
 * @brief Host-side test for the shared device_signature module
 *        (firmware/common/identity) — canonical-message builders +
 *        b64url encoder, including the canary-sense `sense` kind.
 *
 * The Ed25519 sign path needs the Arduino Crypto library, so this test
 * covers the deterministic parts: the canonical string bytes that go
 * into the signer, and the b64url encoder that wraps the 64-byte sig
 * output. The full sign+verify round-trip is exercised on the HA side
 * via pytest (custom_components/securacv/tests/test_signature.py),
 * which rebuilds these exact byte sequences with Python's cryptography.
 *
 * The chain/event/counts vectors are the SAME reference strings the
 * canary-wap host test locks (tests_host/test_device_signature.cpp in
 * that project) — the two copies must never drift apart on canonical
 * bytes, or fielded devices stop verifying.
 */

#include "../common/identity/device_signature.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  got:  '%s'\n  want: '%s'\n",
                 what, got.c_str(), want.c_str());
    std::exit(1);
  }
}

/* ── canonical builders ─────────────────────────────────────────── */

void test_chain_canonical_known_input() {
  uint8_t hash[32];
  for (int i = 0; i < 32; ++i) hash[i] = (uint8_t)i;
  char out[256];
  const size_t n = device_signature::build_chain_canonical(
      /*length=*/42, hash, /*device_id=*/"abc123", out, sizeof(out));
  check(n > 0, "build_chain_canonical wrote something");
  const std::string want =
      "securacv-canary-sig|v1|chain|abc123|42|"
      "000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f";
  check_eq(std::string(out, n), want, "chain canonical matches reference");
}

void test_event_canonical_known_input() {
  char out[256];
  const size_t n = device_signature::build_event_canonical(
      /*event_id=*/7, /*state=*/"active", /*category=*/"event",
      /*privacy=*/"p1", /*motion=*/180, /*breath=*/120, /*bpm=*/15,
      /*device_id=*/"abc123", out, sizeof(out));
  check(n > 0, "build_event_canonical wrote something");
  const std::string want =
      "securacv-canary-sig|v1|event|abc123|7|active|event|p1|180|120|15";
  check_eq(std::string(out, n), want, "event canonical matches reference");
}

void test_counts_canonical_known_input() {
  char out[128];
  const size_t n = device_signature::build_counts_canonical(
      /*total=*/1234, /*device_id=*/"abc123", out, sizeof(out));
  check(n > 0, "build_counts_canonical wrote something");
  check_eq(std::string(out, n),
           "securacv-canary-sig|v1|counts|abc123|1234",
           "counts canonical matches reference");
}

void test_sense_canonical_known_input() {
  /* Inputs chosen for cross-language verification — the HA pytest
   * rebuilds this exact byte sequence (test_signature.py). */
  char out[256];
  const size_t n = device_signature::build_sense_canonical(
      /*seq=*/3,
      /*event_name=*/"presence_detected",
      /*presence=*/"present",
      /*occupants=*/"1",
      /*range=*/"near",
      /*bucket_uptime_s=*/1200,
      /*device_id=*/"sense01", out, sizeof(out));
  check(n > 0, "build_sense_canonical wrote something");
  const std::string want =
      "securacv-canary-sig|v1|sense|sense01|3|presence_detected|present|1|near|1200";
  check_eq(std::string(out, n), want, "sense canonical matches reference");
}

void test_sense_canonical_occupancy_change() {
  char out[256];
  const size_t n = device_signature::build_sense_canonical(
      4, "occupancy_changed", "present", "2+", "mid", 1800,
      "sense01", out, sizeof(out));
  check(n > 0, "occupancy canonical wrote something");
  check_eq(std::string(out, n),
           "securacv-canary-sig|v1|sense|sense01|4|occupancy_changed|present|2+|mid|1800",
           "occupancy canonical matches reference");
}

void test_canonical_truncation_returns_zero() {
  char out[16];
  const size_t n = device_signature::build_sense_canonical(
      3, "presence_detected", "present", "1", "near", 1200,
      "sense01", out, sizeof(out));
  check(n == 0, "truncated canonical returns 0");
  check(out[0] == '\0', "truncated canonical clears the buffer");
}

/* ── b64url encoder ─────────────────────────────────────────────── */

void test_b64url_known_vectors() {
  /* RFC 4648 test vectors, translated to the URL alphabet, no pad. */
  struct Vec { const char* in; const char* want; };
  const Vec vecs[] = {
    {"", ""},
    {"f", "Zg"},
    {"fo", "Zm8"},
    {"foo", "Zm9v"},
    {"foob", "Zm9vYg"},
    {"fooba", "Zm9vYmE"},
    {"foobar", "Zm9vYmFy"},
  };
  for (const auto& v : vecs) {
    char out[32];
    const size_t n = device_signature::b64url_encode_nopad(
        reinterpret_cast<const uint8_t*>(v.in), std::strlen(v.in),
        out, sizeof(out));
    check_eq(std::string(out, n), v.want, "b64url RFC vector");
  }
}

void test_b64url_url_alphabet() {
  /* 0xfb 0xff maps to '-_8' territory in the URL alphabet — proves the
   * '+'→'-' and '/'→'_' translation runs. */
  const uint8_t in[] = {0xfb, 0xef, 0xff};
  char out[16];
  const size_t n = device_signature::b64url_encode_nopad(in, sizeof(in),
                                                         out, sizeof(out));
  check(n == 4, "3 bytes encode to 4 chars");
  check(std::strchr(out, '+') == nullptr && std::strchr(out, '/') == nullptr,
        "no standard-alphabet chars in b64url output");
  check(std::strchr(out, '=') == nullptr, "no padding in b64url output");
  check_eq(std::string(out, n), "--__", "b64url translation vector");
}

void test_b64url_sig_length() {
  uint8_t sig[64];
  for (int i = 0; i < 64; ++i) sig[i] = (uint8_t)(255 - i);
  char out[device_signature::SIG_B64URL_CAP];
  const size_t n = device_signature::b64url_encode_nopad(sig, sizeof(sig),
                                                         out, sizeof(out));
  check(n == device_signature::SIG_B64URL_LEN,
        "64-byte sig encodes to exactly 86 b64url chars");
}

}  // namespace

int main() {
  test_chain_canonical_known_input();
  test_event_canonical_known_input();
  test_counts_canonical_known_input();
  test_sense_canonical_known_input();
  test_sense_canonical_occupancy_change();
  test_canonical_truncation_returns_zero();
  test_b64url_known_vectors();
  test_b64url_url_alphabet();
  test_b64url_sig_length();
  std::puts("ALL DEVICE SIGNATURE (COMMON) TESTS PASSED");
  return 0;
}
