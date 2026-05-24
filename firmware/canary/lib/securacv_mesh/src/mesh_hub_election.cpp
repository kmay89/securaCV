/*
 * SecuraCV Canary — Mesh Hub failover election
 * Version 0.1.0
 */

#include "mesh_hub_election.h"
#include <string.h>

namespace mesh_hub_election {

bool encode(Event           event,
            const uint8_t   fingerprint[FINGERPRINT_LEN],
            uint8_t*        out_buf,
            size_t          out_buf_cap) {
  if (out_buf == nullptr || fingerprint == nullptr) return false;
  if (out_buf_cap < PAYLOAD_LEN) return false;
  out_buf[0] = static_cast<uint8_t>(event);
  memcpy(out_buf + EVENT_LEN, fingerprint, FINGERPRINT_LEN);
  return true;
}

bool decode(const uint8_t*  in_buf,
            size_t          in_len,
            Event*          out_event,
            uint8_t         out_fingerprint[FINGERPRINT_LEN]) {
  if (in_buf == nullptr || out_event == nullptr || out_fingerprint == nullptr)
    return false;
  if (in_len != PAYLOAD_LEN) return false;
  *out_event = static_cast<Event>(in_buf[0]);
  memcpy(out_fingerprint, in_buf + EVENT_LEN, FINGERPRINT_LEN);
  return true;
}

int compare_fingerprints(const uint8_t a[FINGERPRINT_LEN],
                         const uint8_t b[FINGERPRINT_LEN]) {
  return memcmp(a, b, FINGERPRINT_LEN);
}

HubMonitor make_monitor(uint32_t timeout_ms) {
  HubMonitor m;
  m.timeout_ms             = timeout_ms;
  m.last_hub_heartbeat_ms  = 0;
  m.hub_known              = false;
  m.absence_fired          = false;
  return m;
}

void on_hub_heartbeat(HubMonitor& m, uint32_t now_ms) {
  m.last_hub_heartbeat_ms = now_ms;
  m.hub_known             = true;
  m.absence_fired         = false;
}

bool tick(HubMonitor& m, uint32_t now_ms) {
  if (!m.hub_known) return false;
  if (m.absence_fired) return false;
  if ((int32_t)(now_ms - m.last_hub_heartbeat_ms) >= (int32_t)m.timeout_ms) {
    m.absence_fired = true;
    return true;
  }
  return false;
}

void reset_election(HubMonitor& m) {
  m.hub_known      = false;
  m.absence_fired  = false;
}

}  /* namespace mesh_hub_election */
