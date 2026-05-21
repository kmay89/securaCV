/*
 * SecuraCV Canary — Mesh beacon-event wire format
 * Version 0.1.0
 *
 * PR 5c-2. Pins the BEACON_EVENT payload encoding so future PRs can
 * wire mesh_session::send_beacon_event() against a stable contract.
 *
 * Wire format (25 bytes total, fits well under mesh_envelope::MAX_PAYLOAD_LEN=128):
 *
 *   offset 0  : state          uint8_t  — 0=arrived, 1=departed
 *   offset 1  : label_len      uint8_t  — 0..23, NEVER includes the
 *                                          implicit NUL; receivers
 *                                          MUST NOT trust label as a
 *                                          C string until they apply
 *                                          their own NUL-termination
 *   offset 2  : label[23]      char     — sanitized printable ASCII
 *                                          (0x20..0x7E) at pair time
 *                                          by ble_scan::registry_add;
 *                                          unused bytes zeroed
 *
 * State byte values are an explicit enum (not a bool) so future
 * additions (e.g. "transient" for one-shot flyby observations)
 * can extend without breaking older receivers — they'd decode as
 * an unknown state and drop the event silently.
 *
 * The label is a length-prefixed byte sequence, NOT a NUL-terminated
 * C string. This is deliberate: rejecting frames whose label_len
 * exceeds the 23-byte budget is a build-time check, and consumers
 * that want a C string handle that conversion explicitly.
 *
 * Privacy: the wire format carries NO hashed_id, NO MAC, NO RSSI.
 * Receivers see only "<state> in <label>" — same posture as the local
 * csi_event_emit("ble.scout", "beacon_event") path, by design.
 *
 * This TU is pure logic: no Arduino, no mbedtls, no transport. It
 * encodes/decodes bytes and is host-build-clean.
 */

#ifndef SECURACV_MESH_BEACON_H
#define SECURACV_MESH_BEACON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_beacon {

/* Beacon transition state. Values are wire-stable; never renumber. */
enum class BeaconState : uint8_t {
  ARRIVED  = 0,
  DEPARTED = 1,
  /* 2..255 reserved for future extensions (e.g. TRANSIENT_OBSERVED). */
};

/* Wire-format constants. */
constexpr size_t  STATE_LEN       = 1;
constexpr size_t  LABEL_LEN_LEN   = 1;
constexpr size_t  MAX_LABEL_BYTES = 23;   /* room/beacon name max */
constexpr size_t  PAYLOAD_LEN     = STATE_LEN + LABEL_LEN_LEN + MAX_LABEL_BYTES;
/* = 25 bytes. */

/* Encode a (state, label) pair into a fixed 25-byte payload.
 *
 *   • `label` may be nullptr or "" — encoded as label_len=0.
 *   • `label` longer than MAX_LABEL_BYTES is truncated (no error).
 *   • Unused label bytes are zero-filled, NOT padded with spaces, so
 *     two encodings of the same label produce byte-identical frames.
 *
 * Returns true on success and writes exactly PAYLOAD_LEN bytes to
 * `out_buf`. Returns false only if `out_buf` is nullptr or
 * `out_buf_cap` < PAYLOAD_LEN. */
bool encode(BeaconState     state,
            const char*     label,
            uint8_t*        out_buf,
            size_t          out_buf_cap);

/* Decode a 25-byte payload into (state, label).
 *
 *   • `label_buf` must be at least MAX_LABEL_BYTES + 1 bytes (room
 *     for the implicit NUL terminator this function writes).
 *   • On success, `*out_state` is one of BeaconState (cast); the
 *     caller is responsible for treating unknown values as a no-op
 *     (forward compat).
 *   • The decoded label is NUL-terminated at `label_buf[label_len]`.
 *     Bytes after the NUL are unspecified.
 *
 * Returns false if:
 *   • any input pointer is null
 *   • `in_len` < PAYLOAD_LEN (truncated frame)
 *   • `label_buf_cap` < MAX_LABEL_BYTES + 1
 *   • `label_len` field exceeds MAX_LABEL_BYTES (frame is malformed) */
bool decode(const uint8_t*  in_buf,
            size_t          in_len,
            BeaconState*    out_state,
            char*           label_buf,
            size_t          label_buf_cap);

}  /* namespace mesh_beacon */

#endif  /* SECURACV_MESH_BEACON_H */
