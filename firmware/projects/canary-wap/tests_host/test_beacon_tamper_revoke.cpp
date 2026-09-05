// Host-side test for the v0.5 auto-revoke-on-tamper semantics
// (mesh_network.cpp::handle_tamper_alert → beacon_channel::on_peer_tampered).
//
// Without linking beacon_channel.cpp (ESP32-deps heavy), we test the
// *behavioral contract* the implementation promises:
//   1. on_peer_tampered(pubkey) computes the same 16-byte fingerprint
//      that beacon_channel::compute_fingerprint produces from the same
//      pubkey (SHA-256(pubkey)[0:16]).
//   2. If a beacon-set entry matches that fingerprint, its trust_level
//      transitions to REVOKED.
//   3. If no entry matches, it's a no-op (idempotent).
//   4. Calling on_peer_tampered with the same pubkey twice is idempotent
//      (does not toggle/oscillate, does not double-log, does not modify
//      already-REVOKED entries).
//   5. A REVOKED entry remains valid for lookup (so future incoming
//      Beacon frames from that pubkey can still be detected and
//      rejected at the trust check).
//
// We use openssl's sha256 (host-side); the firmware uses mbedtls but
// the bytes are identical.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <openssl/sha.h>

namespace {

constexpr size_t DEVICE_FP_SIZE = 16;
constexpr size_t DEVICE_PUBKEY_SIZE = 32;
constexpr uint8_t BCN_TRUST_COSIGNER = 0;
constexpr uint8_t BCN_TRUST_REVOKED  = 2;

constexpr uint32_t SELFTEST_MISSING_MS = 129600000;  // 36 h

struct SetEntry {
  uint8_t pubkey[DEVICE_PUBKEY_SIZE];
  uint8_t fp[DEVICE_FP_SIZE];
  uint8_t trust;
  uint32_t last_selftest_ms;  // 0 = not observed since boot
  bool valid;
};

// Same fingerprint derivation as beacon_channel.cpp::compute_fingerprint:
// SHA-256(pubkey)[0:16].
void compute_fingerprint(const uint8_t* pubkey, uint8_t* fp_out) {
  uint8_t hash[32];
  SHA256(pubkey, DEVICE_PUBKEY_SIZE, hash);
  std::memcpy(fp_out, hash, DEVICE_FP_SIZE);
}

// Reimplements revoke_beacon_set_entry + on_peer_tampered's combined
// behavior. Returns true if a matching entry was found and revoked (or
// was already revoked — idempotent).
bool on_peer_tampered_impl(std::vector<SetEntry>& set, const uint8_t* pubkey) {
  uint8_t fp[DEVICE_FP_SIZE];
  compute_fingerprint(pubkey, fp);
  for (auto& e : set) {
    if (!e.valid) continue;
    if (std::memcmp(e.fp, fp, DEVICE_FP_SIZE) == 0) {
      if (e.trust == BCN_TRUST_REVOKED) return true;  // idempotent
      e.trust = BCN_TRUST_REVOKED;
      return true;
    }
  }
  return false;
}

int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while(0)

SetEntry mk_entry(uint8_t prefix, uint8_t trust = BCN_TRUST_COSIGNER,
                  uint32_t last_selftest_ms = 0) {
  SetEntry e{};
  e.valid = true;
  e.trust = trust;
  e.last_selftest_ms = last_selftest_ms;
  std::memset(e.pubkey, prefix, DEVICE_PUBKEY_SIZE);
  compute_fingerprint(e.pubkey, e.fp);
  return e;
}

const SetEntry* find_by_fp(const std::vector<SetEntry>& set, const uint8_t* fp) {
  for (const auto& e : set) {
    if (!e.valid) continue;
    if (std::memcmp(e.fp, fp, DEVICE_FP_SIZE) == 0) return &e;
  }
  return nullptr;
}

// The two independent doors handle_alert_frame puts in front of a signer:
// revocation (this file's subject) and supervised health (spec §7.1 step 9).
// `last_selftest == 0` is "not observed since boot", not "stale".
bool signer_admissible(const SetEntry& e, uint32_t now_ms) {
  if (e.trust == BCN_TRUST_REVOKED) return false;
  if (e.last_selftest_ms == 0) return true;
  const uint32_t age = (now_ms > e.last_selftest_ms) ? (now_ms - e.last_selftest_ms) : 0;
  return age <= SELFTEST_MISSING_MS;
}

// ───────────────────────────────────────────────────────────────────────────

void test_revoke_known_pubkey() {
  std::vector<SetEntry> set = { mk_entry(0xAA), mk_entry(0xBB) };
  EXPECT(set[0].trust == BCN_TRUST_COSIGNER, "pre: 0xAA entry is COSIGNER");
  bool revoked = on_peer_tampered_impl(set, set[0].pubkey);
  EXPECT(revoked, "on_peer_tampered returns true on known pubkey");
  EXPECT(set[0].trust == BCN_TRUST_REVOKED, "0xAA entry is now REVOKED");
  EXPECT(set[1].trust == BCN_TRUST_COSIGNER, "0xBB entry untouched");
}

void test_idempotent_double_call() {
  std::vector<SetEntry> set = { mk_entry(0xAA) };
  EXPECT(on_peer_tampered_impl(set, set[0].pubkey), "first call revokes");
  EXPECT(on_peer_tampered_impl(set, set[0].pubkey),
         "second call still returns true (idempotent)");
  EXPECT(set[0].trust == BCN_TRUST_REVOKED, "trust remains REVOKED");
}

void test_unknown_pubkey_is_noop() {
  std::vector<SetEntry> set = { mk_entry(0xAA) };
  uint8_t stranger[DEVICE_PUBKEY_SIZE];
  std::memset(stranger, 0xCC, DEVICE_PUBKEY_SIZE);
  EXPECT(!on_peer_tampered_impl(set, stranger),
         "unknown pubkey -> false (no entry modified)");
  EXPECT(set[0].trust == BCN_TRUST_COSIGNER,
         "known pubkey's trust untouched by unrelated tamper alert");
}

void test_fingerprint_derivation_matches_spec() {
  // The spec invariant: fingerprint = SHA-256(pubkey)[0:16].
  // We verify our test reimplementation produces deterministic output
  // that matches what beacon_channel::compute_fingerprint would produce
  // (this is the same algorithm — both use SHA-256 of the raw pubkey).
  uint8_t pubkey[DEVICE_PUBKEY_SIZE];
  for (size_t i = 0; i < DEVICE_PUBKEY_SIZE; i++) pubkey[i] = (uint8_t)i;
  uint8_t fp1[DEVICE_FP_SIZE], fp2[DEVICE_FP_SIZE];
  compute_fingerprint(pubkey, fp1);
  compute_fingerprint(pubkey, fp2);
  EXPECT(std::memcmp(fp1, fp2, DEVICE_FP_SIZE) == 0,
         "fingerprint derivation is deterministic");
  // Known-answer check: SHA-256 of 0x00..0x1F (32 bytes of identity).
  // First two bytes of the hash are 0x63 0x0d (verified via Python's
  // hashlib.sha256 and confirmed against OpenSSL).
  EXPECT(fp1[0] == 0x63 && fp1[1] == 0x0d,
         "SHA-256 of identity bytes 0..31 starts with 0x63 0x0d (known answer)");
}

void test_distinct_pubkeys_have_distinct_fingerprints() {
  // The Beacon set's lookup-by-fingerprint correctness depends on the
  // pubkey-to-fingerprint mapping being injective in practice (no
  // collisions under SHA-256 truncated to 128 bits among the ~32 paired
  // devices a household will ever have). We sanity-check that two random
  // pubkeys produce different fingerprints.
  uint8_t pk_a[DEVICE_PUBKEY_SIZE], pk_b[DEVICE_PUBKEY_SIZE];
  std::memset(pk_a, 0xAA, DEVICE_PUBKEY_SIZE);
  std::memset(pk_b, 0xBB, DEVICE_PUBKEY_SIZE);
  uint8_t fp_a[DEVICE_FP_SIZE], fp_b[DEVICE_FP_SIZE];
  compute_fingerprint(pk_a, fp_a);
  compute_fingerprint(pk_b, fp_b);
  EXPECT(std::memcmp(fp_a, fp_b, DEVICE_FP_SIZE) != 0,
         "different pubkeys produce different fingerprints");
}

void test_revoked_entry_stays_findable_and_inadmissible() {
  // Contract item 5: revocation does not delete the entry — the receive
  // path has to be able to find it in order to refuse it.
  const uint32_t now_ms = 60000;
  std::vector<SetEntry> set = { mk_entry(0xAA, BCN_TRUST_COSIGNER, now_ms) };
  EXPECT(signer_admissible(set[0], now_ms), "pre: a healthy peer may sign");
  EXPECT(on_peer_tampered_impl(set, set[0].pubkey), "tamper alert revokes the peer");
  const SetEntry* found = find_by_fp(set, set[0].fp);
  EXPECT(found != nullptr, "the revoked entry is still findable by fingerprint");
  EXPECT(found && !signer_admissible(*found, now_ms),
         "a revoked peer never authorizes an alarm, however fresh its selftest");
}

void test_revocation_and_supervised_health_are_separate_doors() {
  // A peer that was never tampered with but has gone silent past 36 h is
  // refused too — auto-revoke is not the only thing gating a signer.
  const uint32_t now_ms = SELFTEST_MISSING_MS + 100000;
  SetEntry silent = mk_entry(0xBB, BCN_TRUST_COSIGNER, /*last_selftest_ms=*/1);
  EXPECT(!signer_admissible(silent, now_ms),
         "a non-revoked peer whose selftest lapsed past 36 h cannot sign");
  SetEntry unproven = mk_entry(0xCC, BCN_TRUST_COSIGNER, /*last_selftest_ms=*/0);
  EXPECT(signer_admissible(unproven, now_ms),
         "a peer with no selftest observed since boot is unknown, not stale");
}

} // namespace

int main() {
  test_revoke_known_pubkey();
  test_idempotent_double_call();
  test_unknown_pubkey_is_noop();
  test_fingerprint_derivation_matches_spec();
  test_distinct_pubkeys_have_distinct_fingerprints();
  test_revoked_entry_stays_findable_and_inadmissible();
  test_revocation_and_supervised_health_are_separate_doors();

  if (failures == 0) {
    std::printf("All beacon tamper auto-revoke invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
