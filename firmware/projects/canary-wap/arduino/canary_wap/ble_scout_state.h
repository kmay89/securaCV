/*
 * SecuraCV Canary — BLE Scout per-beacon presence state machine
 * Version 0.1.0
 *
 * PR 5b. Drives the "this paired beacon is in / out of this Scout's
 * room" decision from a noisy RSSI stream. Pure-logic; the NimBLE
 * scan callback (ble_scout_nimble.cpp) just feeds raw (hashed_id,
 * rssi, now_ms) into this state machine.
 *
 * Per-beacon lifecycle:
 *
 *   AWAY  ──[ smoothed ≥ ARRIVE_THRESHOLD ]──►  PROVISIONAL
 *
 *   PROVISIONAL ──[ smoothed < ARRIVE_THRESHOLD ]──► AWAY
 *
 *   PROVISIONAL ──[ held ≥ DWELL_MS ]──►          PRESENT  + emit "arrived"
 *
 *   PRESENT ──[ no advert for ≥ LOST_MS ]──►      AWAY     + emit "departed"
 *
 * Why a dwell + lost timer instead of a raw threshold:
 *   - A phone briefly walking past the doorway flashes one strong
 *     packet then drops. DWELL_MS rejects that.
 *   - A real handoff produces a 5-second stable spike. DWELL_MS
 *     accepts it.
 *   - LOST_MS avoids "departed" flapping when the phone goes silent
 *     for a few seconds (BT-LE adv interval can be 1-2s).
 *
 * 10-dB hysteresis between arrive/depart is implicit in the
 * arrive-only threshold + "no advert for LOST_MS" detector — we
 * don't need a separate depart threshold because a stalled RSSI
 * means the beacon stopped advertising or is now well below.
 *
 * All times are uint32_t millis() and compared signed-wrap-safe:
 *   `(int32_t)(now - deadline) >= 0`
 * so this works across the millis() wrap at ~49 days.
 */

#ifndef SECURACV_BLE_SCOUT_STATE_H
#define SECURACV_BLE_SCOUT_STATE_H

#include "ble_scan.h"   /* HASHED_ID_LEN, MAX_PAIRED_BEACONS, KalmanRssi */

#include <stdint.h>
#include <stdbool.h>

namespace ble_scout {

/* Thresholds — tuned for indoor handhelds (phones, watches, tags). */
constexpr int8_t   ARRIVE_THRESHOLD_DBM = -75;   /* "close enough to be in this room" */
constexpr uint32_t DWELL_MS             = 5000;  /* must hold ≥ threshold this long */
constexpr uint32_t LOST_MS              = 30000; /* no adverts this long ⇒ departed */

enum class PresenceState : uint8_t {
  AWAY        = 0,
  PROVISIONAL = 1,
  PRESENT     = 2,
};

enum class PresenceEvent : uint8_t {
  NONE     = 0,
  ARRIVED  = 1,
  DEPARTED = 2,
};

struct PresenceSlot {
  uint8_t                   hashed_id[ble_scan::HASHED_ID_LEN];
  ble_scan::KalmanRssi      rssi_filter;
  PresenceState             state;
  uint32_t                  provisional_since_ms;  /* when smoothed first crossed ARRIVE */
  uint32_t                  last_seen_ms;          /* last advert timestamp */
  bool                      in_use;
};

struct PresenceTracker {
  PresenceSlot slots[ble_scan::MAX_PAIRED_BEACONS];
};

/* Zero-initialize the tracker. */
void presence_init(PresenceTracker* t);

/* Drop a beacon's presence state (use when the user un-pairs the
 * beacon). Idempotent — no-op if absent. */
void presence_forget(PresenceTracker* t,
                     const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]);

/* Feed one advert observation. Returns ARRIVED on the AWAY/PROVISIONAL
 * → PRESENT transition, DEPARTED if we time-detect a lost beacon
 * mid-call (rare; usually caught by on_tick), otherwise NONE.
 *
 * `now_ms` is the caller's monotonic timestamp (Arduino millis() on
 * device; an injected test clock on host). */
PresenceEvent presence_on_advert(PresenceTracker* t,
                                 const uint8_t  hashed_id[ble_scan::HASHED_ID_LEN],
                                 int8_t         raw_rssi_dbm,
                                 uint32_t       now_ms);

/* Walk all slots and emit DEPARTED for any beacon whose
 * (now_ms - last_seen_ms) ≥ LOST_MS. Should be called periodically
 * (e.g. once per CSI feature window — 1 Hz). Returns the number of
 * slots that fired DEPARTED, and writes their hashed_ids into
 * `out_departed_ids` (caller-supplied buffer of N×HASHED_ID_LEN bytes).
 * Pass `nullptr` to discard the ids; the count return is still valid.
 *
 * `max_departed` caps how many ids we'll write — the function never
 * over-runs the caller buffer. Remaining departed beacons are still
 * transitioned to AWAY; only the report is truncated. */
size_t presence_on_tick(PresenceTracker* t,
                        uint32_t         now_ms,
                        uint8_t*         out_departed_ids,
                        size_t           max_departed);

/* Diagnostics: lookup current state for a hashed_id. Returns AWAY if
 * the slot has never been seen. */
PresenceState presence_query(const PresenceTracker* t,
                             const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]);

}  /* namespace ble_scout */

#endif /* SECURACV_BLE_SCOUT_STATE_H */
