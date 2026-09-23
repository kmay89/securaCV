/*
 * SecuraCV Canary — BLE Scout proximity pairing window (pure logic)
 *
 * The one way a beacon becomes "paired" (repo sweep F27, option B): the
 * owner arms a short window with a label, holds the tag against the Canary,
 * and the FIRST advert from an unpaired beacon at or above the RSSI
 * threshold inside the window is paired. The raw MAC never crosses an API:
 * the window only ever sees an RSSI and a clock, and the pairing itself
 * happens inside ble_scout_on_advert() (the NimBLE scan callback), which
 * hands the MAC straight to ble_scout_pair() — hashed there, discarded.
 * What the HTTP surface reads back is the 16-byte hashed_id (a per-device
 * keyed hash, unlinkable across devices — ble_scan.h) and the label.
 *
 * This header is the deterministic half: no Arduino, no NimBLE, no locks.
 * ble_scout.cpp owns the one Window instance and serializes every access
 * behind its portMUX (the window is armed by the HTTP task, offered to by
 * the NimBLE host task, and expired by the loop task).
 *
 * Threat note (documented, bounded): the first single qualifying advert
 * wins, with no debounce and no strongest-wins rule, and the caller sees no
 * advert payload. So at the default -45 dBm the owner's own phone (in hand
 * after pressing Pair), a neighboring Canary's fleet-link advert on the
 * same shelf, or a phone held within a few centimeters by someone else
 * could pair instead of the tag. The window is short (<= 60 s), the
 * threshold floor is clamped (never looser than -70 dBm), the owner chose
 * the label, and unpair is one tap. Already-paired beacons never consume
 * the window. A multi-advert confirmation and a fleet-link/Chirp payload
 * filter wait on bench data (U1).
 *
 * Host test: firmware/canary/lib/securacv_ble_scan/test_ble_scout_pairing.cpp
 * (the firmware.yml "Mesh + Scout Host Tests" job).
 */

#ifndef SECURACV_BLE_SCOUT_PAIRING_H
#define SECURACV_BLE_SCOUT_PAIRING_H

#include "ble_scan.h"   /* HASHED_ID_LEN, MAX_LABEL_LEN */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace ble_scout {
namespace pairing {

constexpr uint32_t MAX_WINDOW_MS     = 60000;  /* API contract: window_s <= 60 */
constexpr uint32_t MIN_WINDOW_MS     = 5000;   /* shorter is not a usable gesture */
constexpr uint32_t DEFAULT_WINDOW_MS = 60000;
constexpr int8_t   DEFAULT_RSSI_MIN  = -45;    /* "held against the device" */
constexpr int8_t   RSSI_MIN_FLOOR    = -70;    /* never looser: not "anything in the house" */
constexpr int8_t   RSSI_MIN_CEIL     = -20;    /* tighter than this nothing ever pairs */

enum class State : uint8_t {
  IDLE     = 0,  /* never armed since boot */
  ARMED    = 1,  /* waiting for a strong advert */
  CLAIMED  = 2,  /* an advert won the window; ble_scout_pair() in flight */
  PAIRED   = 3,  /* done: paired_id holds the new beacon's hashed_id */
  FAILED   = 4,  /* an advert won but the registry refused (full / no key) */
  EXPIRED  = 5,  /* the window ran out with no qualifying advert */
  CANCELED = 6,  /* the owner canceled while armed */
};

enum class ArmResult : uint8_t {
  OK            = 0,
  BUSY          = 1,  /* a window is already armed (or claimed) */
  BAD_LABEL     = 2,  /* empty, too long, or not printable ASCII */
  /* The two below come from ble_scout_pair_window_start(), which knows the
   * registry and the key; arm() itself never returns them. */
  REGISTRY_FULL = 3,  /* every slot is in use: unpair one first */
  NOT_READY     = 4,  /* the Scout has not initialized (no key yet) */
};

struct Window {
  State    state;
  uint32_t armed_at_ms;
  uint32_t window_ms;
  int8_t   rssi_min;
  char     label[ble_scan::MAX_LABEL_LEN + 1];
  uint8_t  paired_id[ble_scan::HASHED_ID_LEN];
};

/* A copy for the HTTP task — never a pointer into the live window. */
struct Status {
  State    state;
  uint32_t remaining_ms;   /* 0 unless ARMED */
  uint32_t window_ms;
  int8_t   rssi_min;
  char     label[ble_scan::MAX_LABEL_LEN + 1];
  uint8_t  paired_id[ble_scan::HASHED_ID_LEN];  /* meaningful only when PAIRED */
};

inline void init(Window* w) {
  if (w == nullptr) return;
  memset(w, 0, sizeof(*w));
  w->state    = State::IDLE;
  w->rssi_min = DEFAULT_RSSI_MIN;
}

/* Label rule for the API: 1..MAX_LABEL_LEN bytes, printable ASCII
 * (0x20..0x7E, the chokepoint's rule), not all spaces. Refused rather than
 * rewritten: registry_add would turn "Küche" into "K??che" silently. */
inline bool label_ok(const char* label) {
  if (label == nullptr) return false;
  const size_t n = strlen(label);
  if (n == 0 || n > ble_scan::MAX_LABEL_LEN) return false;
  bool any_visible = false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)label[i];
    if (c < 0x20 || c > 0x7E) return false;
    if (c != 0x20) any_visible = true;
  }
  return any_visible;
}

inline uint32_t clamp_window_ms(uint32_t ms) {
  if (ms == 0) return DEFAULT_WINDOW_MS;
  if (ms < MIN_WINDOW_MS) return MIN_WINDOW_MS;
  if (ms > MAX_WINDOW_MS) return MAX_WINDOW_MS;
  return ms;
}

inline int8_t clamp_rssi_min(int rssi_min) {
  if (rssi_min < RSSI_MIN_FLOOR) return RSSI_MIN_FLOOR;
  if (rssi_min > RSSI_MIN_CEIL)  return RSSI_MIN_CEIL;
  return (int8_t)rssi_min;
}

/* Wrap-safe: true once `now` is window_ms or more past armed_at. */
inline bool elapsed(const Window* w, uint32_t now_ms) {
  return (uint32_t)(now_ms - w->armed_at_ms) >= w->window_ms;
}

/* Arm a new window. `window_ms` and `rssi_min` are clamped (0 means the
 * default window). A window that is still ARMED refuses (BUSY) until it
 * expires or is canceled; a CLAIMED one refuses until finish(). */
inline ArmResult arm(Window* w, const char* label, uint32_t window_ms,
                     int rssi_min, uint32_t now_ms) {
  if (w == nullptr) return ArmResult::BUSY;
  if (w->state == State::CLAIMED) return ArmResult::BUSY;
  if (w->state == State::ARMED && !elapsed(w, now_ms)) return ArmResult::BUSY;
  if (!label_ok(label)) return ArmResult::BAD_LABEL;
  w->state       = State::ARMED;
  w->armed_at_ms = now_ms;
  w->window_ms   = clamp_window_ms(window_ms);
  w->rssi_min    = clamp_rssi_min(rssi_min);
  memset(w->label, 0, sizeof(w->label));
  memcpy(w->label, label, strlen(label));
  memset(w->paired_id, 0, sizeof(w->paired_id));
  return ArmResult::OK;
}

/* Offer one advert from an UNPAIRED beacon (the caller filters out
 * already-paired ones). Returns true exactly once per window: the first
 * advert at/above rssi_min while armed claims it (ARMED -> CLAIMED); the
 * caller then pairs and reports back with finish(). An offer after the
 * window ran out marks it EXPIRED and returns false. */
inline bool offer(Window* w, int8_t rssi_dbm, uint32_t now_ms) {
  if (w == nullptr || w->state != State::ARMED) return false;
  if (elapsed(w, now_ms)) {
    w->state = State::EXPIRED;
    return false;
  }
  if (rssi_dbm < w->rssi_min) return false;
  w->state = State::CLAIMED;
  return true;
}

/* Record the outcome of the claimed pairing. Ignored unless CLAIMED. */
inline void finish(Window* w, bool paired,
                   const uint8_t hashed_id[ble_scan::HASHED_ID_LEN]) {
  if (w == nullptr || w->state != State::CLAIMED) return;
  if (paired && hashed_id != nullptr) {
    memcpy(w->paired_id, hashed_id, ble_scan::HASHED_ID_LEN);
    w->state = State::PAIRED;
  } else {
    w->state = State::FAILED;
  }
}

/* Loop-task tick: ARMED past its window -> EXPIRED. Returns true on that
 * transition. */
inline bool expire(Window* w, uint32_t now_ms) {
  if (w == nullptr || w->state != State::ARMED) return false;
  if (!elapsed(w, now_ms)) return false;
  w->state = State::EXPIRED;
  return true;
}

/* Owner cancel. Only an ARMED window cancels; a CLAIMED one is already
 * pairing (microseconds) and finishes as PAIRED/FAILED. Returns true when
 * it canceled. */
inline bool cancel(Window* w, uint32_t now_ms) {
  if (w == nullptr || w->state != State::ARMED) return false;
  if (elapsed(w, now_ms)) {
    w->state = State::EXPIRED;
    return false;
  }
  w->state = State::CANCELED;
  return true;
}

inline Status status(const Window* w, uint32_t now_ms) {
  Status s;
  memset(&s, 0, sizeof(s));
  if (w == nullptr) return s;
  s.state     = w->state;
  s.window_ms = w->window_ms;
  s.rssi_min  = w->rssi_min;
  memcpy(s.label, w->label, sizeof(s.label));
  s.label[sizeof(s.label) - 1] = '\0';
  if (w->state == State::ARMED) {
    if (elapsed(w, now_ms)) {
      s.state = State::EXPIRED;   /* report what expire() is about to record */
    } else {
      s.remaining_ms = w->window_ms - (uint32_t)(now_ms - w->armed_at_ms);
    }
  }
  if (w->state == State::PAIRED) {
    memcpy(s.paired_id, w->paired_id, sizeof(s.paired_id));
  }
  return s;
}

inline const char* state_name(State s) {
  switch (s) {
    case State::IDLE:     return "idle";
    case State::ARMED:    return "armed";
    case State::CLAIMED:  return "pairing";
    case State::PAIRED:   return "paired";
    case State::FAILED:   return "failed";
    case State::EXPIRED:  return "expired";
    case State::CANCELED: return "canceled";
  }
  return "idle";
}

/* hashed_id <-> 32 lowercase hex chars, the only spelling the API uses. */
inline void id_to_hex(const uint8_t id[ble_scan::HASHED_ID_LEN],
                      char out[2 * ble_scan::HASHED_ID_LEN + 1]) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < ble_scan::HASHED_ID_LEN; ++i) {
    out[2 * i]     = kHex[(id[i] >> 4) & 0xF];
    out[2 * i + 1] = kHex[id[i] & 0xF];
  }
  out[2 * ble_scan::HASHED_ID_LEN] = '\0';
}

/* Strict: exactly 32 hex digits (either case), nothing after. */
inline bool id_from_hex(const char* hex, uint8_t out[ble_scan::HASHED_ID_LEN]) {
  if (hex == nullptr || strlen(hex) != 2 * ble_scan::HASHED_ID_LEN) return false;
  uint8_t tmp[ble_scan::HASHED_ID_LEN];
  for (size_t i = 0; i < 2 * ble_scan::HASHED_ID_LEN; ++i) {
    const char c = hex[i];
    uint8_t v;
    if (c >= '0' && c <= '9')      v = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
    else return false;
    if ((i & 1) == 0) tmp[i / 2] = (uint8_t)(v << 4);
    else              tmp[i / 2] = (uint8_t)(tmp[i / 2] | v);
  }
  memcpy(out, tmp, sizeof(tmp));
  return true;
}

}  /* namespace pairing */
}  /* namespace ble_scout */

#endif  /* SECURACV_BLE_SCOUT_PAIRING_H */
