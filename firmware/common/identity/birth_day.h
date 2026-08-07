/**
 * @file birth_day.h
 * @brief When a key was born — decided once, never restated.
 *
 * THE PROBLEM THIS SOLVES. A Canary's identity is its Ed25519 keypair: the
 * device id, the AP SSID, the mDNS name and the derived birth certificate all
 * fall out of the pubkey fingerprint. But the app could only ever say when
 * THIS PHONE paired with it, which is a fact about the phone. Two people
 * looking at the same Canary saw two different "born" dates, and a device
 * re-paired after a phone restore looked newborn.
 *
 * WHY THIS IS NOT JUST `time(NULL)` AT KEYGEN. The key is generated before any
 * clock exists. An ESP32 has no battery-backed RTC; it boots at the Unix
 * epoch and only learns the date when NTP answers — which is after Wi-Fi,
 * which is after provisioning, which is after the key. So the birth day is
 * always recorded LATER than the birth, and the only honest question is how
 * much later.
 *
 * THE THREE RULES
 *
 *   1. WRITTEN ONCE. A stored birth day is never overwritten, not by a better
 *      clock, not by a firmware update, not by a factory reset that spares
 *      NVS. This is the whole point: a birth day a device could restate is a
 *      birth day that launders the device's age, and the certificate card
 *      would be quoting the most recent lie. Only losing the key itself —
 *      which makes it a different Canary — clears it.
 *   2. ONLY A BELIEVABLE CLOCK. A clock reading before `kClockFloor` is the
 *      epoch showing through, not a date. Stamping it would brand every
 *      device born in 1970 and burn rule 1 on a garbage value.
 *   3. EXACT ONLY WHEN IT'S EXACT. `exact` is set when the clock arrived
 *      while the key was still fresh — same boot, inside the grace window.
 *      A device that sat on a shelf for a week before it ever saw a network
 *      records the day it was FIRST DATED, and says so; the app must not
 *      call that a birthday. "On or before" is a fact. "Born" would be a
 *      guess wearing a fact's clothes.
 *
 * PRIVACY. A day, not a timestamp — `kSecondsPerDay` granularity is coarser
 * than Invariant III's ten-minute buckets and by construction cannot carry a
 * time-of-day. Nothing here is derived from the network, the MAC, or the
 * owner; it is one integer about the key.
 *
 * Board-agnostic and dependency-free so `firmware/tests_host` can exercise
 * every branch — see test_birth_day.cpp. Callers own NVS; this module owns
 * only the decision.
 */

#ifndef SECURACV_COMMON_BIRTH_DAY_H
#define SECURACV_COMMON_BIRTH_DAY_H

#include <stdint.h>

namespace birth {

/** Days are the unit. A birth day has no time of day, on purpose. */
constexpr uint32_t kSecondsPerDay = 86400u;

/**
 * A wall clock reading below this is the boot epoch, not a date.
 * 2025-01-01T00:00:00Z — comfortably before any Canary existed and
 * comfortably after 1970, which is the only distinction it has to make.
 * It is a floor, not a version: it never needs raising.
 */
constexpr uint32_t kClockFloor = 1735689600u;

/**
 * How long a key may have existed and still have its first dated day called
 * a birthday. Six hours covers "flashed, provisioned, joined Wi-Fi, NTP
 * answered" with room for a slow setup and a coffee, and excludes the shelf.
 */
constexpr uint32_t kGraceSeconds = 6u * 3600u;

/** What NVS holds. `day == 0` means nothing has ever been stamped. */
struct Stamp {
  uint32_t day = 0;      ///< Days since the Unix epoch.
  bool     exact = false;///< See rule 3: false means "first dated", not "born".

  bool recorded() const { return day != 0; }
};

/** What the device can see at the moment it asks. */
struct Observation {
  uint32_t unix_s = 0;        ///< What the clock says now.
  uint32_t key_age_s = 0;     ///< Seconds since the key was generated…
  bool     key_age_known = false; ///< …meaningful only when the key was made
                                  ///< this boot. A key that predates this boot
                                  ///< has an age nobody kept, and pretending
                                  ///< otherwise is how a shelf becomes a
                                  ///< birthday.
};

/** Days since the epoch for a wall-clock reading. */
inline uint32_t day_of(uint32_t unix_s) { return unix_s / kSecondsPerDay; }

/** Midnight UTC of a stamped day, for callers that must render a date. */
inline uint32_t unix_of_day(uint32_t day) { return day * kSecondsPerDay; }

/** Rule 2: is this a date, or is it the epoch showing through? */
inline bool clock_is_believable(uint32_t unix_s) { return unix_s >= kClockFloor; }

/**
 * The whole decision. Returns true and fills `out` when the caller should
 * write a new stamp to NVS; returns false when it must not — which is the
 * common case, because a device stamps once in its life and then boots
 * thousands of times without touching it.
 */
inline bool consider(const Stamp& stored, const Observation& now, Stamp* out) {
  if (!out) return false;
  if (stored.recorded()) return false;             // rule 1
  if (!clock_is_believable(now.unix_s)) return false;  // rule 2
  out->day = day_of(now.unix_s);
  out->exact = now.key_age_known && now.key_age_s <= kGraceSeconds;  // rule 3
  return out->day != 0;
}

}  // namespace birth

#endif  // SECURACV_COMMON_BIRTH_DAY_H
