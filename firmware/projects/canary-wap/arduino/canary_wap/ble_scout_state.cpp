/*
 * SecuraCV Canary — BLE Scout presence state machine — Implementation
 *
 * Pure logic; no NimBLE, no Preferences, no Arduino. Compiles
 * identically on host and device.
 */

#include "ble_scout_state.h"

#include <string.h>

namespace ble_scout {

namespace {

/* Find slot by hashed_id, or nullptr. */
PresenceSlot* find_slot(PresenceTracker* t,
                        const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  if (t == nullptr || hashed_id == nullptr) return nullptr;
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    if (t->slots[i].in_use &&
        ble_scan::hashed_id_equal(t->slots[i].hashed_id, hashed_id)) {
      return &t->slots[i];
    }
  }
  return nullptr;
}

const PresenceSlot* find_slot_c(const PresenceTracker* t,
                                const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  if (t == nullptr || hashed_id == nullptr) return nullptr;
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    if (t->slots[i].in_use &&
        ble_scan::hashed_id_equal(t->slots[i].hashed_id, hashed_id)) {
      return &t->slots[i];
    }
  }
  return nullptr;
}

/* Claim a free slot and initialize it for `hashed_id`. Returns nullptr
 * if the tracker is full (which mirrors the registry's bound — should
 * never happen in practice because callers only feed paired beacons). */
PresenceSlot* claim_slot(PresenceTracker* t,
                         const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    if (!t->slots[i].in_use) {
      PresenceSlot* s = &t->slots[i];
      memcpy(s->hashed_id, hashed_id, ble_scan::HASHED_ID_LEN);
      ble_scan::kalman_reset(&s->rssi_filter, 0.0f, 0.0f);
      s->state                = PresenceState::AWAY;
      s->provisional_since_ms = 0;
      s->last_seen_ms         = 0;
      s->in_use               = true;
      return s;
    }
  }
  return nullptr;
}

/* Signed-wrap-safe "now ≥ deadline" check. */
inline bool deadline_reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

}  /* namespace */

void presence_init(PresenceTracker* t) {
  if (t == nullptr) return;
  memset(t, 0, sizeof(*t));
}

void presence_forget(PresenceTracker* t,
                     const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  PresenceSlot* s = find_slot(t, hashed_id);
  if (s == nullptr) return;
  memset(s, 0, sizeof(*s));
}

PresenceEvent presence_on_advert(PresenceTracker* t,
                                 const uint8_t  hashed_id[ble_scan::HASHED_ID_LEN],
                                 int8_t         raw_rssi_dbm,
                                 uint32_t       now_ms) {
  if (t == nullptr || hashed_id == nullptr) return PresenceEvent::NONE;

  PresenceSlot* s = find_slot(t, hashed_id);
  if (s == nullptr) {
    s = claim_slot(t, hashed_id);
    if (s == nullptr) return PresenceEvent::NONE;   /* tracker full */
  }

  /* Update the smoothed RSSI. First sample primes the filter without
   * dragging the estimate from a default 0; see kalman_update. */
  const float smoothed = ble_scan::kalman_update(&s->rssi_filter,
                                                 (float)raw_rssi_dbm);
  s->last_seen_ms = now_ms;

  PresenceEvent emit = PresenceEvent::NONE;

  switch (s->state) {
    case PresenceState::AWAY:
      if (smoothed >= (float)ARRIVE_THRESHOLD_DBM) {
        s->state                = PresenceState::PROVISIONAL;
        s->provisional_since_ms = now_ms;
      }
      break;

    case PresenceState::PROVISIONAL:
      if (smoothed < (float)ARRIVE_THRESHOLD_DBM) {
        /* Fell back below before DWELL — drop the candidacy. */
        s->state                = PresenceState::AWAY;
        s->provisional_since_ms = 0;
      } else if (deadline_reached(now_ms, s->provisional_since_ms + DWELL_MS)) {
        s->state                = PresenceState::PRESENT;
        emit                    = PresenceEvent::ARRIVED;
      }
      break;

    case PresenceState::PRESENT:
      /* No transition on stronger samples; only the LOST_MS timeout
       * (via presence_on_tick) demotes us back to AWAY. */
      break;
  }
  return emit;
}

size_t presence_on_tick(PresenceTracker* t,
                        uint32_t         now_ms,
                        uint8_t*         out_departed_ids,
                        size_t           max_departed) {
  if (t == nullptr) return 0;

  size_t n_emitted = 0;
  size_t n_written = 0;

  for (size_t i = 0; i < ble_scan::MAX_PAIRED_BEACONS; ++i) {
    PresenceSlot* s = &t->slots[i];
    if (!s->in_use) continue;
    if (s->state != PresenceState::PRESENT) continue;
    if (!deadline_reached(now_ms, s->last_seen_ms + LOST_MS)) continue;

    /* Demote to AWAY and emit DEPARTED. The Kalman filter is left
     * primed so the next advert lands smoothly; only the state +
     * timer reset. */
    s->state                = PresenceState::AWAY;
    s->provisional_since_ms = 0;
    n_emitted++;

    if (out_departed_ids != nullptr && n_written < max_departed) {
      memcpy(out_departed_ids + n_written * ble_scan::HASHED_ID_LEN,
             s->hashed_id, ble_scan::HASHED_ID_LEN);
      n_written++;
    }
  }
  return n_emitted;
}

PresenceState presence_query(const PresenceTracker* t,
                             const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  const PresenceSlot* s = find_slot_c(t, hashed_id);
  return (s == nullptr) ? PresenceState::AWAY : s->state;
}

}  /* namespace ble_scout */
