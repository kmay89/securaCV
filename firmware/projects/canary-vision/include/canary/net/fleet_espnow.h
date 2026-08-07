#pragma once
#include <stdint.h>

// Fleet-link presence beacon — the ESP-NOW carrier (send side).
//
// The router-free band. BLE needs the NimBLE stack (which small displays
// compile out for flash budget) and UDP multicast needs a home network both
// devices have joined; this band needs neither — two ESP32s straight out of
// the box hear each other on it. It is what makes the First Light pair demo
// (docs/first_light_demo.md) work with no WiFi, no hub, no app: a Vision and
// a Nightlight powered on a desk, and nothing else.
//
// It carries the SAME bytes as the other carriers, built once in
// canary/net/fleet_beacon_payload.h. This module owns transmission only —
// the channel policy it follows is documented in the wire contract
// (firmware/common/fleet_link/fleet_beacon_espnow.h).
//
// Gated by FEATURE_FLEET_ESPNOW (canary/config.h, default 1). When 0 these
// become no-ops so a board's OTA-slot size guard can veto the carrier
// without touching any call site.

namespace canary::net {

// Prepare the carrier. Requires the WiFi driver to be started (STA mode),
// which wifi_init_or_reboot() does even on an unprovisioned unit; a stack
// that cannot come up degrades to a no-op, never a boot failure.
void fleet_espnow_begin(uint32_t now);

// Send when there is something to say. Cheap to call every loop() pass;
// internally rate-limited to the beacon refresh cadence, with a detection
// edge sending immediately. On a never-provisioned unit this is also where
// the radio is parked on the fallback channel so a boxed pair meets with
// zero configuration.
void fleet_espnow_tick(uint32_t now);

// Frames actually put on the air this boot — a diagnostics counter, not a
// delivery guarantee (broadcast ESP-NOW is best-effort and unacknowledged).
uint32_t fleet_espnow_sent();

}  // namespace canary::net
