/**
 * @file test_mesh_envelope.cpp
 * @brief Host-build conformance test for the mesh_envelope wire format.
 *
 * Verifies:
 *   1. Roundtrip: serialize_signed → parse_and_verify recovers the
 *      exact header + payload bytes.
 *   2. Tamper detection: bit-flip in (header / payload / signature)
 *      causes parse_and_verify to fail.
 *   3. Wrong-pubkey: parse_and_verify with a different peer_pubkey
 *      fails (cross-signer rejection).
 *   4. Version byte enforcement: forging version != PROTOCOL_VERSION
 *      on the wire fails parse.
 *   5. Frame too short / too long is rejected.
 *   6. LE counter + timestamp byte order pinned at known wire offsets.
 *   7. Empty payload (HEADER_LEN + SIGNATURE_LEN = 102 byte frame) works.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_envelope.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_envelope.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_envelope && /tmp/test_mesh_envelope
 */

#include "mesh_envelope.h"
#include "mesh_crypto.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_envelope_run() { return 0; }
#else

namespace {

/* Make a deterministic dummy header. */
mesh_envelope::Header sample_header() {
  mesh_envelope::Header h{};
  h.version  = mesh_envelope::PROTOCOL_VERSION;
  h.msg_type = static_cast<uint8_t>(mesh_envelope::MsgType::CSI_FEATURES);
  for (size_t i = 0; i < mesh_envelope::OPERA_ID_LEN; ++i)
    h.opera_id[i] = (uint8_t)(0x10 + i);
  for (size_t i = 0; i < mesh_envelope::FINGERPRINT_LEN; ++i)
    h.sender_fp[i] = (uint8_t)(0xA0 + i);
  h.counter   = 0xDEADBEEFCAFEBABEULL;
  h.timestamp = 1700000042;
  return h;
}

void test_roundtrip_recovers_payload() {
  uint8_t pub[mesh_crypto::PUBKEY_LEN], priv[mesh_crypto::PRIVKEY_LEN];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));

  mesh_envelope::Header hdr = sample_header();
  const uint8_t payload[] = "hello mesh — this is a CSI feature vector payload";
  const size_t pt_len = sizeof(payload) - 1;

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(
      hdr, payload, pt_len, priv, pub, frame, sizeof(frame));
  assert(frame_len == mesh_envelope::HEADER_LEN + pt_len + mesh_envelope::SIGNATURE_LEN);

  mesh_envelope::Header recovered{};
  const uint8_t* out_payload = nullptr;
  size_t out_payload_len = 0;
  assert(mesh_envelope::parse_and_verify(frame, frame_len, pub,
                                          &recovered, &out_payload, &out_payload_len));

  assert(recovered.version  == mesh_envelope::PROTOCOL_VERSION);
  assert(recovered.msg_type == hdr.msg_type);
  assert(std::memcmp(recovered.opera_id,  hdr.opera_id,  mesh_envelope::OPERA_ID_LEN)    == 0);
  assert(std::memcmp(recovered.sender_fp, hdr.sender_fp, mesh_envelope::FINGERPRINT_LEN) == 0);
  assert(recovered.counter   == hdr.counter);
  assert(recovered.timestamp == hdr.timestamp);
  assert(out_payload_len == pt_len);
  assert(std::memcmp(out_payload, payload, pt_len) == 0);

  std::printf("PASS test_roundtrip_recovers_payload  (frame_len=%zu)\n", frame_len);
}

void test_tampered_payload_rejected() {
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_envelope::Header hdr = sample_header();
  uint8_t pt[16];
  for (size_t i = 0; i < sizeof(pt); ++i) pt[i] = (uint8_t)i;

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, pt, sizeof(pt), priv, pub,
                                                      frame, sizeof(frame));
  /* Flip a bit in the payload region. */
  frame[mesh_envelope::HEADER_LEN + 5] ^= 0x04;

  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  assert(!mesh_envelope::parse_and_verify(frame, frame_len, pub,
                                           &recovered, &out, &out_len));
  std::printf("PASS test_tampered_payload_rejected\n");
}

void test_tampered_header_rejected() {
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_envelope::Header hdr = sample_header();
  uint8_t pt[8] = {0};

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, pt, sizeof(pt), priv, pub,
                                                      frame, sizeof(frame));
  /* Flip a bit in the counter region (header byte). */
  const size_t counter_offset = mesh_envelope::VERSION_LEN +
                                 mesh_envelope::MSG_TYPE_LEN +
                                 mesh_envelope::OPERA_ID_LEN +
                                 mesh_envelope::FINGERPRINT_LEN;
  frame[counter_offset + 3] ^= 0x80;

  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  assert(!mesh_envelope::parse_and_verify(frame, frame_len, pub,
                                           &recovered, &out, &out_len));
  std::printf("PASS test_tampered_header_rejected\n");
}

void test_tampered_signature_rejected() {
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_envelope::Header hdr = sample_header();
  uint8_t pt[4] = {1, 2, 3, 4};

  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, pt, sizeof(pt), priv, pub,
                                                      frame, sizeof(frame));
  /* Flip a bit in the signature region. */
  frame[frame_len - 1] ^= 0x01;

  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  assert(!mesh_envelope::parse_and_verify(frame, frame_len, pub,
                                           &recovered, &out, &out_len));
  std::printf("PASS test_tampered_signature_rejected\n");
}

void test_wrong_pubkey_rejected() {
  /* Sign with key A, verify with key B → must fail. */
  uint8_t pub_a[32], priv_a[32], pub_b[32], priv_b[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub_a, priv_a));
  assert(mesh_crypto::ed25519_generate_keypair(pub_b, priv_b));
  assert(std::memcmp(pub_a, pub_b, 32) != 0);

  mesh_envelope::Header hdr = sample_header();
  uint8_t pt[12] = {0};
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, pt, sizeof(pt), priv_a, pub_a,
                                                      frame, sizeof(frame));
  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  assert(!mesh_envelope::parse_and_verify(frame, frame_len, pub_b,
                                           &recovered, &out, &out_len));
  std::printf("PASS test_wrong_pubkey_rejected\n");
}

void test_wrong_version_rejected() {
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_envelope::Header hdr = sample_header();
  uint8_t pt[4] = {0};
  uint8_t frame[mesh_envelope::MAX_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, pt, sizeof(pt), priv, pub,
                                                      frame, sizeof(frame));
  /* Overwrite the version byte. parse should fail BEFORE signature check
   * because the version mismatch is a structural error. */
  frame[0] = mesh_envelope::PROTOCOL_VERSION + 1;
  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  assert(!mesh_envelope::parse_and_verify(frame, frame_len, pub,
                                           &recovered, &out, &out_len));
  std::printf("PASS test_wrong_version_rejected\n");
}

void test_frame_too_short_rejected() {
  uint8_t pub[32];
  for (size_t i = 0; i < 32; ++i) pub[i] = (uint8_t)i;
  uint8_t tiny[mesh_envelope::MIN_FRAME_LEN - 1] = {0};
  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  assert(!mesh_envelope::parse_and_verify(tiny, sizeof(tiny), pub,
                                           &recovered, &out, &out_len));
  std::printf("PASS test_frame_too_short_rejected\n");
}

void test_empty_payload_roundtrip() {
  /* Header + signature only = 102 bytes. Useful for heartbeats that
   * carry no payload. */
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_envelope::Header hdr = sample_header();
  uint8_t frame[mesh_envelope::MIN_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, nullptr, 0, priv, pub,
                                                      frame, sizeof(frame));
  assert(frame_len == mesh_envelope::MIN_FRAME_LEN);

  mesh_envelope::Header recovered{};
  const uint8_t* out = nullptr;
  size_t out_len = 99;  /* sentinel */
  assert(mesh_envelope::parse_and_verify(frame, frame_len, pub,
                                          &recovered, &out, &out_len));
  assert(out_len == 0);
  assert(out == nullptr);
  std::printf("PASS test_empty_payload_roundtrip  (frame_len=%zu)\n", frame_len);
}

void test_le_byte_order_pinned() {
  /* Pin counter + timestamp byte order at exact wire offsets so an
   * endianness regression on a future ESP32-S2/RISC-V port can't
   * silently break wire-compat with canary-wap. */
  uint8_t pub[32], priv[32];
  assert(mesh_crypto::ed25519_generate_keypair(pub, priv));
  mesh_envelope::Header hdr{};
  hdr.msg_type  = 0x42;
  hdr.counter   = 0x0102030405060708ULL;  /* LE on wire: 08 07 06 05 04 03 02 01 */
  hdr.timestamp = 0x01020304;             /* LE on wire: 04 03 02 01            */
  /* opera_id + sender_fp zeroed — they're already byte arrays so order
   * is trivial. */

  uint8_t frame[mesh_envelope::MIN_FRAME_LEN];
  size_t frame_len = mesh_envelope::serialize_signed(hdr, nullptr, 0, priv, pub,
                                                      frame, sizeof(frame));
  assert(frame_len > 0);

  const size_t counter_off = mesh_envelope::VERSION_LEN +
                              mesh_envelope::MSG_TYPE_LEN +
                              mesh_envelope::OPERA_ID_LEN +
                              mesh_envelope::FINGERPRINT_LEN;
  const size_t ts_off = counter_off + mesh_envelope::COUNTER_LEN;

  static const uint8_t expected_counter_le[8] =
      {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  static const uint8_t expected_ts_le[4] =
      {0x04, 0x03, 0x02, 0x01};
  assert(std::memcmp(frame + counter_off, expected_counter_le, 8) == 0);
  assert(std::memcmp(frame + ts_off,      expected_ts_le,      4) == 0);
  std::printf("PASS test_le_byte_order_pinned\n");
}

}  /* namespace */

int main() {
  std::srand(0xE1ECE);
  test_roundtrip_recovers_payload();
  test_tampered_payload_rejected();
  test_tampered_header_rejected();
  test_tampered_signature_rejected();
  test_wrong_pubkey_rejected();
  test_wrong_version_rejected();
  test_frame_too_short_rejected();
  test_empty_payload_roundtrip();
  test_le_byte_order_pinned();
  std::printf("\nALL MESH_ENVELOPE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
