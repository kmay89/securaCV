// ESP-NOW peer-decode logic — pure, host-testable (no Arduino, no esp_now).
//
// The one decision worth testing without a radio: is a raw ESP-NOW payload a
// well-formed fleet-link presence beacon, and if so what fp4 + status does it
// carry? It reuses the shared beacon wire contract (canary/net/beacon_parse.h —
// the SAME parser the BLE chirp path uses), so an ESP-NOW frame and a BLE advert
// from the same canary resolve to the same witness. Foreign ESP-NOW traffic and
// noise are rejected here before anything reaches the fleet model.

#ifndef CANARY_NET_ESPNOW_PEER_LOGIC_H
#define CANARY_NET_ESPNOW_PEER_LOGIC_H

#include <cstddef>
#include <cstdint>

#include "canary/net/beacon_parse.h"

namespace canary {
namespace net {

// Decode a raw ESP-NOW payload as a fleet-link presence beacon. Returns true and
// writes fp4_out (4 lowercase hex + NUL) when the payload is a beacon of the
// exact expected length and company/type/version; `have_status` then says
// whether the status fields ([4..8]) also decoded into `status_out`. Returns
// false (writing nothing) on any mismatch. Shared by the runtime
// (src/net/espnow_peer.cpp) and tests_host/test_espnow_peer.cpp.
inline bool espnow_decode(const uint8_t* data, int len, char fp4_out[5],
                          canary::fleet::BeaconStatus& status_out,
                          bool& have_status) {
  have_status = false;
  if (!data || len <= 0) return false;
  if (len != (int)BEACON_MFG_LEN && len != (int)BEACON_MFG_V2_LEN) return false;
  if (!beacon_fp4_from_mfg(data, (size_t)len, fp4_out)) return false;
  have_status = beacon_parse_status(data, (size_t)len, status_out);
  return true;
}

}  // namespace net
}  // namespace canary

#endif  // CANARY_NET_ESPNOW_PEER_LOGIC_H
