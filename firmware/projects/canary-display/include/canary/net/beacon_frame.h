// One decode for every band that carries a presence beacon — pure and
// host-testable (no Arduino, no esp_now, no sockets).
//
// A fleet-link presence beacon arrives here three ways: as BLE manufacturer
// data, as an ESP-NOW payload, and as the body of a LAN-multicast datagram. In
// all three the bytes are the SAME blob (see firmware/common/fleet_link/), so
// there is exactly one function that decides "is this one of ours, and what
// does it say" — this one. Each transport module is then only a transport: it
// gets bytes off its radio and hands them here.
//
// That is deliberate. A per-band copy of this check is how bands drift into
// dialects: one accepts a length another rejects, and a Canary becomes visible
// on one radio but not another for reasons no one can see. Foreign traffic and
// noise are rejected here, before anything reaches the fleet model.

#ifndef CANARY_NET_BEACON_FRAME_H
#define CANARY_NET_BEACON_FRAME_H

#include <cstddef>
#include <cstdint>

#include "canary/net/beacon_parse.h"

namespace canary {
namespace net {

// Decode a raw frame body as a fleet-link presence beacon. Returns true and
// writes fp4_out (4 lowercase hex + NUL) when the body is a beacon of the
// exact expected length and company/type/version; `have_status` then says
// whether the status fields ([4..8]) also decoded into `status_out`. Returns
// false (writing nothing) on any mismatch.
inline bool beacon_frame_decode(const uint8_t* data, int len, char fp4_out[5],
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

#endif  // CANARY_NET_BEACON_FRAME_H
