/* Host tests for the key-at-rest policy (firmware/common/identity/key_at_rest.h).
 *
 * Every test here is about a way a device could misstate where its identity
 * key sleeps, or a way an image could act against its own promise. The module
 * has one job — turn {flash encryption, NVS encryption, secure boot,
 * require_fe} into a tier, a wire label, an allow/refuse decision and the one
 * boot line — and each rule exists because the alternative is either a
 * stranded default board or a Tier-3 image quietly using a plaintext key. So
 * the suite is organized by the failure prevented.
 *
 * The failure this suite was rewritten around: treating "flash encryption is
 * on" as "the key is encrypted at rest". Flash encryption does not cover NVS
 * (only app / otadata / nvs_keys partitions are encrypted); NVS is ciphertext
 * only under NVS encryption, which the Arduino-framework canary build does not
 * have. A fused board with plaintext NVS must therefore report plaintext, warn,
 * and make an opt-in image refuse.
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

static Facts facts(bool fe, bool nvs, bool sb, bool req) {
  Facts f;
  f.flash_encryption = fe;
  f.nvs_encryption = nvs;
  f.secure_boot = sb;
  f.require_fe = req;
  return f;
}

// All 16 fact combinations, bit 0 = FE, 1 = NVS encryption, 2 = SB, 3 = require.
static Facts facts_of(int i) {
  return facts((i & 1) != 0, (i & 2) != 0, (i & 4) != 0, (i & 8) != 0);
}

static bool starts_with(const char* s, const char* prefix) {
  return s && std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// ── the failure: a stranded default board ───────────────────────────────────
//
// Rule 1. The default Canary sits at Tier 0 by decision. On ordinary hardware
// it must store and load its key, and it must say so once — a warning, not a
// refusal. A refusal here would halt provisioning on every dev board.
static void test_default_fe_off_allows_and_warns() {
  const Facts f = facts(false, false, false, false);
  Decision d = key_at_rest::decide(f);
  CHECK(d.allow_persist);
  CHECK(d.allow_load);
  CHECK(d.warn_once);
  CHECK(d.reason != nullptr);
  CHECK(key_at_rest::classify(f) == Tier::PlaintextNvs);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::PlaintextNvs), "plaintext-nvs") == 0);
}

// ── the failure: calling a fused board's plaintext NVS "encrypted" ──────────
//
// The review finding this suite exists for. Flash encryption on, NVS
// encryption off — the ONLY state a fused board can be in under
// framework = arduino — leaves the key readable from a flash dump. It must
// classify as plaintext, warn (not INFO), and say why in words that are not
// the FE-off sentence, so the bench can tell the two apart.
static void test_fe_on_without_nvs_encryption_is_still_plaintext() {
  for (int sb = 0; sb < 2; ++sb) {
    const Facts f = facts(true, false, sb != 0, false);
    CHECK(key_at_rest::classify(f) == Tier::PlaintextNvs);
    CHECK(!key_at_rest::nvs_is_encrypted(f));
    Decision d = key_at_rest::decide(f);
    CHECK(d.allow_persist && d.allow_load);   // still the Tier-0 default: allowed
    CHECK(d.warn_once);                       // ...and never quiet about it
    CHECK(d.reason != nullptr);
    CHECK(std::strstr(d.reason, "does not cover NVS") != nullptr);
    Decision off = key_at_rest::decide(facts(false, false, sb != 0, false));
    CHECK(std::strcmp(d.reason, off.reason) != 0);
    CHECK(std::strcmp(key_at_rest::boot_level(f), "WARN") == 0);
  }
}

// ── NVS encryption (on top of flash encryption): allowed, nothing to warn ───
static void test_encrypted_nvs_allows_no_warn() {
  const Facts f = facts(true, true, false, false);
  Decision d = key_at_rest::decide(f);
  CHECK(d.allow_persist);
  CHECK(d.allow_load);
  CHECK(!d.warn_once);
  CHECK(d.reason == nullptr);
  CHECK(key_at_rest::classify(f) == Tier::EncryptedNvs);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::EncryptedNvs), "nvs-encrypted") == 0);
}

// ── Secure Boot on top of encrypted NVS says so ─────────────────────────────
static void test_encrypted_nvs_plus_secure_boot_label() {
  const Facts f = facts(true, true, true, false);
  CHECK(key_at_rest::classify(f) == Tier::EncryptedNvsSecureBoot);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::EncryptedNvsSecureBoot),
                    "nvs-encrypted+secure-boot") == 0);
  Decision d = key_at_rest::decide(f);
  CHECK(d.allow_persist && d.allow_load && !d.warn_once);
}

// ── the failure: an NVS-encryption fact standing in for flash encryption ────
//
// NVS encryption needs flash encryption (its keys live in an encrypted
// nvs_keys partition). A caller that ever reported the first without the
// second must not get an "encrypted" answer out of it.
static void test_nvs_flag_without_fe_is_plaintext() {
  for (int sb = 0; sb < 2; ++sb) {
    const Facts f = facts(false, true, sb != 0, false);
    CHECK(key_at_rest::classify(f) == Tier::PlaintextNvs);
    CHECK(key_at_rest::decide(f).warn_once);
  }
}

// ── the failure: a Tier-3 image using a plaintext key ───────────────────────
//
// Rule 2. An image that promised the key is protected at rest must refuse
// BOTH halves unless its NVS is actually encrypted: never write a key
// (persist) and never use one it finds (load). Refusing only one would let a
// key a default image wrote be used by the image that promised it never
// would be. The reason is non-null so the bench operator sees why
// provisioning halted, and the FE-off sentence is the one K1 greps.
static void test_require_fe_off_refuses_symmetrically() {
  Decision d = key_at_rest::decide(facts(false, false, false, true));
  CHECK(d.allow_persist == false);
  CHECK(d.allow_load == false);
  CHECK(d.allow_persist == d.allow_load);   // symmetry, pinned
  CHECK(d.reason != nullptr);
  CHECK(std::strcmp(d.reason, "flash encryption required by this build but not active") == 0);
  CHECK(!d.warn_once);                      // a refusal is not a warning
  // Secure Boot without flash encryption changes nothing here.
  Decision d2 = key_at_rest::decide(facts(false, false, true, true));
  CHECK(!d2.allow_persist && !d2.allow_load);
}

// ── the failure: the opt-in image accepting flash encryption alone ──────────
//
// The review's rule-2 hole: on a fused board with plaintext NVS the old policy
// ALLOWED the store. It must refuse, symmetrically, with Secure Boot or not,
// and name the NVS gap rather than claim flash encryption is missing.
static void test_require_fe_on_but_nvs_plaintext_refuses() {
  for (int sb = 0; sb < 2; ++sb) {
    const Facts f = facts(true, false, sb != 0, true);
    Decision d = key_at_rest::decide(f);
    CHECK(!d.allow_persist);
    CHECK(!d.allow_load);
    CHECK(!d.warn_once);
    CHECK(d.reason != nullptr);
    CHECK(starts_with(d.reason, "encrypted NVS required by this build"));
    CHECK(std::strstr(d.reason, "flash encryption alone does not cover NVS") != nullptr);
    CHECK(std::strcmp(key_at_rest::boot_level(f), "!!") == 0);
    CHECK(std::strcmp(key_at_rest::boot_text(f), d.reason) == 0);
  }
}

// ── the opt-in image on the hardware it was built for: allowed, quiet ───────
static void test_require_fe_with_encrypted_nvs_allows() {
  Decision d = key_at_rest::decide(facts(true, true, false, true));
  CHECK(d.allow_persist);
  CHECK(d.allow_load);
  CHECK(!d.warn_once);
  CHECK(d.reason == nullptr);
  Decision d2 = key_at_rest::decide(facts(true, true, true, true));
  CHECK(d2.allow_persist && d2.allow_load && !d2.warn_once);
}

// ── the failure: calling Secure Boot an at-rest protection ──────────────────
//
// Secure Boot stops unsigned firmware running; it encrypts nothing. A board
// with Secure Boot and no encrypted NVS still holds its key in plaintext.
static void test_secure_boot_alone_is_still_plaintext() {
  const Facts f = facts(false, false, true, false);
  CHECK(key_at_rest::classify(f) == Tier::PlaintextNvs);
  Decision d = key_at_rest::decide(f);
  CHECK(d.allow_persist && d.allow_load && d.warn_once);
}

// ── the labels are wire fields: plain ASCII, lower-case, no spaces, they
//    never name the thing they describe as secret material, and they name
//    where the bytes sit — never merely which eFuse is burned ───────────────
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
  // "flash-encrypted" read as "the key is encrypted" and was false (flash
  // encryption does not cover NVS). No label may be spelled from the eFuse.
  CHECK(std::strstr(s, "flash") == nullptr);
}

static void test_wire_labels_are_wire_safe() {
  const Tier tiers[] = { Tier::PlaintextNvs, Tier::EncryptedNvs,
                         Tier::EncryptedNvsSecureBoot, Tier::HardwareBound };
  for (Tier t : tiers) check_label_shape(key_at_rest::wire_label(t));
  // Distinct tiers, distinct labels.
  for (Tier a : tiers)
    for (Tier b : tiers)
      if (a != b) CHECK(std::strcmp(key_at_rest::wire_label(a), key_at_rest::wire_label(b)) != 0);
  CHECK(std::strcmp(key_at_rest::wire_label(Tier::HardwareBound), "hw-bound") == 0);
  // The console labels may have spaces, but must also never call the key secret
  // material (regression_check.sh greps serial lines for that), and never claim
  // a ladder tier above 0: Tier 3/4 also need things this module does not read
  // (release-mode flash encryption, JTAG off).
  for (Tier t : tiers) {
    const char* c = key_at_rest::console_label(t);
    CHECK(c != nullptr && std::strlen(c) > 0);
    CHECK(std::strstr(c, "priv") == nullptr);
    CHECK(std::strstr(c, "secret") == nullptr);
    CHECK(std::strstr(c, "Tier 3") == nullptr);
    CHECK(std::strstr(c, "Tier 4") == nullptr);
  }
}

// ── the reserved tier is reserved: no fact pattern produces it ──────────────
static void test_hardware_bound_never_classified() {
  for (int i = 0; i < 16; ++i) {
    CHECK(key_at_rest::classify(facts_of(i)) != Tier::HardwareBound);
  }
}

// ── the boot line is the decision, not a re-derivation of it ────────────────
//
// The device prints boot_level()/boot_text() verbatim. Pin that they are
// exactly decide() + console_label() for every combination, so a firmware that
// prints them cannot say INFO over a plaintext key or WARN over a refusal.
static void test_boot_line_follows_decision() {
  for (int i = 0; i < 16; ++i) {
    const Facts f = facts_of(i);
    const Decision d = key_at_rest::decide(f);
    const char* level = key_at_rest::boot_level(f);
    const char* text = key_at_rest::boot_text(f);
    CHECK(level != nullptr && text != nullptr);
    if (!d.allow_load) CHECK(std::strcmp(level, "!!") == 0);
    else if (d.warn_once) CHECK(std::strcmp(level, "WARN") == 0);
    else CHECK(std::strcmp(level, "INFO") == 0);
    if (d.reason) CHECK(std::strcmp(text, d.reason) == 0);
    else CHECK(std::strcmp(text, key_at_rest::console_label(key_at_rest::classify(f))) == 0);
    // INFO only ever over an encrypted key.
    if (std::strcmp(level, "INFO") == 0) CHECK(key_at_rest::nvs_is_encrypted(f));
  }
  // The K1 bench greps these exact prefixes on the default image.
  CHECK(std::strcmp(key_at_rest::boot_level(facts(false, false, false, false)), "WARN") == 0);
  CHECK(starts_with(key_at_rest::boot_text(facts(false, false, false, false)),
                    "identity key at rest in plaintext NVS"));
}

// ── every one of the 16 fact combinations has a coherent decision ───────────
//
// allow_persist and allow_load always agree; a refusal always carries a
// reason; a warning is only ever issued for an allowed plaintext key; an
// "encrypted" tier only ever with BOTH encryptions; the opt-in refuses exactly
// when NVS is not encrypted, and the default never refuses.
static void test_decision_table_is_coherent() {
  for (int i = 0; i < 16; ++i) {
    const Facts f = facts_of(i);
    const Decision d = key_at_rest::decide(f);
    const Tier t = key_at_rest::classify(f);
    const bool encrypted = f.flash_encryption && f.nvs_encryption;
    CHECK(key_at_rest::nvs_is_encrypted(f) == encrypted);
    CHECK(d.allow_persist == d.allow_load);
    if (!d.allow_persist) CHECK(d.reason != nullptr);
    if (d.warn_once) {
      CHECK(d.allow_persist);
      CHECK(t == Tier::PlaintextNvs);
      CHECK(d.reason != nullptr);
    }
    CHECK((t != Tier::PlaintextNvs) == encrypted);
    if (encrypted) CHECK(d.allow_persist && !d.warn_once && d.reason == nullptr);
    if (f.require_fe) CHECK(d.allow_persist == encrypted);
    else CHECK(d.allow_persist);
    if (!f.require_fe && !encrypted) CHECK(d.warn_once);
  }
}

int main() {
  test_default_fe_off_allows_and_warns();
  test_fe_on_without_nvs_encryption_is_still_plaintext();
  test_encrypted_nvs_allows_no_warn();
  test_encrypted_nvs_plus_secure_boot_label();
  test_nvs_flag_without_fe_is_plaintext();
  test_require_fe_off_refuses_symmetrically();
  test_require_fe_on_but_nvs_plaintext_refuses();
  test_require_fe_with_encrypted_nvs_allows();
  test_secure_boot_alone_is_still_plaintext();
  test_wire_labels_are_wire_safe();
  test_hardware_bound_never_classified();
  test_boot_line_follows_decision();
  test_decision_table_is_coherent();

  if (g_failures == 0) { std::printf("ALL key-at-rest tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
