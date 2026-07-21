// Host-side test for fleet_roster.h (firmware/common/fleet_link) — a Canary's
// own view of the OTHER Canaries it hears over the air (the device-side twin of
// the display's fleet model). Pure POD table, no Arduino/NimBLE: the test
// includes the header directly and exercises note()/upsert/evict/expire/count/
// find plus the "-1 keeps last known status" rule.
//
// Single source of truth: this header is included via -I by the canary / vision
// / sense firmwares (no per-firmware copies — nothing to sync-guard).

#include "fleet_roster.h"

#include <cstdio>
#include <cstdint>

static int g_failures = 0;
#define CHECK(cond, msg)                                            \
  do {                                                             \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; }   \
    else         { printf("ok:   %s\n", (msg)); }                 \
  } while (0)

int main() {
  // ── Init leaves an empty roster ──────────────────────────────────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    CHECK(fleet_roster_count(&r) == 0, "fresh roster is empty");
    CHECK(fleet_roster_find(&r, "abcd") == NULL, "find on empty returns NULL");
  }

  // ── note() inserts, carries status, and find() returns it ────────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    // 0x10 = the presence beacon type byte (fleet_beacon.h FLEET_BEACON_TYPE);
    // the roster is type-agnostic, so it's just an opaque byte here.
    int slot = fleet_roster_note(&r, "1a2b", /*type=*/0x10,
                                 /*battery=*/76, /*health=*/88,
                                 /*chain_lo=*/0x1234, /*flags=*/0x05,
                                 /*rssi=*/-60, /*now=*/1000);
    CHECK(slot >= 0, "note() returns a valid slot");
    CHECK(fleet_roster_count(&r) == 1, "one peer after first note");

    const FleetRosterEntry* e = fleet_roster_find(&r, "1a2b");
    CHECK(e != NULL, "find() locates the noted peer");
    CHECK(e->last_seen_ms == 1000, "last_seen recorded");
    CHECK(e->last_type == 0x10, "type recorded");
    CHECK(e->battery_pct == 76, "battery recorded");
    CHECK(e->health_pct == 88, "health recorded");
    CHECK(e->chain_lo == 0x1234, "chain_lo recorded");
    CHECK(e->flags == 0x05, "flags recorded");
    CHECK(e->rssi == -60, "rssi recorded");
  }

  // ── note() upserts by fp4 (no duplicate slot) ────────────────────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    fleet_roster_note(&r, "beef", 0x10, 50, 50, 1, 0, -55, 1000);
    fleet_roster_note(&r, "beef", 0x10, 55, 60, 2, 0, -50, 5000);
    CHECK(fleet_roster_count(&r) == 1, "second sighting of same fp4 upserts (no dup)");

    const FleetRosterEntry* e = fleet_roster_find(&r, "beef");
    CHECK(e->last_seen_ms == 5000, "upsert refreshes last_seen");
    CHECK(e->battery_pct == 55, "upsert updates battery");
    CHECK(e->chain_lo == 2, "upsert updates chain");
  }

  // ── -1 battery/health leaves the last known value untouched ──────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    fleet_roster_note(&r, "cafe", 0x10, /*battery=*/90, /*health=*/95,
                      0, 0, -40, 1000);
    // A status-less chirp: pass -1 for both — must not wipe the stored status.
    fleet_roster_note(&r, "cafe", 0x01, /*battery=*/-1, /*health=*/-1,
                      3, 0x02, -42, 2000);
    const FleetRosterEntry* e = fleet_roster_find(&r, "cafe");
    CHECK(e->battery_pct == 90, "-1 battery keeps last known");
    CHECK(e->health_pct == 95, "-1 health keeps last known");
    CHECK(e->last_seen_ms == 2000, "but last_seen still advances");
    CHECK(e->last_type == 0x01, "and type/chain/flags still update");
    CHECK(e->chain_lo == 3, "chain updated from status-less sighting");
  }

  // ── expire() ages out only peers past the expiry window ──────────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    fleet_roster_note(&r, "old0", 0x10, 50, 50, 0, 0, -50, 1000);
    fleet_roster_note(&r, "new0", 0x10, 50, 50, 0, 0, -50, 100000);
    CHECK(fleet_roster_count(&r) == 2, "two peers before expiry");

    // now = 1000 + EXPIRY + 1 -> "old0" is stale, "new0" is fresh.
    fleet_roster_expire(&r, 1000 + FLEET_ROSTER_EXPIRY_MS + 1);
    CHECK(fleet_roster_count(&r) == 1, "stale peer aged out");
    CHECK(fleet_roster_find(&r, "old0") == NULL, "the stale one is gone");
    CHECK(fleet_roster_find(&r, "new0") != NULL, "the fresh one remains");
  }

  // ── a full table evicts the stalest peer for a new one ───────────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    char fp[5] = "aa00";
    // Fill every slot; give each a distinct, increasing last_seen so slot 0
    // ("aa00" @ t=100) is the stalest.
    for (int i = 0; i < FLEET_ROSTER_MAX; i++) {
      fp[2] = 'a' + (i / 10);
      fp[3] = '0' + (i % 10);
      fleet_roster_note(&r, fp, 0x10, 50, 50, 0, 0, -50,
                        /*now=*/100 + (uint32_t)i * 10);
    }
    CHECK(fleet_roster_count(&r) == FLEET_ROSTER_MAX, "table is full");

    // One more distinct peer at the newest time -> evicts the stalest (slot 0).
    fleet_roster_note(&r, "zzzz", 0x10, 50, 50, 0, 0, -50, /*now=*/999999);
    CHECK(fleet_roster_count(&r) == FLEET_ROSTER_MAX, "count stays capped");
    CHECK(fleet_roster_find(&r, "aa00") == NULL, "stalest peer was evicted");
    CHECK(fleet_roster_find(&r, "zzzz") != NULL, "new peer took its slot");
  }

  // ── bad input is rejected, and doesn't corrupt the table ─────────────
  {
    FleetRoster r;
    fleet_roster_init(&r);
    CHECK(fleet_roster_note(NULL, "abcd", 0x10, 0, 0, 0, 0, 0, 1) == -1,
          "note(NULL roster) returns -1");
    CHECK(fleet_roster_note(&r, NULL, 0x10, 0, 0, 0, 0, 0, 1) == -1,
          "note(NULL fp4) returns -1");
    char empty[5] = {0};
    CHECK(fleet_roster_note(&r, empty, 0x10, 0, 0, 0, 0, 0, 1) == -1,
          "note(empty fp4) returns -1");
    CHECK(fleet_roster_count(&r) == 0, "rejected notes left the table empty");
  }

  printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
