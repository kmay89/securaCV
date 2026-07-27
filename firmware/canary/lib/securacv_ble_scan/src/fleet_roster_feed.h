/*
 * SecuraCV Canary — Fleet Roster Feed (modular canary)
 *
 * The fleet-roster consumer of the Scout's existing passive BLE scan. Where
 * the Scout tracks user-paired phones/watches for room presence, this tracks
 * the OTHER Canaries: every advert the Scout's NimBLE callback sees is also
 * offered here (fleet_roster_feed_advert), and a fleet-link presence beacon
 * (fleet_beacon.h, type 0x10) or a 17-byte Chirp is parsed and dropped into
 * the shared, pure fleet roster (firmware/common/fleet_link/fleet_roster.h).
 * So this Canary keeps its own "who's in my fleet / when did I last hear
 * them" view — last-heartbeat plus the latest battery/health/chain/flags —
 * exactly like the display and the vision/sense witnesses.
 *
 * One scan, two consumers: the roster reuses the Scout's radio rather than
 * opening a second scanner (only one NimBLEScan can own the controller).
 *
 * PURE: no NimBLE/Arduino here — the caller (ble_scout_nimble.cpp) hands over
 * the raw manufacturer-data bytes and RSSI, and this module parses + tables
 * them. UNSIGNED presence, like the beacon it consumes: liveness and self-
 * reported status, never a verified trust claim.
 *
 * Compiled only when FEATURE_BLE_SCAN=1 (the `full` env) — the same gate as
 * the Scout scan TU it feeds, so the size-guarded `release` build is untouched.
 */

#ifndef SECURACV_FLEET_ROSTER_FEED_H
#define SECURACV_FLEET_ROSTER_FEED_H

#include <stddef.h>
#include <stdint.h>

#include <fleet_roster.h>  // FleetRosterEntry — for snapshot() (common/fleet_link)

namespace fleet_roster_feed {

// Offer one advert's manufacturer data (the bytes AFTER the AD length/type,
// i.e. starting at the 0xFFFF company id — exactly what NimBLE's
// getManufacturerData() returns). A beacon (11 bytes) or chirp (17 bytes) is
// parsed and noted; anything else is ignored. Cheap; safe to call for every
// advert from the scan callback.
void on_advert(const uint8_t* mfg, size_t len, int8_t rssi, uint32_t now_ms);

// Age stale peers out of the roster. Call periodically from the main loop.
void tick(uint32_t now_ms);

// Live peer count (Canaries heard within the roster expiry window).
int peer_count();

// Total fleet-link adverts ingested since boot (beacons + chirps).
uint32_t seen();

// Copy the live roster's occupied entries into `out` (up to `max`), returning
// the number written (0 before the first advert / when unbuilt). Insertion
// order, not sorted. The caller renders/ages the copy; the roster keeps
// ownership. Used by the read-only `n`/"nearby" console command.
int snapshot(FleetRosterEntry* out, int max);

}  // namespace fleet_roster_feed

#endif  // SECURACV_FLEET_ROSTER_FEED_H
