/* Host tests for the key-at-rest policy (firmware/common/identity/key_at_rest.h).
 *
 * Every test here is about a way a device could misstate where its identity
 * key sleeps, or a way an image could act against its own promise. The module
 * has one job — turn {flash encryption, secure boot, require_fe} into a tier,
 * a wire label and an allow/refuse decision — and each rule exists because the
 * alternative is either a stranded default board or a Tier-3 image quietly
 * using a plaintext key. So the suite is organized by the failure prevented.
 *
 * Build & run (via firmware/tests_host/Makefile, mirrors the CI contract):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../common test_key_at_rest.cpp
 */
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "identity/key_at_rest.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

using key_at_rest::Facts;
using key_at_rest::Tier;
using key_at_rest::Decision;

static Facts facts(bool fe, bool sb, bool req) {
  Facts f;
  f.flash_encryption = fe;
  f.secure_boot = sb;
  f.require_fe = req;
  return f;
}

// ── the failure: a stranded default board ───────────────────────────────────
//
// Rule 1. The default Canary sits at Tier 0 by decision. On ordinary hardware
// it must store and load its key, and it must say so once — a warning, not a
// refusal. A refusal here would halt provisioning on every dev board.
static void test_default_fe_off_allows_and_warns() {
  Decision d = key_at_rest::decide(facts(false, false, false));
  CHECK(d.allow_persist);
  CHECK(d.allow_load);
  CHECK(d.warn_once);
  CHECK(d.reason != nullptr);
  CHECK(key_at_rest::classify(facts(false, false, false)) == Tier::PlaintextNvs);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::PlaintextNvs), "plaintext-nvs") == 0);
}

// ── the failure: warning about a key that is already protected ──────────────
//
// A flash-encryption-on board is Tier 3: allowed, and nothing to warn about.
static void test_default_fe_on_allows_no_warn() {
  Decision d = key_at_rest::decide(facts(true, false, false));
  CHECK(d.allow_persist);
  CHECK(d.allow_load);
  CHECK(!d.warn_once);
  CHECK(d.reason == nullptr);
  CHECK(key_at_rest::classify(facts(true, false, false)) == Tier::FlashEncrypted);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::FlashEncrypted), "flash-encrypted") == 0);
}

// ── Secure Boot on top of flash encryption is Tier 4, and says so ───────────
static void test_fe_plus_secure_boot_label() {
  CHECK(key_at_rest::classify(facts(true, true, false)) == Tier::FlashEncryptedSecureBoot);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::FlashEncryptedSecureBoot),
                    "flash-encrypted+secure-boot") == 0);
  Decision d = key_at_rest::decide(facts(true, true, false));
  CHECK(d.allow_persist && d.allow_load && !d.warn_once);
}

// ── the failure: a Tier-3 image using a plaintext key ───────────────────────
//
// Rule 2. An image that promised flash encryption must refuse BOTH halves on
// hardware without it: never write a key (persist) and never use one it finds
// (load). Refusing only one would let a key a default image wrote be used by
// the image that promised it never would be. The reason is non-null so the
// bench operator sees why provisioning halted.
static void test_require_fe_off_refuses_symmetrically() {
  Decision d = key_at_rest::decide(facts(false, false, true));
  CHECK(d.allow_persist == false);
  CHECK(d.allow_load == false);
  CHECK(d.allow_persist == d.allow_load);   // symmetry, pinned
  CHECK(d.reason != nullptr);
  CHECK(!d.warn_once);                      // a refusal is not a warning
  // Secure Boot without flash encryption changes nothing here.
  Decision d2 = key_at_rest::decide(facts(false, true, true));
  CHECK(!d2.allow_persist && !d2.allow_load);
}

// ── the opt-in image on the hardware it was built for: allowed, quiet ───────
static void test_require_fe_on_allows() {
  Decision d = key_at_rest::decide(facts(true, false, true));
  CHECK(d.allow_persist);
  CHECK(d.allow_load);
  CHECK(!d.warn_once);
  CHECK(d.reason == nullptr);
  Decision d2 = key_at_rest::decide(facts(true, true, true));
  CHECK(d2.allow_persist && d2.allow_load && !d2.warn_once);
}

// ── the failure: calling Secure Boot an at-rest protection ──────────────────
//
// Secure Boot stops unsigned firmware running; it encrypts nothing. A board
// with Secure Boot and no flash encryption still holds its key in plaintext.
static void test_secure_boot_alone_is_still_plaintext() {
  CHECK(key_at_rest::classify(facts(false, true, false)) == Tier::PlaintextNvs);
  Decision d = key_at_rest::decide(facts(false, true, false));
  CHECK(d.allow_persist && d.allow_load && d.warn_once);
}

// ── the labels are wire fields: plain ASCII, lower-case, no spaces, and they
//    never name the thing they describe as secret material ──────────────────
static void check_label_shape(const char* s) {
  CHECK(s != nullptr);
  CHECK(std::strlen(s) > 0);
  for (const char* c = s; *c; ++c) {
    unsigned char u = (unsigned char)*c;
    CHECK(u < 0x80);                       // 7-bit ASCII
    CHECK(u != ' ');                       // no spaces
    CHECK(!(u >= 'A' && u <= 'Z'));        // lower-case
  }
  CHECK(std::strstr(s, "priv") == nullptr);
  CHECK(std::strstr(s, "secret") == nullptr);
}

static void test_wire_labels_are_wire_safe() {
  const Tier tiers[] = { Tier::PlaintextNvs, Tier::FlashEncrypted,
                         Tier::FlashEncryptedSecureBoot, Tier::HardwareBound };
  for (Tier t : tiers) check_label_shape(key_at_rest::wire_label(t));
  // Distinct tiers, distinct labels.
  for (Tier a : tiers)
    for (Tier b : tiers)
      if (a != b) CHECK(std::strcmp(key_at_rest::wire_label(a), key_at_rest::wire_label(b)) != 0);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::HardwareBound), "hw-bound") == 0);
  // The console labels may have spaces, but must also never call the key secret
  // material (regression_check.sh greps serial lines for that).
  for (Tier t : tiers) {
    const char* c = key_at_rest::console_label(t);
    CHECK(c != nullptr && std::strlen(c) > 0);
    CHECK(std::strstr(c, "priv") == nullptr);
    CHECK(std::strstr(c, "secret") == nullptr);
  }
}

// ── the reserved tier is reserved: no fact pattern produces it ──────────────
static void test_hardware_bound_never_classified() {
  for (int i = 0; i < 8; ++i) {
    Facts f = facts((i & 1) != 0, (i & 2) != 0, (i & 4) != 0);
    CHECK(key_at_rest::classify(f) != Tier::HardwareBound);
  }
}

// ── every one of the 8 fact combinations has a coherent decision ────────────
//
// allow_persist and allow_load always agree; a refusal always carries a
// reason; a warning is only ever issued for an allowed plaintext key.
static void test_decision_table_is_coherent() {
  for (int i = 0; i < 8; ++i) {
    Facts f = facts((i & 1) != 0, (i & 2) != 0, (i & 4) != 0);
    Decision d = key_at_rest::decide(f);
    CHECK(d.allow_persist == d.allow_load);
    if (!d.allow_persist) CHECK(d.reason != nullptr);
    if (d.warn_once) {
      CHECK(d.allow_persist);
      CHECK(!f.flash_encryption);
      CHECK(d.reason != nullptr);
    }
    if (f.flash_encryption) CHECK(d.allow_persist && !d.warn_once);
  }
}

int main() {
  test_default_fe_off_allows_and_warns();
  test_default_fe_on_allows_no_warn();
  test_fe_plus_secure_boot_label();
  test_require_fe_off_refuses_symmetrically();
  test_require_fe_on_allows();
  test_secure_boot_alone_is_still_plaintext();
  test_wire_labels_are_wire_safe();
  test_hardware_bound_never_classified();
  test_decision_table_is_coherent();

  if (g_failures == 0) { std::printf("ALL key-at-rest tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
