/*
 * SecuraCV Canary — Mesh Hub failover election wire format + monitor
 * Version 0.1.0
 *
 * PR 4c. When the Hub's heartbeat is absent for 60 s, the remaining
 * sensors deterministically elect a backup Hub: the peer (including
 * self) with the lowest 8-byte fingerprint wins. No voting protocol
 * is needed — every node computes the same winner independently.
 *
 * Wire format (9 bytes total):
 *
 *   offset 0  : event          uint8_t  — 0=hub_absent, 1=hub_elected
 *   offset 1  : fingerprint[8] uint8_t  — the elected Hub's fingerprint
 *
 * HubMonitor: pure state machine tracking the Hub's heartbeat.
 * tick(now_ms) returns true when the Hub has been absent for the
 * configured timeout. Host-testable, no I/O.
 *
 * This TU is pure logic: no Arduino, no mbedtls, no transport.
 */

#ifndef SECURACV_MESH_HUB_ELECTION_H
#define SECURACV_MESH_HUB_ELECTION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_hub_election {

enum class Event : uint8_t {
  HUB_ABSENT  = 0,
  HUB_ELECTED = 1,
};

constexpr size_t  FINGERPRINT_LEN = 8;
constexpr size_t  EVENT_LEN       = 1;
constexpr size_t  PAYLOAD_LEN     = EVENT_LEN + FINGERPRINT_LEN;  /* = 9 */

bool encode(Event           event,
            const uint8_t   fingerprint[FINGERPRINT_LEN],
            uint8_t*        out_buf,
            size_t          out_buf_cap);

bool decode(const uint8_t*  in_buf,
            size_t          in_len,
            Event*          out_event,
            uint8_t         out_fingerprint[FINGERPRINT_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * FINGERPRINT COMPARISON
 *
 * Deterministic ordering for election: memcmp-style, lowest wins.
 * Returns < 0 if a < b, 0 if equal, > 0 if a > b.
 * ────────────────────────────────────────────────────────────────────────── */

int compare_fingerprints(const uint8_t a[FINGERPRINT_LEN],
                         const uint8_t b[FINGERPRINT_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * HUB MONITOR
 *
 * Tracks the Hub's heartbeat. Call on_hub_heartbeat() when a heartbeat
 * from the Hub is received. Call tick() every main-loop pass.
 * tick() returns true exactly once when the Hub has been absent for
 * timeout_ms, then stays latched until reset (new heartbeat or
 * explicit reset_election).
 *
 * Host-testable: no I/O, no timers.
 * ────────────────────────────────────────────────────────────────────────── */

struct HubMonitor {
  uint32_t timeout_ms;
  uint32_t last_hub_heartbeat_ms;
  bool     hub_known;
  bool     absence_fired;
};

HubMonitor make_monitor(uint32_t timeout_ms = 60000);

void on_hub_heartbeat(HubMonitor& m, uint32_t now_ms);

bool tick(HubMonitor& m, uint32_t now_ms);

void reset_election(HubMonitor& m);

}  /* namespace mesh_hub_election */

#endif  /* SECURACV_MESH_HUB_ELECTION_H */
