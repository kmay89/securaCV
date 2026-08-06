#pragma once
#include <stdint.h>

// Fleet-link presence beacon — the LAN-multicast carrier (send side).
//
// The band that reaches across a house. BLE and ESP-NOW are both limited to
// direct radio range; this one rides the home WiFi the devices are ALREADY
// joined to, so a Vision in the driveway reaches a Display upstairs. Still no
// broker, no hub, no pairing, no cloud, no configuration — see
// firmware/common/fleet_link/fleet_beacon_udp.h for the wire contract and for
// why the datagram body is the beacon blob verbatim.
//
// It carries the SAME bytes as the BLE advert, built once in
// canary/net/fleet_beacon_payload.h. This module owns transmission only.
//
// Gated by FEATURE_FLEET_UDP (canary/config.h, default 1). When 0 these become
// no-ops so a board's OTA-slot size guard can veto the carrier without
// touching any call site.

namespace canary::net {

// Prepare the carrier. Cheap and safe to call before WiFi is up — the socket
// is opened lazily by the tick once there is a link to open it on.
void fleet_udp_begin(uint32_t now);

// Send when there is something to say and a link to say it on. Cheap to call
// every loop() pass; internally rate-limited to the beacon refresh cadence,
// with a detection edge sending immediately.
//
// Self-healing by construction: the carrier holds no "connected" state of its
// own beyond a socket it will close and reopen. Losing STA closes the socket;
// regaining it opens a fresh one on the new address. There is nothing to
// reset, and no path where a dead link leaves the carrier believing otherwise.
void fleet_udp_tick(uint32_t now);

// Datagrams actually put on the wire this boot — a diagnostics counter, not a
// delivery guarantee (multicast is best-effort and nothing acknowledges it).
uint32_t fleet_udp_sent();

}  // namespace canary::net
