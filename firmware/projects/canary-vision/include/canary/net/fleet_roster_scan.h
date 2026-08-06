#pragma once
#include <stdint.h>

// Fleet-link BLE roster scanner — the RX twin of the advertise-only presence
// beacon (fleet_beacon_adv.h).
//
// Where fleet_beacon_adv makes THIS witness findable, this module tracks the
// OTHER Canaries: a low-duty passive BLE scan parses each nearby fleet-link
// presence beacon (fleet_beacon.h, type 0x10) — and any 17-byte Chirp — and
// feeds the shared, pure fleet roster (firmware/common/fleet_link/
// fleet_roster.h). It answers "who else is in my fleet, and when did I last
// hear each of them" (last-heartbeat + the latest battery/health/chain/flags
// the beacon carried), so every Canary — not just the display — keeps a live
// view of its siblings.
//
// UNSIGNED presence, exactly like the beacon it consumes: liveness and
// self-reported status, never a verified trust claim.
//
// Gated by FEATURE_FLEET_ROSTER (canary/config.h, default 1). When 0 these are
// no-ops so CI's OTA-slot size guard can veto the scanner per board without
// touching any call site — the same off-switch pattern as FEATURE_FLEET_BEACON.

namespace canary::net {

// Drain whatever the last scan captured into the roster, age stale peers out,
// and manage the passive scan itself. Cheap to call every loop() pass; rate-
// limited internally (a short burst on a slow cadence). Lazily brings NimBLE
// up on first use and degrades to a no-op if it can't.
//
// Both radio-state flags matter, and the SECOND one is load-bearing:
//   wifi_up          the STA link is associated right now.
//   wifi_provisioned this unit has real credentials, so wifi_loop() will go on
//                    retrying the join for as long as it takes.
//
// Continuous scanning is only correct when there is no join to protect. A
// provisioned unit that is merely between attempts stays BURSTY: BLE and WiFi
// share one 2.4 GHz radio on the C3/C6, and a never-ending passive scan
// during the retry window starves the very association it is waiting for —
// a device that drops off WiFi then can't get back on. Only an unprovisioned
// unit (nothing to join, the beacon genuinely is the last channel) scans
// continuously.
void fleet_roster_scan_tick(uint32_t now, bool wifi_up, bool wifi_provisioned);

// Live peer count (Canaries heard within the roster expiry window). 0 until the
// first sighting. Cheap — for a serial/status readout of "N in the fleet".
int fleet_roster_scan_peer_count();

// Total fleet-link adverts ingested since boot (beacons + chirps) — a coarse
// "is the listener hearing anything" counter for diagnostics.
uint32_t fleet_roster_scan_seen();

} // namespace canary::net
