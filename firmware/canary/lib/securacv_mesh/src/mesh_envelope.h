/*
 * SecuraCV Canary — Mesh authenticated envelope
 * Version 0.1.0
 *
 * Wire-format helpers for opera-authenticated mesh traffic. Wraps an
 * opaque payload in a 38-byte MessageHeader and appends a 64-byte
 * Ed25519 signature over (header || payload). The receiver parses the
 * header, looks up the sender's public key (by fingerprint or by MAC),
 * verifies the signature, and surfaces the payload.
 *
 * Byte layout (38 + N + 64 bytes):
 *
 *   ┌─────────┬──────────┬──────────┬──────────────┬─────────┬──────────┬──────────┬───────────┐
 *   │ version │ msg_type │ opera_id │  sender_fp   │ counter │ timestmp │  payload │ signature │
 *   │   1     │    1     │   16     │      8       │   8     │    4     │    N     │    64     │
 *   │  (=1)   │          │          │              │ LE u64  │  LE u32  │          │           │
 *   └─────────┴──────────┴──────────┴──────────────┴─────────┴──────────┴──────────┴───────────┘
 *
 * The on-wire bytes are little-endian, the same order canary-wap's
 * send_to_peer() writes its counter in (mesh_network.cpp) and the
 * existing `memcpy(..., &timestamp, 4)` on a little-endian Xtensa/x86
 * build.
 *
 * Layout parity with canary-wap — NOT wire interop:
 *   • Field order and widths (version, msg_type, opera_id, sender_fp,
 *     counter, timestamp, payload, signature) mirror canary-wap's outer
 *     frame (firmware/projects/canary-wap/arduino/canary_wap/
 *     mesh_network.cpp send_to_peer / handle_received_message), and the
 *     signature covers data[0 .. HEADER_LEN + payload_len) the same way
 *     (header + payload, not the signature bytes; both hash with
 *     DOMAIN_MESSAGE before Ed25519).
 *   • The two trees still do not exchange frames: the version byte
 *     differs (PROTOCOL_VERSION below is 1; canary-wap's Opera frames
 *     carry mesh_network::PROTOCOL_VERSION = 0 and each side rejects the
 *     other's), and the msg_type numbering differs (MsgType below vs
 *     canary-wap's MessageType, e.g. TAMPER_ALERT 18 vs 4).
 *
 * What this module does NOT do — and where each piece lives instead:
 *   • The opera-peer table and the pubkey lookup: mesh_session's
 *     TrustedPeer table (register_trusted_peer / find_trusted_peer),
 *     keyed by sender_fp. Callers here pass signer_priv/pub for sends
 *     and peer_pub for verify.
 *   • The replay-counter check: mesh_session::on_opera_frame, per-peer
 *     strict-monotonic last_counter. The Header only carries the counter.
 *   • The opera_id membership check: mesh_session::on_opera_frame too.
 *     mesh_envelope just round-trips the value.
 *   • NVS persistence: mesh_state (opera_secret, trusted_peers,
 *     replay_ctrs, elected_hub, opera_name, mesh_enabled), called by the
 *     integration layer (firmware/canary/src/main.cpp).
 *
 * Threading: pure functions. No globals. Safe to call from any context.
 */

#ifndef SECURACV_MESH_ENVELOPE_H
#define SECURACV_MESH_ENVELOPE_H

#include "mesh_crypto.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_envelope {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

/* Protocol version. Receivers reject frames with a different byte at
 * offset 0. NOT canary-wap's Opera value (its mesh_network::
 * PROTOCOL_VERSION is 0; the 1 in its header belongs to chirp_channel) —
 * see "Layout parity" above. */
constexpr uint8_t PROTOCOL_VERSION = 1;

constexpr size_t VERSION_LEN      = 1;
constexpr size_t MSG_TYPE_LEN     = 1;
constexpr size_t OPERA_ID_LEN     = mesh_crypto::OPERA_ID_LEN;       /* 16 */
constexpr size_t FINGERPRINT_LEN  = mesh_crypto::FINGERPRINT_LEN;    /*  8 */
constexpr size_t COUNTER_LEN      = 8;
constexpr size_t TIMESTAMP_LEN    = 4;
constexpr size_t HEADER_LEN       = VERSION_LEN + MSG_TYPE_LEN
                                  + OPERA_ID_LEN + FINGERPRINT_LEN
                                  + COUNTER_LEN  + TIMESTAMP_LEN;
constexpr size_t SIGNATURE_LEN    = mesh_crypto::SIGNATURE_LEN;      /* 64 */

/* Header field byte offsets inside the signed envelope. Callers that
 * need to peek a single field without invoking parse_and_verify (e.g.
 * the receive dispatch peeks sender_fp to look up the verifying key
 * BEFORE the signature can be checked) should use these constants
 * rather than hand-rolled `1 + 1 + 16 + 8` arithmetic — keeps the
 * layout pinned in one place. */
constexpr size_t OFFSET_VERSION   = 0;
constexpr size_t OFFSET_MSG_TYPE  = OFFSET_VERSION   + VERSION_LEN;     /*  1 */
constexpr size_t OFFSET_OPERA_ID  = OFFSET_MSG_TYPE  + MSG_TYPE_LEN;    /*  2 */
constexpr size_t OFFSET_SENDER_FP = OFFSET_OPERA_ID  + OPERA_ID_LEN;    /* 18 */
constexpr size_t OFFSET_COUNTER   = OFFSET_SENDER_FP + FINGERPRINT_LEN; /* 26 */
constexpr size_t OFFSET_TIMESTAMP = OFFSET_COUNTER   + COUNTER_LEN;     /* 34 */
constexpr size_t OFFSET_PAYLOAD   = HEADER_LEN;                         /* 38 */

/* Minimum on-the-wire frame size: header + signature, zero-byte payload. */
constexpr size_t MIN_FRAME_LEN    = HEADER_LEN + SIGNATURE_LEN;       /* 102 */

/* Hard cap on payload size. ESP-NOW MTU is 250 bytes; with HEADER+SIG
 * overhead (102 bytes) the maximum payload is 148. We round down to
 * 128 for a clean number and to leave headroom for an outer encryption
 * envelope if a future slice adds one. */
constexpr size_t MAX_PAYLOAD_LEN  = 128;

constexpr size_t MAX_FRAME_LEN    = HEADER_LEN + MAX_PAYLOAD_LEN + SIGNATURE_LEN;

/* Message types for opera-authenticated traffic. The byte at offset 1
 * of every signed frame is one of these. Values 0..15 are reserved for
 * pre-membership pairing traffic (see mesh_session::MsgType — those
 * frames have a different, unsigned envelope). 16..255 here. */
enum class MsgType : uint8_t {
  HEARTBEAT      = 16,
  CSI_FEATURES   = 17,   /* PR 3 — 32-byte csi_features_t broadcast */
  TAMPER_ALERT   = 18,
  POWER_ALERT    = 19,
  OFFLINE_IMMINENT = 20,
  WITNESS_RECORD = 21,
  BEACON_EVENT   = 22,   /* PR 5c — ble.scout arrived/departed broadcast */
  CHANNEL_LOCK   = 23,   /* PR 4b — coordinated channel-hop proposal */
  HUB_ELECTION   = 24,   /* PR 4c — Hub failover election broadcast */
  LEAVE_OPERA    = 25,   /* F10 — "I am leaving"; empty payload. A verified
                          * LEAVE removes only the SIGNER's own trust entry
                          * at each receiver (spec §4.2, §8.3). */
  /* F10-rekey — opera_secret rotation on peer removal (mesh_rekey.h,
   * spec §5.6 PIO). All four ride signed envelopes under the CURRENT
   * opera_id. */
  REKEY_OFFER    = 26,   /* initiator → all: rekey_id, ephemeral X25519 pub, removed fp */
  REKEY_ACCEPT   = 27,   /* survivor → initiator: rekey_id, ephemeral X25519 pub */
  REKEY_SECRET   = 28,   /* initiator → survivor: rekey_id, nonce, AEAD(new secret) */
  REKEY_ACK      = 29,   /* survivor → initiator, sent BEFORE it switches */
  /* 30..255 reserved for future use. */
};

/* ──────────────────────────────────────────────────────────────────────────
 * HEADER STRUCT
 *
 * Decoded header. NOT laid out for memcpy from the wire — we serialize
 * field-by-field with explicit LE byte order so endianness drifts can't
 * silently break wire-compat. (The host build is x86 LE and the device
 * Xtensa LE so a struct memcpy would work today, but future ESP32-S2 /
 * RISC-V variants may differ. The explicit LE serialize is cheap.)
 * ────────────────────────────────────────────────────────────────────────── */

struct Header {
  uint8_t  version;
  uint8_t  msg_type;
  uint8_t  opera_id [OPERA_ID_LEN];
  uint8_t  sender_fp[FINGERPRINT_LEN];
  uint64_t counter;
  uint32_t timestamp;
};

/* ──────────────────────────────────────────────────────────────────────────
 * SERIALIZE
 *
 * Writes [header || payload || signature] into out_buf. Signs over
 * (header || payload). Returns the number of bytes written (always
 * HEADER_LEN + payload_len + SIGNATURE_LEN on success), or 0 on
 * failure (null pointer, payload too large, or buffer too small).
 *
 * The Header argument supplies the per-message fields the caller is
 * expected to fill: msg_type, opera_id, sender_fp, counter, timestamp.
 * The version field is forced to PROTOCOL_VERSION regardless of what
 * the caller passes (a stale or wrong value cannot leak onto the wire).
 *
 * signer_privkey + signer_pubkey are the long-term Ed25519 keypair of
 * the sending device. The pubkey is needed because rweather's
 * Ed25519::sign API requires it alongside the private scalar; on
 * device the canary-wap implementation matches this shape (mesh_
 * network.cpp:264-270).
 * ────────────────────────────────────────────────────────────────────────── */

size_t serialize_signed(const Header&  header,
                        const uint8_t* payload, size_t payload_len,
                        const uint8_t  signer_privkey[mesh_crypto::PRIVKEY_LEN],
                        const uint8_t  signer_pubkey [mesh_crypto::PUBKEY_LEN],
                        uint8_t* out_buf, size_t out_buf_cap);

/* ──────────────────────────────────────────────────────────────────────────
 * PARSE + VERIFY
 *
 * Reads [header || payload || signature] from `frame` and:
 *   1. Validates frame_len >= MIN_FRAME_LEN and version == PROTOCOL_VERSION.
 *   2. Decodes the header into *out_header (LE byte order).
 *   3. Verifies the trailing 64-byte Ed25519 signature against the
 *      bytes data[0 .. HEADER_LEN + payload_len) using peer_pubkey.
 *
 * On success, *out_payload points into `frame` (no copy; lifetime
 * matches `frame`) and *out_payload_len is the payload bytes between
 * the header and signature.
 *
 * Returns false on:
 *   • frame == nullptr or frame_len < MIN_FRAME_LEN
 *   • version != PROTOCOL_VERSION
 *   • signature verification fails (corrupt/forged frame)
 *
 * Does NOT check:
 *   • opera_id membership (caller's job)
 *   • sender_fp matches peer_pubkey (caller's job — the peer-table
 *     lookup THAT supplied peer_pubkey is what binds the two)
 *   • counter > last_seen (caller's job, replay defense)
 *   • timestamp freshness (audit O1 in #450 — not security-bearing)
 * ────────────────────────────────────────────────────────────────────────── */

bool parse_and_verify(const uint8_t* frame, size_t frame_len,
                      const uint8_t  peer_pubkey[mesh_crypto::PUBKEY_LEN],
                      Header*        out_header,
                      const uint8_t** out_payload, size_t* out_payload_len);

}  /* namespace mesh_envelope */

#endif  /* SECURACV_MESH_ENVELOPE_H */
