/*
 * SecuraCV canary-wap — Opera pairing frame classifier
 *
 * THE BUG THIS FIXES (F14): mesh_network.cpp sent the five pairing messages
 * (DISCOVER / OFFER / ACCEPT / CONFIRM / COMPLETE) as the RAW payload
 * structs — 58, 98, 98, 32 and 60 bytes, no header of any kind — while its
 * receive path, handle_received_message(), dropped every frame shorter than
 * the 102-byte signed-header minimum before it looked at anything else. So
 * no pairing frame ever reached handle_pair_*: WAP-to-WAP Opera pairing over
 * ESP-NOW could not complete.
 *
 * THE FIX: every pairing frame now carries a 1-byte prefix — its own
 * MessageType value (MSG_PAIR_DISCOVER..MSG_PAIR_COMPLETE = 8..12) — ahead of
 * the unchanged payload struct, and the receive path classifies a frame
 * BEFORE the 102-byte gate:
 *
 *   [type 8..12][exactly sizeof(Pair*Payload) bytes]   → a pairing frame
 *   anything else                                       → not one (falls
 *                                                         through untouched)
 *
 * Why the first byte is unambiguous on this radio:
 *   • signed Opera frames start with mesh_network::PROTOCOL_VERSION = 0;
 *   • Chirp frames start with CHIRP_MAGIC = 0xC4, Beacon with 0xB1;
 *   • 8..12 is none of those, and the exact-length rule rejects everything
 *     that merely starts with such a byte.
 * Pairing is pre-membership, so these frames are unsigned by design (there
 * is no peer key yet to verify against — wrapping them in the signed
 * envelope was option C and was rejected for that reason); the pairing state
 * machine's own checks (roles, confirmation hash, AEAD on COMPLETE) are what
 * authenticate the exchange.
 *
 * This mirrors the PlatformIO mesh's framing (a 1-byte type ahead of the
 * raw pairing payload, firmware/canary/lib/securacv_mesh mesh_session.h),
 * but the two trees still number the types differently (PIO 0..4, WAP 8..12)
 * — they do not pair with each other, and this change does not claim so.
 *
 * Pure header: no Arduino, no ESP-IDF. The sizes and type values here are
 * this header's own constants so the host test can include it alone;
 * mesh_network.cpp static_asserts them against MessageType and the real
 * Pair*Payload structs, so the two cannot drift.
 * Host test: tests_host/test_mesh_pair_frame.cpp.
 */

#ifndef CANARY_WAP_MESH_PAIR_FRAME_H
#define CANARY_WAP_MESH_PAIR_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh_pair_frame {

/* mesh_network::MessageType values (mesh_network.h). */
constexpr uint8_t TYPE_DISCOVER = 8;
constexpr uint8_t TYPE_OFFER    = 9;
constexpr uint8_t TYPE_ACCEPT   = 10;
constexpr uint8_t TYPE_CONFIRM  = 11;
constexpr uint8_t TYPE_COMPLETE = 12;

/* sizeof(mesh_network::Pair*Payload) — all byte arrays, no padding. */
constexpr size_t DISCOVER_LEN = 32 + 25 + 1;        /* pubkey, name[25], role = 58 */
constexpr size_t OFFER_LEN    = 32 + 32 + 33 + 1;   /* eph pub, dev pub, opera name[33], count = 98 */
constexpr size_t ACCEPT_LEN   = OFFER_LEN;          /* ACCEPT reuses the OFFER struct */
constexpr size_t CONFIRM_LEN  = 32;                 /* confirmation hash */
constexpr size_t COMPLETE_LEN = 32 + 16 + 12;       /* secret + tag, nonce = 60 */

constexpr size_t PREFIX_LEN    = 1;
constexpr size_t MAX_FRAME_LEN = PREFIX_LEN + OFFER_LEN;

/* The exact payload length a pairing type carries, or 0 when `type` is not
 * a pairing type. */
inline size_t payload_len_for(uint8_t type) {
  switch (type) {
    case TYPE_DISCOVER: return DISCOVER_LEN;
    case TYPE_OFFER:    return OFFER_LEN;
    case TYPE_ACCEPT:   return ACCEPT_LEN;
    case TYPE_CONFIRM:  return CONFIRM_LEN;
    case TYPE_COMPLETE: return COMPLETE_LEN;
    default:            return 0;
  }
}

/* True iff `data` is a pairing frame: [type 8..12][exactly the payload
 * length that type carries]. On true, *type_out is the type and
 * *payload_out / *payload_len_out point at the payload inside `data`. On
 * false the out-parameters are untouched. */
inline bool classify(const uint8_t* data, size_t len,
                     uint8_t* type_out,
                     const uint8_t** payload_out, size_t* payload_len_out) {
  if (data == nullptr || type_out == nullptr ||
      payload_out == nullptr || payload_len_out == nullptr) {
    return false;
  }
  if (len < PREFIX_LEN) return false;
  const size_t want = payload_len_for(data[0]);
  if (want == 0 || len != PREFIX_LEN + want) return false;
  *type_out        = data[0];
  *payload_out     = data + PREFIX_LEN;
  *payload_len_out = want;
  return true;
}

/* Build [type][payload] into `out`. Returns the frame length, or 0 when
 * `type` is not a pairing type, payload_len is not that type's exact
 * length, or `out` is too small — the sender can only emit what the
 * receiver's classify() accepts. */
inline size_t build(uint8_t type, const void* payload, size_t payload_len,
                    uint8_t* out, size_t out_cap) {
  if (payload == nullptr || out == nullptr) return 0;
  const size_t want = payload_len_for(type);
  if (want == 0 || payload_len != want) return 0;
  if (out_cap < PREFIX_LEN + want) return 0;
  out[0] = type;
  memcpy(out + PREFIX_LEN, payload, want);
  return PREFIX_LEN + want;
}

}  // namespace mesh_pair_frame

#endif  // CANARY_WAP_MESH_PAIR_FRAME_H
