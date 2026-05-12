// Host-side test for Beacon v0.4 solo origination invariants
// (spec/beacon_channel_v0.md §6.2 + AGENTS.md invariant #2 solo exception).
//
// Same approach as test_beacon_origination.cpp: we don't link
// beacon_channel.cpp (it needs ESP32 deps). We test the *protocol-level*
// validation pipeline that handle_alert_frame implements.
//
// The solo-path invariants under test:
//   - `BCN_FLAG_SOLO_ORIGIN` flag must be set on the wire
//   - `certainty == Observed`
//   - `originator_fp == cosigner_fp` (the cosigner is the BOOT button on
//     the same device)
//   - Single Ed25519 signature copied into both slots (we abstract
//     "signature valid" as a boolean since we don't link Ed25519 here)
//   - Originator is in the local beacon set and not revoked
//
// Solo frames that violate ANY of these are rejected. Dual-pubkey frames
// that violate the inverse (originator==cosigner) are still rejected (no
// regression).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t DEVICE_FP_SIZE = 16;
constexpr uint8_t BCN_MAGIC = 0xB1;
constexpr uint8_t BCN_SCOPE_PRIVATE = 2;
constexpr uint8_t BCN_TRUST_REVOKED = 2;
constexpr uint8_t BCN_FLAG_SOLO_ORIGIN = 0x04;
constexpr uint8_t BCN_CERT_OBSERVED = 0;
constexpr uint8_t BCN_CERT_LIKELY   = 1;

struct SetEntry {
  uint8_t fp[DEVICE_FP_SIZE];
  uint8_t trust;
  bool valid;
};

struct Frame {
  uint8_t  magic;
  uint8_t  flags;
  uint8_t  scope;
  uint8_t  certainty;
  uint8_t  originator_fp[DEVICE_FP_SIZE];
  uint8_t  cosigner_fp[DEVICE_FP_SIZE];
  bool     sig_a_valid;
  bool     sig_b_valid;
};

const SetEntry* find_in_set(const std::vector<SetEntry>& s, const uint8_t* fp) {
  for (const auto& e : s) {
    if (!e.valid) continue;
    if (std::memcmp(e.fp, fp, DEVICE_FP_SIZE) == 0) return &e;
  }
  return nullptr;
}

// Reimplements handle_alert_frame's gating logic for both the dual-pubkey
// and solo paths. Single source of truth for "what would the firmware do?"
bool would_accept(const std::vector<SetEntry>& set, const Frame& f) {
  if (f.magic != BCN_MAGIC) return false;
  if (f.scope != BCN_SCOPE_PRIVATE) return false;

  const bool is_solo = (f.flags & BCN_FLAG_SOLO_ORIGIN) != 0;
  if (is_solo) {
    if (f.certainty != BCN_CERT_OBSERVED) return false;
    if (std::memcmp(f.originator_fp, f.cosigner_fp, DEVICE_FP_SIZE) != 0) return false;
  }

  const SetEntry* a = find_in_set(set, f.originator_fp);
  const SetEntry* b = is_solo ? a : find_in_set(set, f.cosigner_fp);
  if (!a || !b) return false;
  if (a->trust == BCN_TRUST_REVOKED) return false;
  if (b->trust == BCN_TRUST_REVOKED) return false;

  // Dual-pubkey path: originator and cosigner must differ.
  if (!is_solo &&
      std::memcmp(f.originator_fp, f.cosigner_fp, DEVICE_FP_SIZE) == 0) {
    return false;
  }

  if (!f.sig_a_valid || !f.sig_b_valid) return false;
  return true;
}

int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while(0)

SetEntry mk(uint8_t prefix, uint8_t trust = 0) {
  SetEntry e{};
  e.valid = true;
  e.trust = trust;
  std::memset(e.fp, prefix, DEVICE_FP_SIZE);
  return e;
}

Frame mk_solo(uint8_t prefix, uint8_t certainty = BCN_CERT_OBSERVED,
              bool sa = true, bool sb = true) {
  Frame f{};
  f.magic = BCN_MAGIC;
  f.scope = BCN_SCOPE_PRIVATE;
  f.flags = BCN_FLAG_SOLO_ORIGIN;
  f.certainty = certainty;
  std::memset(f.originator_fp, prefix, DEVICE_FP_SIZE);
  std::memset(f.cosigner_fp,   prefix, DEVICE_FP_SIZE);
  f.sig_a_valid = sa;
  f.sig_b_valid = sb;
  return f;
}

// ───────────────────────────────────────────────────────────────────────────

void test_solo_happy_path() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_solo(0xAA);
  EXPECT(would_accept(set, f),
         "solo happy path: BOOT-button origination from a known pubkey accepted");
}

void test_solo_requires_observed_certainty() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_solo(0xAA, /*certainty=*/BCN_CERT_LIKELY);
  EXPECT(!would_accept(set, f),
         "solo certainty != Observed -> rejected (spec section 6.2 invariant)");
}

void test_solo_requires_origin_eq_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_solo(0xAA);
  // Tamper: change cosigner_fp to a different value.
  std::memset(f.cosigner_fp, 0xBB, DEVICE_FP_SIZE);
  EXPECT(!would_accept(set, f),
         "solo frame with originator != cosigner -> rejected (malformed)");
}

void test_solo_requires_originator_in_set() {
  std::vector<SetEntry> set = { mk(0xCC) };  // 0xAA not paired
  Frame f = mk_solo(0xAA);
  EXPECT(!would_accept(set, f),
         "solo from unpaired pubkey -> rejected (no implicit trust)");
}

void test_solo_revoked_originator() {
  std::vector<SetEntry> set = { mk(0xAA, BCN_TRUST_REVOKED) };
  Frame f = mk_solo(0xAA);
  EXPECT(!would_accept(set, f),
         "solo from revoked pubkey -> rejected");
}

void test_solo_requires_signatures_valid() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_solo(0xAA, BCN_CERT_OBSERVED, /*sa=*/true, /*sb=*/false);
  EXPECT(!would_accept(set, f),
         "solo with invalid sig_b -> rejected (uniform verify of both slots)");

  Frame f2 = mk_solo(0xAA, BCN_CERT_OBSERVED, /*sa=*/false, /*sb=*/true);
  EXPECT(!would_accept(set, f2),
         "solo with invalid sig_a -> rejected");
}

// No regression: a dual-pubkey frame with originator==cosigner but NO solo
// flag is still rejected (it's malformed under the dual-pubkey rule).
void test_dual_with_collapsed_signers_still_rejected() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f{};
  f.magic = BCN_MAGIC;
  f.scope = BCN_SCOPE_PRIVATE;
  f.flags = 0;  // NOT solo
  f.certainty = BCN_CERT_LIKELY;
  std::memset(f.originator_fp, 0xAA, DEVICE_FP_SIZE);
  std::memset(f.cosigner_fp,   0xAA, DEVICE_FP_SIZE);  // collapsed
  f.sig_a_valid = true;
  f.sig_b_valid = true;
  EXPECT(!would_accept(set, f),
         "dual-pubkey frame with collapsed signers without solo flag -> rejected");
}

// Cross-check: an actual dual-pubkey frame still works (no regression).
void test_dual_still_works() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f{};
  f.magic = BCN_MAGIC;
  f.scope = BCN_SCOPE_PRIVATE;
  f.flags = 0;
  f.certainty = BCN_CERT_LIKELY;
  std::memset(f.originator_fp, 0xAA, DEVICE_FP_SIZE);
  std::memset(f.cosigner_fp,   0xBB, DEVICE_FP_SIZE);
  f.sig_a_valid = true;
  f.sig_b_valid = true;
  EXPECT(would_accept(set, f),
         "standard dual-pubkey path: two distinct pubkeys still accepted");
}

} // namespace

int main() {
  test_solo_happy_path();
  test_solo_requires_observed_certainty();
  test_solo_requires_origin_eq_cosigner();
  test_solo_requires_originator_in_set();
  test_solo_revoked_originator();
  test_solo_requires_signatures_valid();
  test_dual_with_collapsed_signers_still_rejected();
  test_dual_still_works();

  if (failures == 0) {
    std::printf("All beacon solo origination invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
