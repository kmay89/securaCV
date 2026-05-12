// Host-side test for the Beacon channel's two-pubkey origination invariants.
//
// Mirrors the verification logic in beacon_channel.cpp::handle_alert_frame
// for the validation pipeline:
//   1. magic = 0xB1
//   2. scope = Private
//   3. both originator_fp and cosigner_fp present in local beacon set
//   4. neither revoked
//   5. originator_fp != cosigner_fp
//   6. both signatures verify (assumed; we don't link Ed25519 here)
//
// We test the *gating logic* without doing real Ed25519 — the protocol
// invariant being asserted is that any frame failing any of these checks is
// rejected at the earliest possible point.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t DEVICE_FP_SIZE = 16;
constexpr uint8_t BCN_MAGIC = 0xB1;
constexpr uint8_t BCN_SCOPE_PRIVATE = 2;
constexpr uint8_t BCN_TRUST_REVOKED = 2;

struct SetEntry {
  uint8_t fp[DEVICE_FP_SIZE];
  uint8_t trust;
  bool valid;
};

struct Frame {
  uint8_t magic;
  uint8_t scope;
  uint8_t originator_fp[DEVICE_FP_SIZE];
  uint8_t cosigner_fp[DEVICE_FP_SIZE];
  bool sig_a_valid;
  bool sig_b_valid;
};

const SetEntry* find_in_set(const std::vector<SetEntry>& s, const uint8_t* fp) {
  for (const auto& e : s) {
    if (!e.valid) continue;
    if (std::memcmp(e.fp, fp, DEVICE_FP_SIZE) == 0) return &e;
  }
  return nullptr;
}

// Returns true if the frame would be accepted by handle_alert_frame.
bool would_accept(const std::vector<SetEntry>& set, const Frame& f) {
  if (f.magic != BCN_MAGIC) return false;
  if (f.scope != BCN_SCOPE_PRIVATE) return false;
  if (std::memcmp(f.originator_fp, f.cosigner_fp, DEVICE_FP_SIZE) == 0) return false;
  const SetEntry* a = find_in_set(set, f.originator_fp);
  const SetEntry* b = find_in_set(set, f.cosigner_fp);
  if (!a || !b) return false;
  if (a->trust == BCN_TRUST_REVOKED) return false;
  if (b->trust == BCN_TRUST_REVOKED) return false;
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

Frame mk_frame(uint8_t orig_prefix, uint8_t cosign_prefix,
               bool sa = true, bool sb = true,
               uint8_t magic = BCN_MAGIC, uint8_t scope = BCN_SCOPE_PRIVATE) {
  Frame f{};
  f.magic = magic;
  f.scope = scope;
  std::memset(f.originator_fp, orig_prefix, DEVICE_FP_SIZE);
  std::memset(f.cosigner_fp,   cosign_prefix, DEVICE_FP_SIZE);
  f.sig_a_valid = sa;
  f.sig_b_valid = sb;
  return f;
}

void test_happy_path_two_signatures() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(would_accept(set, f), "happy path: two distinct paired pubkeys, both signatures verify");
}

void test_reject_single_signature() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB, /*sa=*/true, /*sb=*/false);
  EXPECT(!would_accept(set, f),
         "two-pubkey rule: missing cosigner signature must be rejected");
}

void test_reject_originator_equals_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_frame(0xAA, 0xAA);
  EXPECT(!would_accept(set, f),
         "originator and cosigner must be distinct pubkeys");
}

void test_reject_unpaired_originator() {
  std::vector<SetEntry> set = { mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f),
         "originator not in local beacon set → reject");
}

void test_reject_unpaired_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f),
         "cosigner not in local beacon set → reject");
}

void test_reject_revoked_originator() {
  std::vector<SetEntry> set = { mk(0xAA, BCN_TRUST_REVOKED), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f), "revoked originator → reject");
}

void test_reject_revoked_cosigner() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB, BCN_TRUST_REVOKED) };
  Frame f = mk_frame(0xAA, 0xBB);
  EXPECT(!would_accept(set, f), "revoked cosigner → reject");
}

void test_reject_wrong_magic() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  Frame f = mk_frame(0xAA, 0xBB, true, true, /*magic=*/0xC4);  // chirp magic
  EXPECT(!would_accept(set, f), "wrong magic byte → reject");
}

void test_reject_wrong_scope() {
  std::vector<SetEntry> set = { mk(0xAA), mk(0xBB) };
  // scope=0 ("Public" in CAP), prohibited for Beacon
  Frame f = mk_frame(0xAA, 0xBB, true, true, BCN_MAGIC, /*scope=*/0);
  EXPECT(!would_accept(set, f), "non-Private scope → reject (lint invariant)");
}

} // namespace

int main() {
  test_happy_path_two_signatures();
  test_reject_single_signature();
  test_reject_originator_equals_cosigner();
  test_reject_unpaired_originator();
  test_reject_unpaired_cosigner();
  test_reject_revoked_originator();
  test_reject_revoked_cosigner();
  test_reject_wrong_magic();
  test_reject_wrong_scope();

  if (failures == 0) {
    std::printf("All beacon origination invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
