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
 * The on-wire bytes are little-endian, matching canary-wap
 * mesh_network.cpp:386-394 (counter) and the existing `memcpy(...,
 * &timestamp, 4)` on a little-endian Xtensa/x86 build.
 *
 * Wire-compat with canary-wap (post-fix):
 *   • version, msg_type, opera_id, sender_fp, counter, timestamp,
 *     payload, signature byte order: identical to
 *     firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp
 *     {send_to_peer @367, handle_received_message @430}.
 *   • Signature covers data[0 .. HEADER_LEN + payload_len), i.e. the
 *     header and payload but NOT the signature bytes themselves
 *     (canary-wap line 405:
 *       sign_message(g_device_privkey, msg, offset + payload_len, sig)
 *     where `offset` == HEADER_LEN).
 *
 * What this module does NOT do (intentionally, deferred to higher
 * layers):
 *   • No opera-peer table. The caller passes signer_priv/pub for sends
 *     and peer_pub for verify. PR 2h will add an OperaPeer table to
 *     mesh_session and route the pubkey lookup.
 *   • No replay-counter table. The Header carries the counter on the
 *     wire; the caller checks it against per-peer state. PR 2h supplies
 *     that state machine.
 *   • No opera_id membership check. The caller passes the local
 *     opera_id and matches it. mesh_envelope just round-trips the
 *     value.
 *   • No NVS persistence. PR 2i / integration-layer concern.
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

/* Protocol version. Matches canary-wap PROTOCOL_VERSION constant.
 * Receivers reject frames with a different byte at offset 0. */
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
  /* 23..255 reserved for future use. */
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
