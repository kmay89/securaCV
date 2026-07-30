// common/link/link_frame.h — the Canary Link wire frame: header layout,
// discrimination, and AEAD nonce construction. Pure, host-testable (no
// Arduino, no esp_now, no mbedtls): the same bytes run on-device and in
// tests_host/test_link_frame.cpp.
//
// Canary Link is the small authenticated datagram layer under the Tin Can's
// "strings" (docs/design/canary_tincan_kids_watch.md §5.5). It is deliberately
// NOT kid-specific — the payload `kind` is an opaque byte here — because the
// same session/replay/relay machinery is what an industrial deployment needs:
// many nodes, one radio, no broker, no cloud, and messages that survive the
// router going down.
//
// Two things this header exists to get right, both of which are easy to get
// wrong and fatal when you do:
//
//  1. NONCE UNIQUENESS ACROSS BOTH DIRECTIONS. Two peers sharing one key and
//     each counting from zero will hand the same key/nonce pair to two
//     different plaintexts on their first frame. That single mistake destroys
//     both confidentiality and authenticity under ChaCha20-Poly1305 and
//     AES-GCM alike. The defence is layered: peers derive SEPARATE directional
//     keys (link_session.h), and the nonce built here still carries `dir` and
//     `session_id` explicitly, so a key mix-up cannot silently collide either.
//     The nonce is fully recoverable from the frame — never random, never
//     implicit.
//
//  2. MUTABLE FIELDS MUST NOT BE AUTHENTICATED. A relay decrements `ttl` and
//     increments `hops`, so those two bytes CANNOT sit inside the AEAD's
//     additional data — an end-to-end tag over a field the middle is allowed
//     to change would fail on every forwarded frame. They are therefore
//     UNTRUSTED: usable for loop prevention and nothing else. No security
//     decision may read them. See link_relay.h.
//
// Frame layout (little-endian; header is 18 bytes):
//
//   off  size  field        authenticated?
//   ---  ----  -----------  --------------
//    0     2   magic        yes   0xCA 0x5E — never 0xFFFF, see below
//    2     1   ver          yes
//    3     1   kind         yes   opaque payload class (payload layer owns it)
//    4     2   session_id   yes   which string/peering this belongs to
//    6     1   dir          yes   0 = A->B, 1 = B->A (role from link_session.h)
//    7     1   flags        yes   reserved, must be 0 in v1
//    8     8   ctr          yes   monotonic per (session, dir)
//   16     1   hops         NO    incremented by each relay
//   17     1   ttl          NO    decremented by each relay
//   18     n   ciphertext   (the AEAD output over the payload)
//   18+n  16   tag
//
//   AAD = bytes[0..15] — everything above `hops`.
//   nonce(12) = dir(1) | session_id(2, LE) | 0x00 | ctr(8, LE)
//
// Why the magic is 0xCA5E: this layer shares the air with the fleet-link
// presence beacon (common/fleet_link, canary/net/beacon_parse.h), whose frames
// are exactly 11 bytes beginning 0xFF 0xFF 0x10 0x01. A link frame can never
// be mistaken for one — different first byte, and a floor length well above 11
// — so one receive callback can carry both without either parser guessing.

#ifndef CANARY_LINK_LINK_FRAME_H
#define CANARY_LINK_LINK_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace link {

static constexpr uint8_t LINK_MAGIC0 = 0xCA;
static constexpr uint8_t LINK_MAGIC1 = 0x5E;
static constexpr uint8_t LINK_VER = 0x01;

static constexpr size_t LINK_HEADER_LEN = 18;
static constexpr size_t LINK_TAG_LEN = 16;
static constexpr size_t LINK_NONCE_LEN = 12;

// AAD covers the header up to (not including) the mutable relay bytes.
static constexpr size_t LINK_AAD_LEN = 16;

// ESP-NOW's payload ceiling is 250 bytes. Leave the header and tag room and
// round down hard: the Tin Can's largest payload is a doodle stroke chunk, and
// keeping frames small is what keeps a house full of nodes quiet.
static constexpr size_t LINK_MAX_PAYLOAD = 180;
static constexpr size_t LINK_MAX_FRAME =
    LINK_HEADER_LEN + LINK_MAX_PAYLOAD + LINK_TAG_LEN;
static constexpr size_t LINK_MIN_FRAME = LINK_HEADER_LEN + LINK_TAG_LEN;

// Default hop budget. A house is one or two hops; the ceiling exists so a
// forwarding loop dies quickly rather than saturating the channel.
static constexpr uint8_t LINK_DEFAULT_TTL = 3;
static constexpr uint8_t LINK_MAX_TTL = 8;

// Direction. Which peer is A and which is B is decided once, at pairing, by
// link_session.h — never negotiated per frame.
enum class Dir : uint8_t {
  AtoB = 0,
  BtoA = 1,
};

// The parsed, plaintext header. `hops`/`ttl` are carried but marked untrusted
// by the field comment above; nothing in this struct is decrypted yet.
struct LinkHeader {
  uint8_t ver = 0;
  uint8_t kind = 0;
  uint16_t session_id = 0;
  Dir dir = Dir::AtoB;
  uint8_t flags = 0;
  uint64_t ctr = 0;
  uint8_t hops = 0;  // UNTRUSTED — loop prevention only
  uint8_t ttl = 0;   // UNTRUSTED — loop prevention only
  size_t payload_len = 0;
};

// ---------------------------------------------------------------------------
// Little-endian helpers. Explicit rather than memcpy-of-a-struct so the wire
// format is identical on any host that runs the tests.
// ---------------------------------------------------------------------------

inline void put_u16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

inline uint16_t get_u16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

inline void put_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

inline uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
  return v;
}

// ---------------------------------------------------------------------------
// Discrimination — answer "is this even ours?" before any parsing
// ---------------------------------------------------------------------------

// True if `data` could be a Canary Link frame. Cheap and total: safe to call
// on any inbound buffer, including a fleet presence beacon or pure noise.
inline bool link_is_frame(const uint8_t* data, size_t len) {
  if (!data) return false;
  if (len < LINK_MIN_FRAME || len > LINK_MAX_FRAME) return false;
  return data[0] == LINK_MAGIC0 && data[1] == LINK_MAGIC1;
}

// ---------------------------------------------------------------------------
// Nonce
// ---------------------------------------------------------------------------

// Build the 12-byte AEAD nonce for (dir, session_id, ctr). Deterministic and
// recoverable from the frame — the whole point. `out` must hold 12 bytes.
inline void link_nonce(Dir dir, uint16_t session_id, uint64_t ctr,
                       uint8_t out[LINK_NONCE_LEN]) {
  out[0] = (uint8_t)dir;
  put_u16(out + 1, session_id);
  out[3] = 0x00;  // reserved; keeps ctr 8-byte aligned within the nonce
  put_u64(out + 4, ctr);
}

// ---------------------------------------------------------------------------
// Header encode / decode
//
// These do NOT encrypt. Encryption is the runtime's job (mbedtls on-device);
// keeping the crypto call out of this header is what lets the format, the
// nonce derivation and the replay window be tested with no crypto library at
// all — and it keeps one file to review for "is the nonce right?".
// ---------------------------------------------------------------------------

// Write the 18-byte header for a frame carrying `payload_len` bytes of
// ciphertext. Returns false if the payload would not fit. `hops` starts at 0.
inline bool link_write_header(uint8_t* out, size_t out_cap, uint8_t kind,
                              uint16_t session_id, Dir dir, uint64_t ctr,
                              uint8_t ttl, size_t payload_len) {
  if (!out) return false;
  if (payload_len > LINK_MAX_PAYLOAD) return false;
  if (out_cap < LINK_HEADER_LEN + payload_len + LINK_TAG_LEN) return false;
  if (ttl == 0 || ttl > LINK_MAX_TTL) return false;

  out[0] = LINK_MAGIC0;
  out[1] = LINK_MAGIC1;
  out[2] = LINK_VER;
  out[3] = kind;
  put_u16(out + 4, session_id);
  out[6] = (uint8_t)dir;
  out[7] = 0x00;  // flags, reserved
  put_u64(out + 8, ctr);
  out[16] = 0;    // hops
  out[17] = ttl;
  return true;
}

// Parse and validate a frame header. Returns false — writing nothing useful —
// on anything that isn't a well-formed v1 frame: bad magic, wrong version,
// reserved flags set, a length that doesn't leave room for a tag, or a payload
// over the cap. Rejecting here is what keeps foreign traffic away from the
// session and payload layers entirely.
inline bool link_parse_header(const uint8_t* data, size_t len,
                              LinkHeader& out) {
  if (!link_is_frame(data, len)) return false;
  if (data[2] != LINK_VER) return false;
  if (data[7] != 0x00) return false;  // unknown flags — refuse, don't ignore

  const size_t payload_len = len - LINK_HEADER_LEN - LINK_TAG_LEN;
  if (payload_len > LINK_MAX_PAYLOAD) return false;

  const uint8_t dir_raw = data[6];
  if (dir_raw > 1) return false;

  out.ver = data[2];
  out.kind = data[3];
  out.session_id = get_u16(data + 4);
  out.dir = (Dir)dir_raw;
  out.flags = data[7];
  out.ctr = get_u64(data + 8);
  out.hops = data[16];
  out.ttl = data[17];
  out.payload_len = payload_len;
  return true;
}

// The AAD span for a frame: the authenticated header prefix. Callers hand this
// to the AEAD as additional data so magic/ver/kind/session/dir/ctr are all
// covered by the tag — and `hops`/`ttl` deliberately are not.
inline const uint8_t* link_aad(const uint8_t* data) { return data; }
inline size_t link_aad_len() { return LINK_AAD_LEN; }

// Pointer to where ciphertext begins.
inline uint8_t* link_payload(uint8_t* data) { return data + LINK_HEADER_LEN; }
inline const uint8_t* link_payload(const uint8_t* data) {
  return data + LINK_HEADER_LEN;
}

}  // namespace link
}  // namespace canary

#endif  // CANARY_LINK_LINK_FRAME_H
