#pragma once
#include <stdint.h>

// Fleet-link BLE presence beacon — advertise-only (NimBLE).
//
// Broadcasts the canonical 11-byte fleet-link presence beacon
// (firmware/common/fleet_link/fleet_beacon.h) as BLE manufacturer data so a
// canary-display finds this witness DIRECTLY over BLE — no MQTT broker and no
// shared WiFi. Advertise-only: no GATT server, no scan, no service UUIDs
// (these devices have none to preserve, so the beacon is the whole advert).
//
// The whole implementation is gated by FEATURE_FLEET_BEACON (canary/config.h,
// default 1). When the flag is 0 these become no-ops, so CI's OTA-slot size
// guard can veto BLE per board without touching any call site.

namespace canary::net {

// Lazy NimBLE init + first advertisement. Call once, after WiFi STA is up
// (BLE and WiFi-STA coexist on the C3/C6 shared 2.4 GHz radio) and after
// canary::witness::init() so the fingerprint identity is available. Safe when
// the stack can't come up — it degrades to a no-op for this boot rather than
// crashing.
void fleet_beacon_begin(uint32_t now);

// Rebuild the manufacturer data from live state (chain height, degraded /
// on-wifi flags, fingerprint) and refresh the advert. Rate-limited internally
// (~5 s); cheap to call every loop() pass. No-op until begin() brought the
// stack up. Broker-independent — keeps advertising through an MQTT outage.
void fleet_beacon_tick(uint32_t now);

// Feed the live detection state into the advert (v2 beacon: ALERT flag +
// class token + confidence 0..100 — a token and a percentage, nothing
// identifying). Cheap to call every vision tick: a mere score change waits
// for the ~5 s refresh, but a presence EDGE (active flips, or the class
// changes while active) republishes immediately so a display alerts without
// the cadence lag. detect_class is a FLEET_BEACON_DETECT_* token.
void fleet_beacon_note_detection(bool active, uint8_t detect_class,
                                 int score_pct, uint32_t now);

} // namespace canary::net
