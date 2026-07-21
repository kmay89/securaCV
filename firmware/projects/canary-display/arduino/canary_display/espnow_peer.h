// ESP-NOW peer presence — a router-independent second transport for the fleet.
//
// An attacker who kills the WiFi AP silences an MQTT-only fleet. ESP-NOW is
// connectionless and router-independent, so Canaries keep hearing each other on
// the raw 2.4 GHz channel. This is the DISPLAY side: it RECEIVES the same
// fleet-link presence beacon it already understands over BLE (canary/net/
// beacon_parse.h — one wire contract, one parser) and feeds it into the fleet
// model as an UNSIGNED observation (on_beacon), exactly like the BLE chirp path.
//
// Honesty rule: the dash has no signing identity — it verifies others' chains
// and TOFU-pins their keys, it never mints its own. So it never transmits and
// never "cross-signs"; it is a receive-only observer of peer liveness. A peer's
// signed chain head still travels over MQTT/the signed pipeline; ESP-NOW carries
// only the coarse presence summary the beacon already coarsens.
//
// Gated by FEATURE_ESPNOW (default 0). Compiled only in the
// canary-display-dash-espnow env; the default and emulator builds see an empty
// translation unit, so the wasm stays byte-for-byte identical. No stubs are
// needed — every caller in main.cpp is under the same gate.

#pragma once

#include <cstdint>

namespace canary {
namespace net {

// Bring ESP-NOW up on the current WiFi channel and register the receive
// callback (call after WiFi is initialized). Idempotent. Returns false if the
// ESP-NOW stack couldn't start. No-op returning false unless FEATURE_ESPNOW.
bool espnow_begin();

// True once espnow_begin() has succeeded.
bool espnow_ready();

// Drain received peer beacons into the fleet model. Cheap and self-throttling;
// safe to call every loop pass. `now` is millis().
void espnow_loop(uint32_t now);

// Count of well-formed peer beacons received this session (for diagnostics).
uint32_t espnow_seen();

}  // namespace net
}  // namespace canary
