/*
 * SecuraCV Canary — Mesh beacon-event wire format — implementation
 *
 * Pure logic; no Arduino / mbedtls / transport. Host build and device
 * build link byte-identically.
 */

#include "mesh_beacon.h"

#include <string.h>

namespace mesh_beacon {

bool encode(BeaconState     state,
            const char*     label,
            uint8_t*        out_buf,
            size_t          out_buf_cap) {
  if (out_buf == nullptr) return false;
  if (out_buf_cap < PAYLOAD_LEN) return false;

  /* Zero the entire payload so the unused label tail is deterministic
   * (two encodings of the same logical content produce byte-identical
   * frames; useful for replay-defense and for golden-vector tests). */
  memset(out_buf, 0, PAYLOAD_LEN);

  out_buf[0] = static_cast<uint8_t>(state);

  /* Bounded label length. nullptr/"" → label_len=0. Otherwise count
   * up to MAX_LABEL_BYTES; we tolerate over-long labels by truncating
   * silently rather than failing, matching the pairing-side behaviour
   * where strncpy already caps at MAX_LABEL_LEN. */
  size_t lbl = 0;
  if (label != nullptr) {
    while (lbl < MAX_LABEL_BYTES && label[lbl] != '\0') ++lbl;
  }
  out_buf[1] = static_cast<uint8_t>(lbl);

  if (lbl > 0) {
    memcpy(out_buf + STATE_LEN + LABEL_LEN_LEN, label, lbl);
  }
  return true;
}

bool decode(const uint8_t*  in_buf,
            size_t          in_len,
            BeaconState*    out_state,
            char*           label_buf,
            size_t          label_buf_cap) {
  if (in_buf == nullptr || out_state == nullptr || label_buf == nullptr) {
    return false;
  }
  /* Strict length: the wire format is a fixed 25 bytes. Accepting
   * longer frames would let a sender smuggle trailing bytes that
   * receivers silently ignore — a forward-compat hazard and a real
   * security concern (the trailing bytes participate in the envelope
   * signature, but downstream consumers wouldn't see them). Future
   * BEACON_EVENT v2 with a longer payload should claim its own
   * MsgType, not extend this one. */
  if (in_len != PAYLOAD_LEN) return false;
  if (label_buf_cap < MAX_LABEL_BYTES + 1) return false;

  const uint8_t state_byte = in_buf[0];
  const uint8_t label_len  = in_buf[1];

  /* A label_len exceeding the on-wire budget is a malformed frame —
   * reject rather than trust. This is the only frame-shape check;
   * `state_byte` is forwarded verbatim so receivers can treat
   * unknown values as no-op (forward compat). */
  if (label_len > MAX_LABEL_BYTES) return false;

  *out_state = static_cast<BeaconState>(state_byte);

  if (label_len > 0) {
    memcpy(label_buf, in_buf + STATE_LEN + LABEL_LEN_LEN, label_len);
  }
  label_buf[label_len] = '\0';
  return true;
}

}  /* namespace mesh_beacon */
