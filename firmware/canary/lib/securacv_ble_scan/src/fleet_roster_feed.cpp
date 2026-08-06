/*
 * fleet_roster_feed.cpp — parse fleet-link adverts into the shared roster.
 *
 * See fleet_roster_feed.h. Pure: includes only the header-only wire format
 * (fleet_beacon.h) and roster (fleet_roster.h) from common/fleet_link — no
 * NimBLE/Arduino. Compiled only in the `full` env (FEATURE_BLE_SCAN), which is
 * where the Scout scan that feeds it exists; the `release` size guard is
 * therefore untouched.
 */

/* FEATURE_BLE_SCAN comes from platformio.ini build_flags in the canary PIO
 * build and from build_config.h in canary-wap — include the latter when
 * present so the gate matches the Scout scan TU (ble_scout_nimble.cpp). */
#if defined(__has_include)
  #if __has_include("build_config.h")
    #include "build_config.h"
  #endif
#endif

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN

#include "fleet_roster_feed.h"

#include <fleet_beacon.h>   // presence-beacon wire format (common/fleet_link)
#include <fleet_roster.h>   // shared pure roster           (common/fleet_link)

namespace fleet_roster_feed {

namespace {

FleetRoster s_roster;
bool        s_inited = false;
uint32_t    s_seen   = 0;

void ensure_inited() {
  if (!s_inited) {
    fleet_roster_init(&s_roster);
    s_inited = true;
  }
}

void fp4_from_bytes(uint8_t b0, uint8_t b1, char out[5]) {
  static const char H[] = "0123456789abcdef";
  out[0] = H[(b0 >> 4) & 0xF];
  out[1] = H[b0 & 0xF];
  out[2] = H[(b1 >> 4) & 0xF];
  out[3] = H[b1 & 0xF];
  out[4] = '\0';
}

}  // namespace

void on_advert(const uint8_t* mfg, size_t len, int8_t rssi, uint32_t now_ms) {
  if (!mfg) return;
  ensure_inited();

  // Fleet-link presence beacon (11-byte v1 / 13-byte v2 mfg, type 0x10) —
  // full status. v2 adds detection class + confidence (canary-vision);
  // fleet_beacon_parse accepts both, and the flags byte (incl. ALERT) rides
  // into the roster either way.
  if (len == FLEET_BEACON_MFG_LEN || len == FLEET_BEACON_MFG_V2_LEN) {
    FleetBeaconFields f;
    if (!fleet_beacon_parse(mfg, len, &f)) return;
    char fp4[5];
    fp4_from_bytes(f.fp_b0, f.fp_b1, fp4);
    fleet_roster_note(&s_roster, fp4, FLEET_BEACON_TYPE, f.battery_pct,
                      f.health_pct, f.chain_lo16, f.flags, rssi, now_ms);
    s_seen++;
    return;
  }

  // Chirp (17-byte mfg, company 0xFFFF LE, type 0x01..0x05) — presence + type
  // only; fp2 rides at [15-16], the same 2 bytes the beacon carries. Pass -1
  // for battery/health so the roster keeps the last known status.
  if (len == 17) {
    if (mfg[0] != 0xFF || mfg[1] != 0xFF) return;
    const uint8_t type = mfg[2];
    if (type < 0x01 || type > 0x05) return;
    char fp4[5];
    fp4_from_bytes(mfg[15], mfg[16], fp4);
    fleet_roster_note(&s_roster, fp4, type, /*battery=*/-1, /*health=*/-1,
                      /*chain_lo=*/0, /*flags=*/0, rssi, now_ms);
    s_seen++;
    return;
  }
}

void tick(uint32_t now_ms) {
  if (!s_inited) return;
  fleet_roster_expire(&s_roster, now_ms);
}

int peer_count() {
  return s_inited ? fleet_roster_count(&s_roster) : 0;
}

uint32_t seen() { return s_seen; }

// Single-task by design, so the copy needs no lock: the writer (on_advert, via
// the NimBLE scan callback), tick(), peer_count(), and this snapshot all run on
// the MAIN LOOP — the same invariant ble_scout.cpp relies on for its plain
// broadcast-callback store ("emit_arrived/departed run from ble_scout_tick /
// on_advert — both on the main loop", ble_scout.cpp). There is no concurrent
// writer to tear an entry mid-copy. If the roster is ever fed from a separate
// task, this AND the existing readers (peer_count/tick) would need a shared
// critical section together — a subsystem change, not a per-caller lock.
int snapshot(FleetRosterEntry* out, int max) {
  if (!s_inited || !out || max <= 0) return 0;
  int n = 0;
  for (int i = 0; i < FLEET_ROSTER_MAX && n < max; ++i) {
    if (s_roster.entries[i].used) out[n++] = s_roster.entries[i];
  }
  return n;
}

}  // namespace fleet_roster_feed

#endif  // FEATURE_BLE_SCAN
