// Host test: the setup network's key is refused, not made up, when its HMAC
// fails (firmware/common/network/ap_password.h, sweep F350 review).
//
// fleet_key's HMAC zeroes its output when the primitive fails; on the device
// that is mbedtls_md_hmac() reporting an allocation failure. The first cut of
// derive_ap_password() rendered those 32 zero bytes and returned true, so the
// key was "cv-222222222222" on every unit, and setup() would have raised the
// setup network, whose peers are handed the page token, on a key anyone can
// type. This binary defines OpenSSL's one-shot HMAC() itself (the program's
// own definition wins over libcrypto's at link time), which stands in for the
// device-side failure, and holds:
//   - the checked HMAC reports both failures OpenSSL can give (no result; a
//     result of the wrong length) and zeroes its output;
//   - derive_ap_password() refuses both, for two different keys, and leaves
//     "" with nothing written past the first byte, even over a buffer that
//     held a key before;
//   - every call went through this HMAC(), so a pass is the refusal's, not a
//     real HMAC's.
// test_ap_password.cpp holds the derivation itself, over the real HMAC.
//
// Build/run: make -C firmware/tests_host (links OpenSSL for EVP_sha256()).

#include "../common/network/ap_password.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

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

// ── the failing HMAC() ─────────────────────────────────────────────────────
enum class Fail { NoResult, ShortResult };
static Fail g_fail = Fail::NoResult;
static int g_calls = 0;

extern "C" unsigned char* HMAC(const EVP_MD* /*evp_md*/, const void* /*key*/, int /*key_len*/,
                               const unsigned char* /*data*/, size_t /*data_len*/,
                               unsigned char* md, unsigned int* md_len) {
  ++g_calls;
  if (g_fail == Fail::NoResult) return nullptr;
  // A result, but 20 bytes of it: what a SHA-1 HMAC would hand back.
  std::memset(md, 0xA5, 20);
  if (md_len != nullptr) *md_len = 20;
  return md;
}

static const char* name_of(Fail f) { return f == Fail::NoResult ? "no result" : "short result"; }

// ── tests ──────────────────────────────────────────────────────────────────
static void the_checked_hmac_reports_the_failure(Fail f) {
  g_fail = f;
  const int before = g_calls;
  uint8_t key[32];
  std::memset(key, 0x11, sizeof(key));
  uint8_t out[canary::net::fleet_key::HASH_BYTES];
  std::memset(out, 0xEE, sizeof(out));
  const bool ran = canary::net::fleet_key::hmac_sha256_checked(
      key, sizeof(key), reinterpret_cast<const uint8_t*>("m"), 1, out);
  CHECK(!ran, "%s: the checked HMAC said it ran", name_of(f));
  for (size_t i = 0; i < sizeof(out); i++) {
    CHECK(out[i] == 0, "%s: output byte %zu is 0x%02x, not zeroed", name_of(f), i, out[i]);
  }
  CHECK(g_calls == before + 1, "%s: the HMAC() under test was not called", name_of(f));

  // The unchecked form keeps what it always did: zeroed output, nothing said.
  std::memset(out, 0xEE, sizeof(out));
  canary::net::fleet_key::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>("m"), 1,
                                      out);
  for (size_t i = 0; i < sizeof(out); i++) {
    CHECK(out[i] == 0, "%s: unchecked output byte %zu is 0x%02x", name_of(f), i, out[i]);
  }
}

static void the_key_is_refused(Fail f) {
  g_fail = f;
  uint8_t a[32], b[32];
  for (size_t i = 0; i < 32; i++) {
    a[i] = (uint8_t)(i + 1);     // test_ap_password's known-answer key
    b[i] = (uint8_t)(0xF0 ^ i);  // a second unit
  }
  for (const uint8_t (*k)[32] : {&a, &b}) {
    const int before = g_calls;
    char out[32];
    std::memset(out, 'Z', sizeof(out));
    const bool ok = canary::net::derive_ap_password(*k, out, sizeof(out));
    CHECK(!ok, "%s: derive_ap_password returned true with key %s", name_of(f), out);
    CHECK(out[0] == '\0', "%s: a refusal left \"%.*s\", not \"\"", name_of(f), 15, out);
    for (size_t i = 1; i < sizeof(out); i++) {
      CHECK(out[i] == 'Z', "%s: the refusal wrote byte %zu", name_of(f), i);
    }
    CHECK(g_calls > before, "%s: the derivation did not reach the HMAC() under test", name_of(f));
  }

  // A buffer that already held a key: the refusal leaves it reading "", so
  // no caller can go on to raise the network on the old one.
  char held[16];
  std::snprintf(held, sizeof(held), "%s", "cv-e8Qun72aUeCc");
  CHECK(!canary::net::derive_ap_password(a, held, sizeof(held)), "%s: refused over a held key",
        name_of(f));
  CHECK(held[0] == '\0', "%s: a refusal over a held key left \"%s\"", name_of(f), held);
}

int main() {
  for (Fail f : {Fail::NoResult, Fail::ShortResult}) {
    the_checked_hmac_reports_the_failure(f);
    the_key_is_refused(f);
  }
  if (g_failures) {
    std::printf("test_ap_password_hmac_fail: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_ap_password_hmac_fail: OK (a failed HMAC, no result or a short one, is "
              "reported and the setup network's key is refused, %d HMAC() calls)\n",
              g_calls);
  return 0;
}
