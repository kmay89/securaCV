/*
 * SecuraCV Canary — Mesh TAMPER_ALERT wire format
 * Version 0.1.0
 *
 * F10 (the alerts channel). Pins the payload of the opera-authenticated
 * TAMPER_ALERT frame (mesh_envelope::MsgType::TAMPER_ALERT = 18) so the
 * sender (mesh_session::send_tamper_alert) and the receiver
 * (mesh_session's dispatch_verified → alert ring + GET /api/mesh/alerts)
 * share one contract.
 *
 * Wire format (6 bytes, fixed — fits well under
 * mesh_envelope::MAX_PAYLOAD_LEN = 128):
 *
 *   offset 0 : kind         uint8_t  — Kind below (the dictionary's
 *                                      firmware tamper `kind` vocabulary,
 *                                      spec/witness_dictionary.json
 *                                      tamper_vocabularies)
 *   offset 1 : severity     uint8_t  — 0..7, the LogLevel scale
 *                                      (include/log_level.h)
 *   offset 2 : witness_seq  uint32_t — LE; the sender's witness-chain
 *                                      sequence number of the record the
 *                                      alert narrates, 0 = none
 *
 * Privacy: templates only. There is NO free-text detail on the wire (the
 * canary-wap TamperAlertPayload carries a 64-byte `detail` string; this
 * format deliberately does not). A receiver renders `kind` through
 * kind_name() and never shows a sender-authored string.
 *
 * Wire compatibility: PIO-only. canary-wap numbers its outer frame type
 * differently (MSG_TAMPER_ALERT = 4, not 18) and carries a different
 * payload struct, so the two trees do not exchange alerts; do not claim
 * cross-tree delivery (spec/canary_mesh_network_v0.md §8.3).
 *
 * This TU is pure logic: no Arduino, no mbedtls, no transport. It
 * encodes/decodes bytes and is host-build-clean.
 */

#ifndef SECURACV_MESH_ALERT_H
#define SECURACV_MESH_ALERT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_alert {

/* Tamper kind. Values are wire-stable; never renumber. The names are the
 * firmware tamper `kind` strings the MQTT tamper topic already publishes
 * (main.cpp's tamper drain) — no new vocabulary. */
enum class Kind : uint8_t {
  ENCLOSURE_TAMPER = 0,
  TEMP_DRIFT       = 1,
  CAMERA_TAMPER    = 2,
  /* 3..255 reserved. A receiver decodes an unknown value and renders it
   * as "unknown" (forward compat), it does not drop the frame. */
};

constexpr size_t  KIND_LEN        = 1;
constexpr size_t  SEVERITY_LEN    = 1;
constexpr size_t  WITNESS_SEQ_LEN = 4;
constexpr size_t  PAYLOAD_LEN     = KIND_LEN + SEVERITY_LEN + WITNESS_SEQ_LEN;  /* = 6 */
constexpr uint8_t MAX_SEVERITY    = 7;

/* Encode into exactly PAYLOAD_LEN bytes. Returns false on a null buffer,
 * out_cap < PAYLOAD_LEN, or severity > MAX_SEVERITY. */
bool encode(Kind     kind,
            uint8_t  severity,
            uint32_t witness_seq,
            uint8_t* out,
            size_t   out_cap);

/* Decode a PAYLOAD_LEN-byte payload. Returns false on a null pointer,
 * in_len != PAYLOAD_LEN (fixed size: trailing bytes would be signed but
 * unseen — same smuggling argument as mesh_beacon::decode), or a
 * severity byte > MAX_SEVERITY. An unknown kind byte decodes. */
bool decode(const uint8_t* in,
            size_t         in_len,
            Kind*          out_kind,
            uint8_t*       out_severity,
            uint32_t*      out_witness_seq);

/* "enclosure_tamper" / "temp_drift" / "camera_tamper" / "unknown". */
const char* kind_name(Kind kind);

/* The alert `type` string GET /api/mesh/alerts emits for every entry:
 * "TAMPER" — the string canary-wap's alert_type_name() uses for the same
 * class (the web UI shows it as the row's level label). */
const char* type_name();

/* One received alert, as the session's history ring stores it and the
 * REST builder renders it. timestamp_ms is the RECEIVER's uptime at
 * receipt (millis()), the same basis canary-wap's MeshAlert uses. */
struct Record {
  uint32_t timestamp_ms;
  uint8_t  sender_fp[8];        /* mesh_crypto::FINGERPRINT_LEN */
  Kind     kind;
  uint8_t  severity;
  uint32_t witness_seq;
};

}  /* namespace mesh_alert */

#endif  /* SECURACV_MESH_ALERT_H */
