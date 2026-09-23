/*
 * SecuraCV Canary — Mesh TAMPER_ALERT wire format — implementation
 *
 * Pure logic; no Arduino / mbedtls / transport. Host build and device
 * build link byte-identically.
 */

#include "mesh_alert.h"

namespace mesh_alert {

bool encode(Kind     kind,
            uint8_t  severity,
            uint32_t witness_seq,
            uint8_t* out,
            size_t   out_cap) {
  if (out == nullptr || out_cap < PAYLOAD_LEN) return false;
  if (severity > MAX_SEVERITY) return false;
  out[0] = static_cast<uint8_t>(kind);
  out[1] = severity;
  out[2] = static_cast<uint8_t>(witness_seq);
  out[3] = static_cast<uint8_t>(witness_seq >> 8);
  out[4] = static_cast<uint8_t>(witness_seq >> 16);
  out[5] = static_cast<uint8_t>(witness_seq >> 24);
  return true;
}

bool decode(const uint8_t* in,
            size_t         in_len,
            Kind*          out_kind,
            uint8_t*       out_severity,
            uint32_t*      out_witness_seq) {
  if (in == nullptr || out_kind == nullptr ||
      out_severity == nullptr || out_witness_seq == nullptr) {
    return false;
  }
  if (in_len != PAYLOAD_LEN) return false;
  if (in[1] > MAX_SEVERITY) return false;
  *out_kind        = static_cast<Kind>(in[0]);
  *out_severity    = in[1];
  *out_witness_seq = static_cast<uint32_t>(in[2])
                   | (static_cast<uint32_t>(in[3]) << 8)
                   | (static_cast<uint32_t>(in[4]) << 16)
                   | (static_cast<uint32_t>(in[5]) << 24);
  return true;
}

const char* kind_name(Kind kind) {
  switch (kind) {
    case Kind::ENCLOSURE_TAMPER: return "enclosure_tamper";
    case Kind::TEMP_DRIFT:       return "temp_drift";
    case Kind::CAMERA_TAMPER:    return "camera_tamper";
    default:                     return "unknown";
  }
}

const char* type_name() { return "TAMPER"; }

}  /* namespace mesh_alert */
