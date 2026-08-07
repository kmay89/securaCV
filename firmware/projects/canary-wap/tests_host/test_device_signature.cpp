/**
 * @file test_device_signature.cpp
 * @brief Host-side test for the canonical-message builders + b64url
 *        encoder in device_signature.cpp.
 *
 * We can't exercise the Ed25519 sign path on the host without pulling
 * in the Arduino Crypto library, so this test focuses on the
 * deterministic parts: the canonical string bytes that go into the
 * signer, and the b64url encoder that wraps the 64-byte sig output.
 *
 * The full sign+verify round-trip is exercised on the HA side via
 * pytest (see custom_components/securacv/tests/test_signature.py),
 * which uses Python's `cryptography` library to produce a known
 * signature and verify a known-good canonical string.
 *
 * Run via: `make -C firmware/projects/canary-wap/tests_host`.
 */

#include "../arduino/canary_wap/device_signature.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
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
  /* Inputs chosen for cross-language verification — the HA test rebuilds
   * this exact byte sequence and signs it with a known Ed25519 privkey. */
  uint8_t hash[32];
  for (int i = 0; i < 32; ++i) hash[i] = (uint8_t)i;
  char out[256];
  const size_t n = device_signature::build_chain_canonical(
      /*length=*/42, hash, /*device_id=*/"abc123", out, sizeof(out));
  check(n > 0, "build_chain_canonical wrote something");
  const std::string got(out, n);
  /* Lowercase hex, pipe-separated, no trailing newline. */
  const std::string want =
      "securacv-canary-sig|v1|chain|abc123|42|"
      "000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f";
  check_eq(got, want, "chain canonical matches reference");
}

void test_event_canonical_known_input() {
  char out[256];
  const size_t n = device_signature::build_event_canonical(
      /*event_id=*/7,
      /*state=*/"active",
      /*category=*/"event",
      /*privacy=*/"p1",
      /*motion=*/180,
      /*breath=*/120,
      /*bpm=*/15,
      /*device_id=*/"abc123",
      out, sizeof(out));
  check(n > 0, "build_event_canonical wrote something");
  const std::string got(out, n);
  const std::string want =
      "securacv-canary-sig|v1|event|abc123|7|active|event|p1|180|120|15";
  check_eq(got, want, "event canonical matches reference");
}

void test_counts_canonical_known_input() {
  char out[128];
  const size_t n = device_signature::build_counts_canonical(
      /*total=*/12345, /*device_id=*/"abc123", out, sizeof(out));
  check(n > 0, "build_counts_canonical wrote something");
  const std::string got(out, n);
  const std::string want = "securacv-canary-sig|v1|counts|abc123|12345";
  check_eq(got, want, "counts canonical matches reference");
}

void test_canonical_truncation_is_safe() {
  /* A 16-byte buffer is too small for any of the canonicals (the
   * "securacv-canary-sig|v1|..." prefix alone is 21+ bytes). Builders
   * must return 0 and leave a clean empty string rather than emit a
   * truncated message that would silently fail verification. */
  uint8_t hash[32] = {};
  char out[16];
  const size_t n = device_signature::build_chain_canonical(
      1, hash, "abc", out, sizeof(out));
  check(n == 0, "truncation returns 0");
  check(out[0] == '\0', "truncated buffer is empty-string");
}

/* ── b64url encoder ─────────────────────────────────────────────── */

void test_b64url_known_vectors() {
  struct Vec {
    const char* hex;
    const char* expected;
  };
  /* RFC 4648 §10 test vectors, transformed to b64url-no-pad. The HA
   * verifier uses Python's `base64.urlsafe_b64decode(s + "=" * pad)`
   * which inverts these exact strings. */
  Vec vecs[] = {
      { "",         ""       },
      { "66",       "Zg"     },  /* "f" */
      { "666f",     "Zm8"    },  /* "fo" */
      { "666f6f",   "Zm9v"   },  /* "foo" */
      { "666f6f62", "Zm9vYg" },  /* "foob" */
      /* A 64-byte input — same shape as an Ed25519 sig. We just check
       * the output length (86) since the exact bytes depend on the
       * input. */
  };
  for (const auto& v : vecs) {
    uint8_t bin[64] = {};
    size_t blen = std::strlen(v.hex) / 2;
    for (size_t i = 0; i < blen; ++i) {
      char b[3] = { v.hex[2*i], v.hex[2*i + 1], 0 };
      bin[i] = (uint8_t)std::strtoul(b, nullptr, 16);
    }
    char enc[128] = {};
    const size_t w = device_signature::b64url_encode_nopad(
        bin, blen, enc, sizeof(enc));
    check_eq(std::string(enc, w), std::string(v.expected),
             "b64url RFC vector");
  }

  /* 64-byte zero input → predictable 86-char output. The exact bytes
   * are computed by Python's `base64.urlsafe_b64encode(bytes(64)).rstrip(b"=")`
   * = 86 'A's plus "..AA". We just assert the length here; the HA test
   * cross-checks the bytes. */
  uint8_t zeros[64] = {};
  char enc[128] = {};
  const size_t w = device_signature::b64url_encode_nopad(
      zeros, sizeof(zeros), enc, sizeof(enc));
  check(w == device_signature::SIG_B64URL_LEN, "64-byte input → 86 chars");
}

void test_b64url_url_safe_alphabet() {
  /* Input chosen so standard b64 produces '+' and '/'. The url-safe
   * encoder must emit '-' and '_' in their place. */
  const uint8_t in[3] = { 0xFB, 0xFF, 0xBF };
  /* Standard b64 of {0xFB,0xFF,0xBF} = "+/+/" */
  char enc[16] = {};
  const size_t w = device_signature::b64url_encode_nopad(
      in, sizeof(in), enc, sizeof(enc));
  check(w == 4, "3-byte input → 4 chars (no padding)");
  const std::string got(enc, w);
  check_eq(got, "-_-_", "+ and / translated to - and _");
}

void test_b64url_output_truncation_is_safe() {
  /* If the caller hands us a buffer too small for the output, the
   * encoder must return 0 and leave an empty string — never produce
   * a partial b64 string a verifier would treat as a sig of the
   * wrong length. */
  const uint8_t in[3] = { 0xAA, 0xBB, 0xCC };
  char enc[2];  /* 4-char output won't fit. */
  const size_t w = device_signature::b64url_encode_nopad(
      in, sizeof(in), enc, sizeof(enc));
  check(w == 0, "tight buffer → returns 0");
  check(enc[0] == '\0', "tight buffer → empty");
}


/* ââ whoami presence proof: canonical + the nonce gate âââââââââââ */

void test_whoami_canonical_known_input() {
  /* The Flasher's Rust verifier rebuilds this exact byte sequence before
   * checking the device's Ed25519 sig over it â a drift here silently
   * breaks fleet-book verification, so the bytes are pinned. */
  char out[256];
  const size_t n = device_signature::build_whoami_canonical(
      "00112233445566778899aabbccddeeff", "canary_wap_a1b2", out, sizeof(out));
  check(n > 0, "build_whoami_canonical wrote something");
  check_eq(std::string(out, n),
           "securacv-canary-sig|v1|whoami|canary_wap_a1b2|"
           "00112233445566778899aabbccddeeff",
           "whoami canonical bytes");
}

void test_whoami_nonce_gate() {
  /* The identity key must never sign attacker-shaped bytes: 16-64 chars,
   * lowercase hex only. Everything else is refused before the signer. */
  check(device_signature::whoami_nonce_ok("00112233445566778899aabbccddeeff"),
        "32-char lowercase hex accepted");
  check(device_signature::whoami_nonce_ok("0123456789abcdef"),
        "16-char minimum accepted");
  check(device_signature::whoami_nonce_ok(std::string(64, 'a').c_str()),
        "64-char maximum accepted");
  check(!device_signature::whoami_nonce_ok("0123456789abcde"),
        "15 chars refused (too short)");
  check(!device_signature::whoami_nonce_ok(std::string(65, 'a').c_str()),
        "65 chars refused (too long)");
  check(!device_signature::whoami_nonce_ok("0123456789ABCDEF"),
        "uppercase hex refused (canonical must be deterministic)");
  check(!device_signature::whoami_nonce_ok("0123456789abcdeg"),
        "non-hex char refused");
  check(!device_signature::whoami_nonce_ok("0123456789abcd|f"),
        "canonical separator char refused");
  check(!device_signature::whoami_nonce_ok(""), "empty refused");
  check(!device_signature::whoami_nonce_ok(nullptr), "null refused");
}

void test_whoami_sign_refuses_bad_nonce_even_direct() {
  /* Belt and suspenders: sign_whoami itself re-validates, so no future
   * caller can route an unvalidated nonce to the key. (On the host the
   * sign path stubs to false anyway; the refusal must come first and
   * must not touch the output buffer size contract.) */
  char sig[device_signature::SIG_HEX_CAP];
  check(!device_signature::sign_whoami("NOT-HEX!", sig, sizeof(sig)),
        "malformed nonce refused by sign_whoami");
  check(!device_signature::sign_whoami("00112233445566778899aabbccddeeff",
                                       sig, 10),
        "undersized sig buffer refused");
}

}  /* anonymous namespace */

int main() {
  test_chain_canonical_known_input();
  test_event_canonical_known_input();
  test_counts_canonical_known_input();
  test_canonical_truncation_is_safe();
  test_b64url_known_vectors();
  test_b64url_url_safe_alphabet();
  test_b64url_output_truncation_is_safe();
  test_whoami_canonical_known_input();
  test_whoami_nonce_gate();
  test_whoami_sign_refuses_bad_nonce_even_direct();
  std::printf("OK  all device_signature tests passed\n");
  return 0;
}
