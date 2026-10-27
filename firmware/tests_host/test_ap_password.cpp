// Host tests for firmware/common/network/ap_password.h — the key of a
// Canary's own setup network, an HMAC of the device private key (sweep F350).
//
// Until F350 the flagship (firmware/canary) built its SoftAP key as "cv-"
// plus 5 characters of its public-key fingerprint, which the device
// publishes (the SSID suffix spells two of its bytes; every signed MQTT event
// body, the receipt, /api/status and the serial log carry all eight). Its
// setup() now calls canary::net::derive_ap_password(device.privkey, ...).
// What this suite pins:
//   - known answers, computed independently (Python's hmac over the same
//     domain label and alphabet rule);
//   - the format over 256 keys: "cv-" + 12 characters, 15 in all, each from
//     canary-wap's unambiguous alphabet (cut verbatim out of the sketch);
//   - every one-bit change of the key changes the result;
//   - parity: canary-wap's own derive_ap_password, cut verbatim out of
//     canary_wap.ino by cut_functions.awk (with the sketch's alphabet and
//     secure_zero; its mbedTLS HMAC is stood in by fleet_key's), gives the
//     same key for every one of those keys;
//   - refusals: a buffer below 16 bytes or a null one is refused, and a
//     refusal writes nothing past the first byte;
//   - the signature: the real DeviceIdentity's 8-byte pubkey_fp does not
//     bind to the key parameter and its privkey does (static_asserts), so a
//     revert of the call to the fingerprint is a compile error. The 32-byte
//     public key still binds; that gap is the call site's to close.
// A failed HMAC (refused, never rendered as a key) is
// test_ap_password_hmac_fail.cpp's: it replaces HMAC() for its whole binary.
//
// Build/run: make -C firmware/tests_host (the Makefile cuts the WAP's
// function and links OpenSSL).

#include "../common/network/ap_password.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include "securacv_witness.h"  // the real DeviceIdentity, over stubs/witness_glue

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

// ── canary-wap's derivation, verbatim ──────────────────────────────────────
// build/ap_password_wap.inc holds the sketch's UNAMBIGUOUS_ALPHABET and
// UNAMBIGUOUS_LEN lines, its secure_zero() and its derive_ap_password(), each
// under a #line naming canary_wap.ino. The sketch's hmac_sha256() calls
// mbedTLS and answers whether it ran (sweep F359); this stand-in is the same
// HMAC-SHA256 over OpenSSL, with the same answer. The sketch's refusal of a
// failed HMAC is projects/canary-wap/tests_host/test_wap_hmac_fail.cpp's.
namespace wap {
static bool hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len,
                        uint8_t out[32]) {
  return canary::net::fleet_key::hmac_sha256_checked(key, key_len, data, data_len, out);
}
#include "ap_password_wap.inc"
}  // namespace wap

// ── the signature ──────────────────────────────────────────────────────────
using Derive = decltype(&canary::net::derive_ap_password);
using PrivkeyRef = decltype((std::declval<DeviceIdentity&>().privkey));
using PubkeyRef = decltype((std::declval<DeviceIdentity&>().pubkey));
using FingerprintRef = decltype((std::declval<DeviceIdentity&>().pubkey_fp));
static_assert(std::is_same_v<FingerprintRef, uint8_t (&)[8]>, "DeviceIdentity::pubkey_fp moved");
static_assert(std::is_invocable_v<Derive, PrivkeyRef, char*, size_t>,
              "DeviceIdentity::privkey binds: main.cpp's setup() call compiles");
static_assert(std::is_invocable_v<Derive, const uint8_t (&)[32], char*, size_t>,
              "a const 32-byte key binds");
static_assert(!std::is_invocable_v<Derive, FingerprintRef, char*, size_t>,
              "DeviceIdentity::pubkey_fp must not bind");
static_assert(!std::is_invocable_v<Derive, const uint8_t (&)[8], char*, size_t>,
              "a fingerprint must not bind");
static_assert(!std::is_invocable_v<Derive, const uint8_t*, char*, size_t>,
              "a pointer (a decayed array of any length) must not bind");
// The documented gap: the public key is 32 bytes too. The call site in
// main.cpp's setup() is what names the private key.
static_assert(std::is_invocable_v<Derive, PubkeyRef, char*, size_t>,
              "if this fails, the gap the header comment names is closed: update it");

static_assert(canary::net::AP_PASSWORD_LEN == 15, "cv- + 12");
static_assert(canary::net::AP_PASSWORD_DOMAIN_LEN == 23, "the WAP hashes 23 bytes of its label");

// ── fixtures ───────────────────────────────────────────────────────────────
static const size_t kKeys = 256;

// Key `i` of the deterministic set: HMAC-SHA256 of the index under a test
// label, so the bytes are spread like a real key's.
static void key_of(size_t i, uint8_t (&k)[32]) {
  static const char kLabel[] = "test_ap_password:key";
  const uint8_t idx[2] = {(uint8_t)(i >> 8), (uint8_t)i};
  canary::net::fleet_key::hmac_sha256((const uint8_t*)kLabel, sizeof(kLabel) - 1, idx,
                                      sizeof(idx), k);
}

static std::string derive(const uint8_t (&k)[32]) {
  char out[16];
  std::memset(out, 'Z', sizeof(out));
  if (!canary::net::derive_ap_password(k, out, sizeof(out))) return "<refused>";
  return std::string(out);
}

static std::string derive_wap(const uint8_t (&k)[32]) {
  char out[16];
  std::memset(out, 'Z', sizeof(out));
  if (!wap::derive_ap_password(k, out, sizeof(out))) return "<refused>";
  return std::string(out);
}

// ── tests ──────────────────────────────────────────────────────────────────
static void known_answers() {
  uint8_t k[32];
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i + 1);
  CHECK(derive(k) == "cv-e8Qun72aUeCc", "key 0x01..0x20: %s", derive(k).c_str());
  uint8_t z[32] = {0};
  CHECK(derive(z) == "cv-3jByJ6rHdhM2", "key of 32 zero bytes: %s", derive(z).c_str());
}

static void format_over_256_keys() {
  std::set<std::string> seen;
  for (size_t i = 0; i < kKeys; i++) {
    uint8_t k[32];
    key_of(i, k);
    const std::string pw = derive(k);
    CHECK(pw.size() == canary::net::AP_PASSWORD_LEN, "key %zu: %zu characters (%s)", i, pw.size(),
          pw.c_str());
    CHECK(pw.compare(0, 3, "cv-") == 0, "key %zu: no cv- prefix (%s)", i, pw.c_str());
    for (size_t c = 3; c < pw.size(); c++) {
      CHECK(std::memchr(wap::UNAMBIGUOUS_ALPHABET, pw[c], wap::UNAMBIGUOUS_LEN) != nullptr,
            "key %zu: '%c' is not in the unambiguous alphabet", i, pw[c]);
      CHECK(std::strchr("0Oo1IilL", pw[c]) == nullptr, "key %zu: confusable '%c'", i, pw[c]);
    }
    seen.insert(pw);
  }
  CHECK(seen.size() == kKeys, "256 keys gave %zu distinct passwords", seen.size());
  CHECK(wap::UNAMBIGUOUS_LEN == 54, "the sketch's alphabet is %zu characters", wap::UNAMBIGUOUS_LEN);
}

static void one_bit_changes_the_key() {
  uint8_t k[32];
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i + 1);
  const std::string base = derive(k);
  for (size_t bit = 0; bit < 256; bit++) {
    uint8_t f[32];
    std::memcpy(f, k, sizeof(f));
    f[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    CHECK(derive(f) != base, "flipping bit %zu left the password at %s", bit, base.c_str());
  }
}

static void equal_to_canary_wap() {
  for (size_t i = 0; i < kKeys; i++) {
    uint8_t k[32];
    key_of(i, k);
    CHECK(derive(k) == derive_wap(k), "key %zu: shared %s, canary-wap %s", i, derive(k).c_str(),
          derive_wap(k).c_str());
  }
  uint8_t k[32];
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i + 1);
  CHECK(derive_wap(k) == "cv-e8Qun72aUeCc", "canary-wap, key 0x01..0x20: %s", derive_wap(k).c_str());
  uint8_t ff[32];
  std::memset(ff, 0xff, sizeof(ff));
  CHECK(derive(ff) == derive_wap(ff), "key of 0xff bytes: shared %s, canary-wap %s",
        derive(ff).c_str(), derive_wap(ff).c_str());
}

static void refusals() {
  uint8_t k[32];
  key_of(7, k);
  char buf[32];

  std::memset(buf, 'Z', sizeof(buf));
  CHECK(!canary::net::derive_ap_password(k, buf, canary::net::AP_PASSWORD_LEN),
        "a 15-byte buffer (no room for the NUL) is refused");
  CHECK(buf[0] == '\0', "a refusal writes \"\"");
  for (size_t i = 1; i < sizeof(buf); i++) {
    CHECK(buf[i] == 'Z', "a refusal wrote byte %zu", i);
  }

  std::memset(buf, 'Z', sizeof(buf));
  CHECK(!canary::net::derive_ap_password(k, buf, 0), "a zero-length buffer is refused");
  CHECK(buf[0] == 'Z', "a zero-length refusal writes nothing");

  CHECK(!canary::net::derive_ap_password(k, nullptr, 64), "a null buffer is refused");

  std::memset(buf, 'Z', sizeof(buf));
  CHECK(canary::net::derive_ap_password(k, buf, canary::net::AP_PASSWORD_LEN + 1),
        "a 16-byte buffer (main.cpp's g_ap_password) is enough");
  CHECK(std::strlen(buf) == canary::net::AP_PASSWORD_LEN, "16-byte buffer: %s", buf);
  for (size_t i = canary::net::AP_PASSWORD_LEN + 1; i < sizeof(buf); i++) {
    CHECK(buf[i] == 'Z', "a 16-byte call wrote byte %zu", i);
  }

  char wide[65];  // ScvNetworkManager::m_ap_password
  CHECK(canary::net::derive_ap_password(k, wide, sizeof(wide)), "a 65-byte buffer");
  CHECK(std::string(wide) == std::string(buf), "the buffer size does not change the key");
}

int main() {
  known_answers();
  format_over_256_keys();
  one_bit_changes_the_key();
  equal_to_canary_wap();
  refusals();
  if (g_failures) {
    std::printf("test_ap_password: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_ap_password: OK (known answers, 256-key format and parity with canary-wap, "
              "256 one-bit changes, refusals)\n");
  return 0;
}
