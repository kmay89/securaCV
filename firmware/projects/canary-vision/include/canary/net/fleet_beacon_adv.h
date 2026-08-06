#pragma once
#include <stdint.h>

// Fleet-link presence beacon — the BLE carrier (advertise-only, NimBLE).
//
// Broadcasts the canonical fleet-link presence beacon
// (firmware/common/fleet_link/fleet_beacon.h) as BLE manufacturer data so a
// canary-display finds this witness DIRECTLY over BLE — no MQTT broker and no
// shared WiFi. Advertise-only: no GATT server, no scan, no service UUIDs
// (these devices have none to preserve, so the beacon is the whole advert).
//
// This is one of two carriers for the same payload; the other is
// canary/net/fleet_udp.h, which reaches across a house over the WiFi
// the devices are already joined to. What the beacon SAYS lives in
// canary/net/fleet_beacon_payload.h — including fleet_beacon_note_detection(),
// which used to live here — so the bands cannot drift.
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

} // namespace canary::net
