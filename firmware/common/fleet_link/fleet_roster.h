/*
 * SecuraCV — Fleet Roster
 *
 * A Canary's own view of the OTHER Canaries it has heard over the air: the
 * device-side twin of what the display keeps. The radio layer (a BLE scan
 * callback) parses each nearby advert — the fleet-link presence beacon
 * (fleet_beacon.h, type 0x10) or a chirp — and calls fleet_roster_note();
 * this module keeps the table and ages stale peers out. It answers "who else
 * is in my fleet, and when did I last hear each of them" — last-heartbeat,
 * plus the latest battery/health/chain/flags the beacon carried.
 *
 * PURE: no Arduino / NimBLE / heap — just a fixed-size POD table operated on
 * by free functions, so it round-trips in a plain g++ host test
 * (tests_host / test_fleet_roster.cpp). Each firmware declares ONE
 * `static FleetRoster` and feeds it from its scan callback.
 *
 * Trust posture: like the beacon and the display's chirp/mDNS ingest, this is
 * UNSIGNED presence — it tracks liveness and self-reported status, never a
 * verified trust claim.
 */
#ifndef FLEET_ROSTER_H
#define FLEET_ROSTER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Max peers tracked. Small on purpose — a household fleet, not a census.
#ifndef FLEET_ROSTER_MAX
#define FLEET_ROSTER_MAX 16
#endif

// Drop a peer this long after its last heartbeat (matches the WAP ble_nearby
// NEARBY_EXPIRY_SEC = 120 s and the beacon/chirp cadence).
#ifndef FLEET_ROSTER_EXPIRY_MS
#define FLEET_ROSTER_EXPIRY_MS 120000UL
#endif

// One tracked peer. battery_pct / health_pct are -1 when unknown (the beacon's
// 0xFF sentinel, or a chirp that carried no status).
typedef struct {
  char     fp4[5];        // peer fingerprint suffix, 4 lowercase hex + NUL — the key
  uint32_t last_seen_ms;  // millis of the most recent sighting — the "last chirp"
  uint8_t  last_type;     // last beacon/chirp type byte
  int16_t  battery_pct;   // -1 = unknown
  int16_t  health_pct;    // -1 = unknown
  uint16_t chain_lo;      // low 16 bits of the peer's chain height
  uint8_t  flags;         // beacon flag bits (tamper/mic-muted/degraded/on-wifi/alert)
  int8_t   rssi;          // dBm; 0 = unknown
  uint8_t  used;          // slot occupied
} FleetRosterEntry;

typedef struct {
  FleetRosterEntry entries[FLEET_ROSTER_MAX];
} FleetRoster;

static inline void fleet_roster_init(FleetRoster* r) {
  if (r) memset(r, 0, sizeof(*r));
}

// Record a sighting of peer fp4 (4 hex chars). Upserts by fp4: updates
// last_seen + type + rssi + chain + flags, and overwrites battery/health only
// when this sighting actually carried them (pass -1 to leave the stored value
// untouched — a status-less chirp doesn't wipe the last known battery). On a
// full table, evicts the stalest entry. Returns the slot index, or -1 on bad
// input. Pure — `now` is the caller's millis clock.
static inline int fleet_roster_note(FleetRoster* r, const char fp4[5],
                                    uint8_t type, int battery_pct, int health_pct,
                                    uint16_t chain_lo, uint8_t flags,
                                    int8_t rssi, uint32_t now) {
  if (!r || !fp4 || fp4[0] == '\0') return -1;

  int slot = -1, free_slot = -1, oldest = -1;
  uint32_t oldest_age = 0;
  for (int i = 0; i < FLEET_ROSTER_MAX; i++) {
    if (!r->entries[i].used) { if (free_slot < 0) free_slot = i; continue; }
    if (strncmp(r->entries[i].fp4, fp4, 4) == 0) { slot = i; break; }
    const uint32_t age = now - r->entries[i].last_seen_ms;  // wrap-safe unsigned
    if (oldest < 0 || age > oldest_age) { oldest = i; oldest_age = age; }
  }
  if (slot < 0) slot = (free_slot >= 0) ? free_slot : oldest;
  if (slot < 0) return -1;

  FleetRosterEntry* e = &r->entries[slot];
  if (!e->used || strncmp(e->fp4, fp4, 4) != 0) {
    // New (or evicting) entry: start clean so a stale peer's status can't leak.
    memset(e, 0, sizeof(*e));
    e->fp4[0] = fp4[0]; e->fp4[1] = fp4[1];
    e->fp4[2] = fp4[2]; e->fp4[3] = fp4[3]; e->fp4[4] = '\0';
    e->battery_pct = -1;
    e->health_pct  = -1;
    e->used = 1;
  }
  e->last_seen_ms = now;
  e->last_type    = type;
  e->chain_lo     = chain_lo;
  e->flags        = flags;
  e->rssi         = rssi;
  if (battery_pct >= 0) e->battery_pct = (int16_t)battery_pct;
  if (health_pct  >= 0) e->health_pct  = (int16_t)health_pct;
  return slot;
}

// Age out peers not heard within FLEET_ROSTER_EXPIRY_MS. Call periodically.
static inline void fleet_roster_expire(FleetRoster* r, uint32_t now) {
  if (!r) return;
  for (int i = 0; i < FLEET_ROSTER_MAX; i++) {
    if (!r->entries[i].used) continue;
    if ((uint32_t)(now - r->entries[i].last_seen_ms) > (uint32_t)FLEET_ROSTER_EXPIRY_MS) {
      r->entries[i].used = 0;
    }
  }
}

// Live peer count (occupied slots). Call fleet_roster_expire() first for a
// freshly-aged count.
static inline int fleet_roster_count(const FleetRoster* r) {
  if (!r) return 0;
  int n = 0;
  for (int i = 0; i < FLEET_ROSTER_MAX; i++) if (r->entries[i].used) n++;
  return n;
}

// Find a peer by fp4; returns its entry or NULL.
static inline const FleetRosterEntry* fleet_roster_find(const FleetRoster* r,
                                                        const char fp4[5]) {
  if (!r || !fp4) return NULL;
  for (int i = 0; i < FLEET_ROSTER_MAX; i++) {
    if (r->entries[i].used && strncmp(r->entries[i].fp4, fp4, 4) == 0) {
      return &r->entries[i];
    }
  }
  return NULL;
}

#endif // FLEET_ROSTER_H
