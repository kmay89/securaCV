// Host test: the setup network's key is refused, not made up, when its HMAC
// fails (firmware/common/network/ap_password.h, sweep F350 review).
//
// fleet_key's HMAC zeroes its output when the primitive fails; on the device
// that is mbedtls_md_hmac() reporting an allocation failure. The first cut of
// derive_ap_password() rendered those 32 zero bytes and returned true, so the
// key was "cv-222222222222" on every unit, and setup() would have raised the
// setup network, whose peers are handed the page token, on a key anyone can
// type. This suite runs over hmac_fail_seam.h's primitive and is built twice
// (Makefile): once over fleet_key.h's OpenSSL branch, and once with -DARDUINO
// over its mbedTLS branch, the one the device ships. Each build holds:
//   - the checked HMAC reports each failure its library can give and zeroes
//     its output; the unchecked form zeroes it and says nothing, as before;
//   - derive_ap_password() refuses each failure, for two different keys, and
//     leaves "" with nothing written past the first byte, even over a buffer
//     that held a key before;
//   - over the real primitive, the same build derives test_ap_password's
//     known answer and says so (so the refusal is the failure's, not a branch
//     that always refuses);
//   - every call reached the seam, so a pass is the refusal's, not a real
//     HMAC's.
// test_ap_password.cpp holds the derivation itself, over the real HMAC.
//
// Build/run: make -C firmware/tests_host (links OpenSSL in both builds).

#include "../common/network/ap_password.h"
#include "hmac_fail_seam.h"

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

using seam::Mode;
using seam::name_of;

// test_ap_password.cpp's known answer: privkey bytes 0x01..0x20.
static constexpr const char* kKnownAnswer = "cv-e8Qun72aUeCc";

static void known_key(uint8_t (&k)[32]) {
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i + 1);
}

// ── tests ──────────────────────────────────────────────────────────────────
static void the_checked_hmac_reports_the_failure(Mode f) {
  seam::g_mode = f;
  const int before = seam::g_calls;
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
  CHECK(seam::g_calls > before, "%s: the primitive under test was not called", name_of(f));

  // The unchecked form keeps what it always did: zeroed output, nothing said.
  std::memset(out, 0xEE, sizeof(out));
  canary::net::fleet_key::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>("m"), 1,
                                      out);
  for (size_t i = 0; i < sizeof(out); i++) {
    CHECK(out[i] == 0, "%s: unchecked output byte %zu is 0x%02x", name_of(f), i, out[i]);
  }
}

static void the_key_is_refused(Mode f) {
  seam::g_mode = f;
  uint8_t a[32], b[32];
  known_key(a);
  for (size_t i = 0; i < 32; i++) b[i] = (uint8_t)(0xF0 ^ i);  // a second unit
  for (const uint8_t (*k)[32] : {&a, &b}) {
    const int before = seam::g_calls;
    char out[32];
    std::memset(out, 'Z', sizeof(out));
    const bool ok = canary::net::derive_ap_password(*k, out, sizeof(out));
    CHECK(!ok, "%s: derive_ap_password returned true with key %.*s", name_of(f), 15, out);
    CHECK(out[0] == '\0', "%s: a refusal left \"%.*s\", not \"\"", name_of(f), 15, out);
    for (size_t i = 1; i < sizeof(out); i++) {
      CHECK(out[i] == 'Z', "%s: the refusal wrote byte %zu", name_of(f), i);
    }
    CHECK(seam::g_calls > before, "%s: the derivation did not reach the primitive under test",
          name_of(f));
  }

  // A buffer that already held a key: the refusal leaves it reading "", so
  // no caller can go on to raise the network on the old one.
  char held[16];
  std::snprintf(held, sizeof(held), "%s", kKnownAnswer);
  CHECK(!canary::net::derive_ap_password(a, held, sizeof(held)), "%s: refused over a held key",
        name_of(f));
  CHECK(held[0] == '\0', "%s: a refusal over a held key left \"%s\"", name_of(f), held);
}

// The same build over the real primitive derives the known answer: the branch
// under test reports success when the HMAC ran, and its output is used.
static void the_real_primitive_derives_the_known_answer() {
  seam::g_mode = Mode::Pass;
  const int before = seam::g_calls;
  uint8_t a[32];
  known_key(a);
  char out[16];
  std::memset(out, 'Z', sizeof(out));
  const bool ok = canary::net::derive_ap_password(a, out, sizeof(out));
  CHECK(ok, "pass: derive_ap_password refused over a working HMAC");
  CHECK(std::strcmp(out, kKnownAnswer) == 0, "pass: got \"%.*s\", want %s", 15, out, kKnownAnswer);
  CHECK(seam::g_calls > before, "pass: the derivation did not reach the primitive under test");
}

int main() {
  for (Mode f : seam::kFailures) {
    the_checked_hmac_reports_the_failure(f);
    the_key_is_refused(f);
  }
  the_real_primitive_derives_the_known_answer();
  if (g_failures) {
    std::printf("test_ap_password_hmac_fail (%s): %d failure(s)\n", seam::kBranch, g_failures);
    return 1;
  }
  std::printf("test_ap_password_hmac_fail: OK (%s: each failed HMAC is reported and the setup "
              "network's key is refused; the working HMAC gives the known answer; %d primitive "
              "calls)\n",
              seam::kBranch, seam::g_calls);
  return 0;
}
