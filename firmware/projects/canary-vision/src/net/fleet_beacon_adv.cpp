// src/net/fleet_beacon_adv.cpp — fleet-link presence beacon, BLE carrier.
//
// A minimal NimBLE advertiser that broadcasts the canonical fleet-link presence
// beacon (fleet_beacon.h) as manufacturer data. A canary-display resolves this
// witness directly over BLE — broker-free and WiFi-free. No GATT server and no
// scan response: these devices carry no service UUIDs to preserve, so the
// beacon IS the primary (and only) advert. Flash cost stays minimal —
// advertise-only, no services, no characteristics.
//
// This file no longer decides what the beacon SAYS. The payload is built once
// in fleet_beacon_payload.cpp and every carrier transmits that same buffer
// verbatim, so BLE and the LAN-multicast carrier cannot drift into dialects of
// the same beacon. This module is now purely "put these bytes on the BLE
// radio, and keep doing so".
//
// Cross-core: NimBLE-Arduino 1.4.x (arduino-esp32 core 2.x, this project) and
// 2.x (core 3.x, canary-sense) differ on init()/setAdvertisementData() return
// types and isInitialized(); the ESP_ARDUINO_VERSION_MAJOR split mirrors
// firmware/projects/canary-display/src/net/chirp_scan.cpp.

#include "canary/net/fleet_beacon_adv.h"

#if defined(FEATURE_FLEET_BEACON) && FEATURE_FLEET_BEACON

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <fleet_beacon.h>              // canonical wire format (common/fleet_link)
#include <string>
#include <cstring>

#include "canary/config.h"
#include "canary/runtime_config.h"     // canary::cfg::get().device_id (GAP name)
#include "canary/net/fleet_beacon_payload.h"
#include "canary/log.h"

namespace canary::net {

namespace {

constexpr uint32_t REFRESH_MS = 5000;   // rebuild the manufacturer data cadence

bool     s_inited   = false;   // NimBLE up + advert started
bool     s_failed   = false;   // bring-up failed — no-op for the rest of this boot
uint32_t s_next_ms  = 0;
uint32_t s_last_gen = 0;       // payload generation this carrier last put on air

// Put the current payload on the BLE radio.
void publish_adv(uint32_t now) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;

  uint8_t mfg[FLEET_BEACON_MFG_V2_LEN];
  const size_t n = fleet_beacon_payload_build(mfg);

  NimBLEAdvertisementData advData;
  advData.setManufacturerData(std::string((const char*)mfg, n));

  // stop -> set -> start refreshes the on-air payload deterministically on BOTH
  // NimBLE majors: 1.4.x's setAdvertisementData only latches for the next
  // start(), while 2.x updates live. setAdvertisementData is called as a bare
  // statement so the return-type drift (void on 1.4.x, bool on 2.x) is
  // irrelevant. Advertise-only, so nothing else to preserve across the restart.
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();

  s_last_gen = fleet_beacon_payload_generation();
  s_next_ms = now + REFRESH_MS;
}

}  // namespace

void fleet_beacon_begin(uint32_t now) {
  if (s_inited || s_failed) return;

  // Bring NimBLE up exactly once, under the device id as the GAP name (matches
  // the mDNS advert). init() returns void on 1.4.x and bool on 2.x — call it as
  // a statement and confirm via isInitialized() on 2.x (1.4.x lacks it, so
  // there a non-null getAdvertising() is the signal) — same guard style as
  // chirp_scan.cpp's ble_up().
  NimBLEDevice::init(canary::cfg::get().device_id);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::isInitialized()) {
    s_failed = true;
    canary::log_line("BEACON", "NimBLE init failed — presence beacon off this boot.");
    return;
  }
#endif
  if (!NimBLEDevice::getAdvertising()) {
    s_failed = true;
    canary::log_line("BEACON", "No advertiser handle — presence beacon off this boot.");
    return;
  }

  s_inited = true;
  publish_adv(now);
  canary::log_line("BEACON",
                   "Fleet-link presence beacon advertising (BLE, broker-free).");
}

void fleet_beacon_tick(uint32_t now) {
  if (!s_inited) return;
  // A detection edge bumped the generation — put it on air immediately rather
  // than waiting out the refresh. Every carrier watches the same counter, so
  // one edge reaches every band without this module knowing the others exist.
  if (fleet_beacon_payload_generation() != s_last_gen) {
    publish_adv(now);
    return;
  }
  if ((int32_t)(now - s_next_ms) < 0) return;
  publish_adv(now);
}

}  // namespace canary::net

#else  // FEATURE_FLEET_BEACON off — no-op stubs (per-board size-guard off switch)

#include <stdint.h>
namespace canary::net {
void fleet_beacon_begin(uint32_t) {}
void fleet_beacon_tick(uint32_t) {}
}  // namespace canary::net

#endif  // FEATURE_FLEET_BEACON
