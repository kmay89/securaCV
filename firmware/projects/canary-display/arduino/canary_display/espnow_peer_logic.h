// ESP-NOW peer-decode logic — pure, host-testable (no Arduino, no esp_now).
//
// The one decision worth testing without a radio: is a raw ESP-NOW payload a
// well-formed fleet-link presence beacon, and if so what fp4 + status does it
// carry?
//
// That decision is NOT special to ESP-NOW — the same bytes arrive over BLE and
// over LAN multicast — so it lives once in canary/net/beacon_frame.h and this
// header is the ESP-NOW spelling of it. Keeping the name means the ESP-NOW
// call sites and their host test read as they always did while there is only
// one implementation to keep correct.

#ifndef CANARY_NET_ESPNOW_PEER_LOGIC_H
#define CANARY_NET_ESPNOW_PEER_LOGIC_H

#include <cstddef>
#include <cstdint>

#include "canary/net/beacon_frame.h"

namespace canary {
namespace net {

// Decode a raw ESP-NOW payload as a fleet-link presence beacon. See
// beacon_frame_decode() for the contract — this is that function, named for
// the transport that calls it. Shared by the runtime (src/net/espnow_peer.cpp)
// and tests_host/test_espnow_peer.cpp.
inline bool espnow_decode(const uint8_t* data, int len, char fp4_out[5],
                          canary::fleet::BeaconStatus& status_out,
                          bool& have_status) {
  return beacon_frame_decode(data, len, fp4_out, status_out, have_status);
}

}  // namespace net
}  // namespace canary

#endif  // CANARY_NET_ESPNOW_PEER_LOGIC_H
