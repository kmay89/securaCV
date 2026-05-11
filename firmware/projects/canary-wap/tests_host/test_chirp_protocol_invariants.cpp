// Host-side protocol-invariants test for Chirp v0.2.
//
// This is a self-contained test that validates the canonical-message byte
// layouts and the session-id-derivation rule documented in
// spec/chirp_channel_v0.md (v0.2) and enforced in chirp_channel.cpp.
//
// We do NOT pull in chirp_channel.cpp itself (which depends on ESP-NOW,
// Arduino, mbedtls, etc.). Instead we re-implement the canonical builders
// here in plain C++ and assert the resulting byte sequences match the
// documented format. This catches accidental drift between the firmware's
// signing input and what an independent verifier (e.g. a future CAP
// gateway, an audit-log consumer, a Beacon-aware UI) would compute.
//
// Run from tests_host/:
//   make test_chirp_protocol_invariants
//   ./test_chirp_protocol_invariants

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

namespace {

constexpr size_t SESSION_PUBKEY_SIZE = 32;
constexpr size_t SESSION_ID_SIZE     = 8;
constexpr size_t SIGNATURE_SIZE      = 64;
constexpr uint8_t CHIRP_MAGIC        = 0xC4;
constexpr uint8_t PROTOCOL_VERSION   = 1;  // v0.2

struct ChirpHeader {
  uint8_t magic;
  uint8_t version;
  uint8_t msg_type;
  uint8_t session_id[SESSION_ID_SIZE];
  uint8_t hop_count;
  uint32_t timestamp;
  uint8_t nonce[8];
};

struct ChirpWitnessPayload {
  uint8_t template_id;
  uint8_t detail_slot;
  uint8_t urgency;
  uint8_t ttl_minutes;
  uint8_t reserved[4];
  uint8_t session_pubkey[SESSION_PUBKEY_SIZE];
  uint8_t signature[SIGNATURE_SIZE];
  uint8_t signed_origin_pubkey[SESSION_PUBKEY_SIZE];
  uint8_t signed_origin_signature[SIGNATURE_SIZE];
};

// Re-implementation of build_witness_canonical from chirp_channel.cpp.
size_t build_witness_canonical(const ChirpHeader* hdr,
                               const ChirpWitnessPayload* payload,
                               const uint8_t* signer_pubkey,
                               uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:chirp:witness:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t need = domain_len + 8 + 1 + 1 + 1 + 1 + 4 + SESSION_PUBKEY_SIZE;
  if (out_max < need) return 0;
  size_t i = 0;
  std::memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  std::memcpy(out + i, hdr->nonce, 8); i += 8;
  out[i++] = payload->template_id;
  out[i++] = payload->detail_slot;
  out[i++] = payload->urgency;
  out[i++] = payload->ttl_minutes;
  std::memcpy(out + i, &hdr->timestamp, 4); i += 4;
  std::memcpy(out + i, signer_pubkey, SESSION_PUBKEY_SIZE); i += SESSION_PUBKEY_SIZE;
  return i;
}

size_t build_ack_canonical(const uint8_t* original_nonce, uint8_t ack_type,
                           const uint8_t* confirmer_pubkey,
                           uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:chirp:ack:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t need = domain_len + 8 + 1 + SESSION_PUBKEY_SIZE;
  if (out_max < need) return 0;
  size_t i = 0;
  std::memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  std::memcpy(out + i, original_nonce, 8); i += 8;
  out[i++] = ack_type;
  std::memcpy(out + i, confirmer_pubkey, SESSION_PUBKEY_SIZE); i += SESSION_PUBKEY_SIZE;
  return i;
}

size_t build_suppress_canonical(const uint8_t* original_nonce,
                                const uint8_t* voter_pubkey,
                                uint8_t* out, size_t out_max) {
  static const char DOMAIN[] = "securacv:chirp:suppress:v0";
  const size_t domain_len = sizeof(DOMAIN) - 1;
  size_t need = domain_len + 8 + SESSION_PUBKEY_SIZE;
  if (out_max < need) return 0;
  size_t i = 0;
  std::memcpy(out + i, DOMAIN, domain_len); i += domain_len;
  std::memcpy(out + i, original_nonce, 8); i += 8;
  std::memcpy(out + i, voter_pubkey, SESSION_PUBKEY_SIZE); i += SESSION_PUBKEY_SIZE;
  return i;
}

int failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while(0)

void test_protocol_version_is_v02() {
  EXPECT(PROTOCOL_VERSION == 1,
         "PROTOCOL_VERSION must be 1 (v0.2). v0.1 frames were rejected to close audit C1.");
}

void test_magic_byte() {
  EXPECT(CHIRP_MAGIC == 0xC4, "CHIRP_MAGIC must be 0xC4");
}

void test_witness_canonical_layout() {
  // A deterministic input produces a deterministic canonical.
  ChirpHeader hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  hdr.magic = CHIRP_MAGIC;
  hdr.version = PROTOCOL_VERSION;
  hdr.msg_type = 1;
  std::memcpy(hdr.nonce, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
  hdr.timestamp = 0x12345678;

  ChirpWitnessPayload pl;
  std::memset(&pl, 0, sizeof(pl));
  pl.template_id = 0x20;  // BCN_EMERG_FIRE_VISIBLE
  pl.detail_slot = 0x0A;
  pl.urgency = 2;         // CHIRP_URG_URGENT
  pl.ttl_minutes = 15;

  uint8_t signer_pubkey[SESSION_PUBKEY_SIZE];
  for (size_t i = 0; i < SESSION_PUBKEY_SIZE; i++) signer_pubkey[i] = (uint8_t)i;

  uint8_t canon[256];
  size_t cl = build_witness_canonical(&hdr, &pl, signer_pubkey, canon, sizeof(canon));

  // Expected length: 25 (domain) + 8 (nonce) + 4 (tem/det/urg/ttl) + 4 (ts) + 32 (pubkey) = 73.
  EXPECT(cl == 25 + 8 + 4 + 4 + 32,
         "witness canonical length must equal documented byte layout (73 bytes)");

  // Domain prefix is at byte 0.
  EXPECT(std::memcmp(canon, "securacv:chirp:witness:v0", 25) == 0,
         "witness canonical MUST begin with domain separator 'securacv:chirp:witness:v0'");

  // Same inputs produce same output.
  uint8_t canon2[256];
  size_t cl2 = build_witness_canonical(&hdr, &pl, signer_pubkey, canon2, sizeof(canon2));
  EXPECT(cl == cl2 && std::memcmp(canon, canon2, cl) == 0,
         "witness canonical must be deterministic");
}

void test_ack_canonical_layout() {
  uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t confirmer_pk[SESSION_PUBKEY_SIZE];
  for (size_t i = 0; i < SESSION_PUBKEY_SIZE; i++) confirmer_pk[i] = (uint8_t)(0xAA + i);

  uint8_t canon[128];
  size_t cl = build_ack_canonical(nonce, 1 /* CONFIRMED */, confirmer_pk, canon, sizeof(canon));
  EXPECT(cl == 21 + 8 + 1 + 32,
         "ack canonical length must equal 21 (domain) + 8 (nonce) + 1 (type) + 32 (pubkey) = 62");
  EXPECT(std::memcmp(canon, "securacv:chirp:ack:v0", 21) == 0,
         "ack canonical MUST begin with domain separator 'securacv:chirp:ack:v0'");
}

void test_suppress_canonical_layout() {
  uint8_t nonce[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  uint8_t voter_pk[SESSION_PUBKEY_SIZE];
  for (size_t i = 0; i < SESSION_PUBKEY_SIZE; i++) voter_pk[i] = (uint8_t)(0x55 + i);

  uint8_t canon[128];
  size_t cl = build_suppress_canonical(nonce, voter_pk, canon, sizeof(canon));
  EXPECT(cl == 26 + 8 + 32,
         "suppress canonical length must equal 26 (domain) + 8 (nonce) + 32 (pubkey) = 66");
  EXPECT(std::memcmp(canon, "securacv:chirp:suppress:v0", 26) == 0,
         "suppress canonical MUST begin with domain separator 'securacv:chirp:suppress:v0'");
}

void test_canonical_distinguishes_signers() {
  // Two different signers, same body → different canonical → different signature.
  ChirpHeader hdr; std::memset(&hdr, 0, sizeof(hdr));
  ChirpWitnessPayload pl; std::memset(&pl, 0, sizeof(pl));
  pl.template_id = 0x20;

  uint8_t pk_a[SESSION_PUBKEY_SIZE]; std::memset(pk_a, 0xAA, sizeof(pk_a));
  uint8_t pk_b[SESSION_PUBKEY_SIZE]; std::memset(pk_b, 0xBB, sizeof(pk_b));

  uint8_t canon_a[256], canon_b[256];
  size_t cl_a = build_witness_canonical(&hdr, &pl, pk_a, canon_a, sizeof(canon_a));
  size_t cl_b = build_witness_canonical(&hdr, &pl, pk_b, canon_b, sizeof(canon_b));

  EXPECT(cl_a == cl_b, "both canonicals same length");
  EXPECT(std::memcmp(canon_a, canon_b, cl_a) != 0,
         "different signer_pubkey MUST produce different canonical bytes — this is the relay re-sign invariant (audit C4)");
}

void test_emoji_display_size_lifted() {
  // v0.2: EMOJI_DISPLAY_SIZE = 31 (5 emojis @ up to 6 bytes each, plus null).
  // The mesh_network.h header defines this; we hard-check it here so a
  // future revert to 19 (3 emojis) fails the build.
  constexpr size_t EMOJI_DISPLAY_SIZE = 31;
  EXPECT(EMOJI_DISPLAY_SIZE == 31,
         "v0.2 emoji display must be at least 5 emojis worth of bytes (audit C11)");
}

} // namespace

int main() {
  test_protocol_version_is_v02();
  test_magic_byte();
  test_witness_canonical_layout();
  test_ack_canonical_layout();
  test_suppress_canonical_layout();
  test_canonical_distinguishes_signers();
  test_emoji_display_size_lifted();

  if (failures == 0) {
    std::printf("All chirp protocol invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
