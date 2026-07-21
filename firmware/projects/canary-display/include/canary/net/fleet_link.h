#pragma once
#include <stdint.h>

// Fleet link — Layer 3 of the direct, broker-free BLE path (trailblazer
// spec §6 extension). Where chirp_scan.cpp passively LISTENS to a WAP's
// presence beacon (Layer 1/2), fleet_link is the on-demand PULL: a NimBLE
// central that CONNECTS to a nearby WAP's GATT status service and reads its
// richer, live self-report (full chain seq, health, degrade, SD%, mic-muted,
// battery) with NO MQTT broker and NO home WiFi.
//
// Gated by FEATURE_FLEET_LINK (BLE central is heavier than passive scan —
// flash, heap, and 2.4 GHz coexistence). Everything here is UNSIGNED and
// coarse like a chirp/beacon: it feeds liveness + diagnostics, NEVER trust
// (the badge is never set from a fleet-link read).
//
// The reads are fed into the fleet model via on_beacon(fp4, ...) — the same
// non-trust ingest the passive beacon uses — so a known witness is updated in
// place and an unknown one appears as a "SCV-XXXX" pseudo witness.

namespace canary::net {

// Drive the (non-blocking-ish) state machine. Runs only while broker_down; the
// heap gate and an s_ble_failed latch keep it from thrashing a thin/failing
// radio. wifi_up is accepted for symmetry with chirp_scan and to bias the
// coexistence decision (BLE central contends with WiFi for the radio).
void fleet_link_loop(uint32_t now, bool broker_down, bool wifi_up);

// Ask fleet_link to pull the status of the WAP whose fingerprint suffix is
// fp4 ("abcd", 4 lowercase hex) — e.g. from a display tap on that witness.
// The actual connect happens on the next fleet_link_loop that finds the
// target advertising and the conditions safe.
void fleet_link_request(const char* fp4);

// Diagnostics: successful GATT status pulls since boot.
uint32_t fleet_link_count();

}  // namespace canary::net
