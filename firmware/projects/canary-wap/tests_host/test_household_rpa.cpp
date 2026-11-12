// Household device recognition resolves a Resolvable Private Address
// against the enrolled IRKs (household.cpp's resolve_rpa_detailed()), and
// the hash compare in it runs without an early exit.
//
// The REAL household.cpp, over the real nvs_store.h on the first-boot
// suite's fake NVS (stubs/first_boot; Arduino.h with a settable millis()
// and mbedtls/aes.h over OpenSSL's AES-128 from stubs/household). Its
// answers, which the wave 26 change kept:
//   - the Bluetooth Core Specification's sample for ah() (Vol 3, Part H,
//     Appendix D.7: IRK ec0234a3 57c8ad05 341010a6 0a397d9b, prand 708194,
//     hash 0dfbaa) resolves to the slot that holds the IRK;
//   - an RPA made from each of three enrolled IRKs (this suite's own ah(),
//     the spec's formula over OpenSSL) resolves to its slot, after a reboot
//     too (the IRKs reloaded from NVS);
//   - the same address one bit off in any of its 24 hash bits, or in a
//     prand bit, matches no slot (this suite's ah() says none of the three
//     IRKs gives that hash): no match is counted and no slot's last-seen
//     stamp moves;
//   - an address without the RPA type bits is not resolved (no AES runs),
//     and a slot whose AES refuses is skipped, not matched.
// And a source pin: resolve_rpa_detailed() compares the computed hash with
// ct_compare::equal() over the whole 3-byte hash (the staged copy of
// firmware/common/core/ct_compare.h, which firmware/tests_host's
// test_ct_compare holds to reading every byte), after ah() ran, and not with
// the `computed_hash[0] == expected_hash[0] && ...` chain it replaced, which
// branched on each byte; the two lengths are asserted equal; household.cpp
// includes ct_compare.h once. The chain gave the same answers, so only the
// pin fails on it. No timing is measured here or anywhere: constant time is
// not measurable meaningfully on a host.
//
// Host-tested; not run on a device. The ESP32 builds of household.cpp are
// firmware.yml's.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-household-rpa

#include <openssl/evp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Preferences.h"     // stubs/first_boot: host_nvs()
#include "mbedtls/aes.h"     // stubs/household: the fault switches
#include "beacon_source_scan.h"
#include "dp.h"
#include "health_log.h"
#include "household.h"

#ifndef HOUSEHOLD_CPP
#error "the Makefile passes HOUSEHOLD_CPP, household.cpp's absolute path"
#endif

static int g_checks = 0;
static int g_failed = 0;

#define CHECK_MSG(cond, msg)                                                   \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_failed++;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, std::string(msg).c_str()); \
    }                                                                          \
  } while (0)

// ── What household.cpp links against ────────────────────────────────────────

static std::vector<std::string> g_log;
void health_log(LogLevel, LogCategory, const char* message) { g_log.push_back(message ? message : ""); }
void log_health(LogLevel, LogCategory, const char* message, const char*) {
  g_log.push_back(message ? message : "");
}

// get_stats_for_export() draws through dp::Release; the DP budget is
// test_dp_budget's (the real dp.cpp), not this suite's. This stand-in
// refuses every release, so nothing here exports.
namespace dp {
Release::Release(uint16_t draws, uint16_t epsilon_x1000, uint32_t delta_inv)
    : draws_left_(draws), epsilon_x1000_(epsilon_x1000), delta_inv_(delta_inv), gen_(0),
      ok_(false), short_(true) {}
bool Release::complete() const { return false; }
uint32_t Release::u32(uint32_t, uint32_t) { return 0; }
}  // namespace dp

// ── This suite's ah(), from the specification ───────────────────────────────
// hash = e(IRK, 0^104 || prand) mod 2^24, the IRK and the block big-endian;
// the address LSB-first, as NimBLE hands it (mac[0..2] hash, mac[3..5] prand,
// the top two bits of mac[5] the RPA type 0b01).

static uint32_t ref_ah(const uint8_t irk[16], uint32_t prand24) {
  uint8_t block[16] = {0};
  block[13] = (uint8_t)(prand24 >> 16);
  block[14] = (uint8_t)(prand24 >> 8);
  block[15] = (uint8_t)prand24;
  uint8_t out[32];
  int len = 0, fin = 0;
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), nullptr, irk, nullptr);
  EVP_CIPHER_CTX_set_padding(c, 0);
  EVP_EncryptUpdate(c, out, &len, block, 16);
  EVP_EncryptFinal_ex(c, out + len, &fin);
  EVP_CIPHER_CTX_free(c);
  return ((uint32_t)out[13] << 16) | ((uint32_t)out[14] << 8) | out[15];
}

static void make_rpa(uint32_t hash24, uint32_t prand24, uint8_t mac[6]) {
  mac[0] = (uint8_t)hash24;
  mac[1] = (uint8_t)(hash24 >> 8);
  mac[2] = (uint8_t)(hash24 >> 16);
  mac[3] = (uint8_t)prand24;
  mac[4] = (uint8_t)(prand24 >> 8);
  mac[5] = (uint8_t)(prand24 >> 16);
}

static const uint8_t kSpecIrk[16] = {0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05,
                                     0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b};

static const uint8_t kIrks[3][16] = {
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01},
    {0xa5, 0x5a, 0x0f, 0xf0, 0x3c, 0xc3, 0x96, 0x69, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0},
    {0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c, 0x2b},
};
// prand values with the RPA type bits (0b01 in the top two of 24).
static const uint32_t kPrands[] = {0x708194, 0x400000, 0x7fffff, 0x5a5a5a, 0x412345};

static void fresh() {
  household::deinit();
  host_nvs().clear();
  g_mbedtls_aes_fail_setkey = false;
  g_mbedtls_aes_fail_crypt = false;
  g_host_millis = 1000;
  household::init();
  g_log.clear();
}

static int enroll(const uint8_t irk[16], const char* label) {
  household::begin_enrollment();
  const int slot = household::add_irk(irk, label);
  household::end_enrollment();
  return slot;
}

// Which of the three enrolled IRKs, by this suite's ah(), gives `hash` for
// `prand`: its index, or -1.
static int ref_slot(uint32_t hash24, uint32_t prand24) {
  for (int i = 0; i < 3; i++) {
    if (ref_ah(kIrks[i], prand24) == hash24) return i;
  }
  return -1;
}

// ── The answers ─────────────────────────────────────────────────────────────

static void the_spec_sample_resolves() {
  CHECK_MSG(ref_ah(kSpecIrk, 0x708194) == 0x0dfbaa, "this suite's ah() does not give the spec's 0dfbaa");
  fresh();
  CHECK_MSG(enroll(kSpecIrk, "spec") == 0, "the spec IRK was not enrolled in slot 0");
  const uint8_t mac[6] = {0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70};
  const household::ResolveResult r = household::resolve_rpa_detailed(mac);
  CHECK_MSG(r.looked_like_rpa && r.matched && r.slot == 0, "the spec's RPA did not resolve to slot 0");
  CHECK_MSG(household::resolve_rpa(mac), "resolve_rpa() of the spec's RPA is false");
}

static void enrolled_rpas_resolve_to_their_slot() {
  fresh();
  for (int i = 0; i < 3; i++) {
    CHECK_MSG(enroll(kIrks[i], "dev") == i, "IRK " + std::to_string(i) + " not enrolled in its slot");
  }
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {  // a reboot: the IRKs come back from NVS
      household::deinit();
      household::init();
      CHECK_MSG(household::count() == 3, "after a reboot, " + std::to_string(household::count()) + " enrolled");
    }
    for (int i = 0; i < 3; i++) {
      for (uint32_t p : kPrands) {
        uint8_t mac[6];
        make_rpa(ref_ah(kIrks[i], p), p, mac);
        g_host_millis = 5000u + (uint32_t)i * 10u;
        const household::ResolveResult r = household::resolve_rpa_detailed(mac);
        CHECK_MSG(r.looked_like_rpa && r.matched && r.slot == i,
                  "pass " + std::to_string(pass) + ": IRK " + std::to_string(i) + "'s RPA resolved to slot " +
                      std::to_string(r.slot));
        CHECK_MSG(household::last_seen_ms((uint8_t)i) == g_host_millis,
                  "a match did not stamp its slot's last-seen time");
      }
    }
  }
}

static void one_bit_off_matches_no_slot() {
  fresh();
  for (int i = 0; i < 3; i++) enroll(kIrks[i], "dev");
  household::Stats before{};
  household::get_stats(&before);
  uint32_t seen[3];
  for (int i = 0; i < 3; i++) seen[i] = household::last_seen_ms((uint8_t)i);

  int tried = 0, refused = 0;
  g_host_millis = 90000;
  for (int i = 0; i < 3; i++) {
    for (uint32_t p : kPrands) {
      const uint32_t hash = ref_ah(kIrks[i], p);
      // Every hash bit, then every prand bit below the two type bits.
      for (int bit = 0; bit < 24 + 22; bit++) {
        const uint32_t h = bit < 24 ? hash ^ (1u << bit) : hash;
        const uint32_t q = bit < 24 ? p : p ^ (1u << (bit - 24));
        if (ref_slot(h, q) != -1) continue;  // another IRK gives this hash: not a refusal case
        uint8_t mac[6];
        make_rpa(h, q, mac);
        tried++;
        const household::ResolveResult r = household::resolve_rpa_detailed(mac);
        CHECK_MSG(r.looked_like_rpa, "a one-bit-off RPA was not taken as an RPA");
        if (!r.matched && r.slot == -1) {
          refused++;
        } else {
          CHECK_MSG(false, "IRK " + std::to_string(i) + " prand " + std::to_string(p) + " bit " +
                               std::to_string(bit) + " flipped: matched slot " + std::to_string(r.slot));
        }
      }
    }
  }
  CHECK_MSG(tried >= 3 * 5 * 46 - 2, "only " + std::to_string(tried) + " one-bit-off addresses tried");
  CHECK_MSG(refused == tried, std::to_string(tried - refused) + " of " + std::to_string(tried) + " matched");
  household::Stats after{};
  household::get_stats(&after);
  CHECK_MSG(after.total_resolves_matched == before.total_resolves_matched,
            "a one-bit-off RPA counted as a match");
  CHECK_MSG(after.total_resolves_attempted == before.total_resolves_attempted + (uint32_t)tried,
            "not every one-bit-off RPA counted as an attempt");
  bool stamps_kept = true;
  for (int i = 0; i < 3; i++) stamps_kept = stamps_kept && household::last_seen_ms((uint8_t)i) == seen[i];
  CHECK_MSG(stamps_kept, "a one-bit-off RPA moved a slot's last-seen stamp");
}

static void non_rpa_and_refused_aes() {
  fresh();
  enroll(kIrks[0], "dev");
  const uint32_t p = 0x708194;
  uint8_t mac[6];
  make_rpa(ref_ah(kIrks[0], p), p, mac);
  for (uint8_t type : {0x00, 0x80, 0xC0}) {  // public/static-random kinds, not 0b01
    uint8_t m[6];
    std::memcpy(m, mac, 6);
    m[5] = (uint8_t)((m[5] & 0x3F) | type);
    const int aes = g_mbedtls_aes_encryptions;
    const household::ResolveResult r = household::resolve_rpa_detailed(m);
    CHECK_MSG(!r.looked_like_rpa && !r.matched && r.slot == -1, "a non-RPA address was resolved");
    CHECK_MSG(g_mbedtls_aes_encryptions == aes, "a non-RPA address ran AES");
  }
  g_mbedtls_aes_fail_setkey = true;
  CHECK_MSG(!household::resolve_rpa_detailed(mac).matched, "a slot whose AES key refused matched");
  g_mbedtls_aes_fail_setkey = false;
  g_mbedtls_aes_fail_crypt = true;
  CHECK_MSG(!household::resolve_rpa_detailed(mac).matched, "a slot whose AES refused matched");
  g_mbedtls_aes_fail_crypt = false;
  CHECK_MSG(household::resolve_rpa_detailed(mac).matched, "the RPA does not resolve once AES answers");
}

// ── The source pin ──────────────────────────────────────────────────────────

static void the_compare_is_constant_time() {
  namespace bss = beacon_source_scan;
  bool ok = false;
  const std::string src = bss::read_source(HOUSEHOLD_CPP, &ok);
  CHECK_MSG(ok, "cannot read household.cpp");
  const std::string code = bss::strip_comments(src);
  const std::string body = bss::squeeze(bss::function_body(code, "resolve_rpa_detailed"));
  CHECK_MSG(!body.empty(), "household.cpp: no resolve_rpa_detailed()");
  const std::string compared =
      bss::squeeze("if (ct_compare::equal(computed_hash, expected_hash, sizeof(expected_hash))) {");
  const std::string ah_ran = bss::squeeze("if (!ah(s_slots[i].irk, prand, computed_hash)) continue;");
  CHECK_MSG(bss::count(body, compared) == 1,
            "resolve_rpa_detailed() does not compare the hash with ct_compare::equal() over the whole hash");
  CHECK_MSG(bss::before(body, ah_ran, compared), "resolve_rpa_detailed(): no ct_compare::equal() of the hash after ah() ran");
  CHECK_MSG(body.find("==expected_hash[") == std::string::npos && body.find("expected_hash[0]==") == std::string::npos,
            "resolve_rpa_detailed() compares hash bytes one by one");
  CHECK_MSG(body.find("memcmp(") == std::string::npos, "resolve_rpa_detailed() calls memcmp()");
  CHECK_MSG(bss::count(body, bss::squeeze("static_assert(sizeof(computed_hash) == sizeof(expected_hash),")) == 1,
            "resolve_rpa_detailed(): the two hash lengths are not asserted equal");
  CHECK_MSG(bss::count(bss::squeeze(code), "#include\"ct_compare.h\"") == 1,
            "household.cpp does not include ct_compare.h once");
}

int main() {
  the_spec_sample_resolves();
  enrolled_rpas_resolve_to_their_slot();
  one_bit_off_matches_no_slot();
  non_rpa_and_refused_aes();
  the_compare_is_constant_time();
  household::deinit();
  if (g_failed != 0) {
    std::printf("test_household_rpa: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("test_household_rpa: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
