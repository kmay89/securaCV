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
//
// The solo path is not a relaxed path: every receive-path check the
// dual-pubkey path answers to applies here too. The header msg_type must
// agree with the signed canonical, EXERCISE and BCN_FLAG_IS_EXERCISE imply
// each other, and the template must be in the life-safety set — so a
// captured solo drill cannot be rebroadcast as a solo alert.

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
constexpr uint8_t BCN_FLAG_IS_EXERCISE = 0x01;
constexpr uint8_t BCN_CERT_OBSERVED = 0;
constexpr uint8_t BCN_CERT_LIKELY   = 1;

constexpr uint8_t MSG_ALERT    = 0;
constexpr uint8_t MSG_CANCEL   = 2;
constexpr uint8_t MSG_EXERCISE = 3;

constexpr uint8_t TPL_FIRE_VISIBLE = 0x20;
// 0x00 is CHIRP_TPL_AUTH_POLICE_ACTIVITY in Chirp's numbering — a category
// Beacon excludes by design (spec §4).
constexpr uint8_t TPL_NOT_BEACON   = 0x00;

bool is_valid_beacon_template(uint8_t id) {
  switch (id) {
    case 0x10: case 0x12:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
    case 0x30: case 0x31: case 0x32:
    case 0x80: case 0x81: case 0x82:
      return true;
    default:
      return false;
  }
}

struct SetEntry {
  uint8_t fp[DEVICE_FP_SIZE];
  uint8_t trust;
  bool valid;
};

struct Frame {
  uint8_t  magic;
  uint8_t  hdr_msg_type;
  uint8_t  flags;
  uint8_t  scope;
  uint8_t  canon_msg_type;
  uint8_t  template_id;
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

  // Only the canonical is signed, so the header msg_type must agree with it.
  if (f.canon_msg_type != f.hdr_msg_type) return false;
  // spec §5.4: EXERCISE <=> BCN_FLAG_IS_EXERCISE, in both directions.
  const bool exercise_flag = (f.flags & BCN_FLAG_IS_EXERCISE) != 0;
  if ((f.canon_msg_type == MSG_EXERCISE) != exercise_flag) return false;
  if (!is_valid_beacon_template(f.template_id)) return false;

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
  f.hdr_msg_type = MSG_ALERT;
  f.canon_msg_type = MSG_ALERT;
  f.template_id = TPL_FIRE_VISIBLE;
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
  f.hdr_msg_type = MSG_ALERT;
  f.canon_msg_type = MSG_ALERT;
  f.template_id = TPL_FIRE_VISIBLE;
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
  f.hdr_msg_type = MSG_ALERT;
  f.canon_msg_type = MSG_ALERT;
  f.template_id = TPL_FIRE_VISIBLE;
  f.certainty = BCN_CERT_LIKELY;
  std::memset(f.originator_fp, 0xAA, DEVICE_FP_SIZE);
  std::memset(f.cosigner_fp,   0xBB, DEVICE_FP_SIZE);
  f.sig_a_valid = true;
  f.sig_b_valid = true;
  EXPECT(would_accept(set, f),
         "standard dual-pubkey path: two distinct pubkeys still accepted");
}

// ───────────────────────────────────────────────────────────────────────────
// The solo path answers to the same receive-path checks as the dual path.
// ───────────────────────────────────────────────────────────────────────────

void test_solo_header_msg_type_must_match_canonical() {
  std::vector<SetEntry> set = { mk(0xAA) };
  // A captured solo drill, rebroadcast with the unsigned header byte
  // rewritten to ALERT. The single signature still verifies in both slots.
  Frame f = mk_solo(0xAA);
  f.canon_msg_type = MSG_EXERCISE;
  f.flags = BCN_FLAG_SOLO_ORIGIN | BCN_FLAG_IS_EXERCISE;
  f.hdr_msg_type = MSG_ALERT;
  EXPECT(!would_accept(set, f),
         "solo drill promoted to ALERT via the unsigned header byte -> rejected");

  Frame g = mk_solo(0xAA);
  g.canon_msg_type = MSG_CANCEL;
  g.hdr_msg_type = MSG_ALERT;
  EXPECT(!would_accept(set, g),
         "solo CANCEL promoted to ALERT via the unsigned header byte -> rejected");
}

void test_solo_exercise_flag_biconditional() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_solo(0xAA);
  f.hdr_msg_type = MSG_EXERCISE;
  f.canon_msg_type = MSG_EXERCISE;
  EXPECT(!would_accept(set, f),
         "solo EXERCISE without BCN_FLAG_IS_EXERCISE -> rejected (spec 5.4)");

  f.flags = BCN_FLAG_SOLO_ORIGIN | BCN_FLAG_IS_EXERCISE;
  EXPECT(would_accept(set, f), "solo EXERCISE carrying the flag -> accepted");

  Frame g = mk_solo(0xAA);  // real ALERT
  g.flags = BCN_FLAG_SOLO_ORIGIN | BCN_FLAG_IS_EXERCISE;
  EXPECT(!would_accept(set, g),
         "solo ALERT wearing the drill flag -> rejected (no demotion either)");
}

void test_solo_template_must_be_life_safety() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_solo(0xAA);
  f.template_id = TPL_NOT_BEACON;
  EXPECT(!would_accept(set, f),
         "solo frame carrying a non-Beacon template -> rejected (spec section 4)");
  f.template_id = 0x99;
  EXPECT(!would_accept(set, f), "solo frame with a junk template byte -> rejected");
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
  test_solo_header_msg_type_must_match_canonical();
  test_solo_exercise_flag_biconditional();
  test_solo_template_must_be_life_safety();

  if (failures == 0) {
    std::printf("All beacon solo origination invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
