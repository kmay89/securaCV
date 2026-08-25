#pragma once
#include <stdint.h>

#include "canary/detect_profiles.h"

// Runtime detection configuration — NVS-backed with compiled-in defaults.
//
// Why this exists: the loaded SSCMA model decides what class index means
// "person" and how its confidence scores are calibrated. Models are swapped
// on the Grove Vision AI V2 module via SenseCraft (no host reflash), so the
// host firmware's detection semantics must be adjustable at runtime too —
// otherwise every model change needs a rebuild. These settings are exposed
// as Home Assistant number entities (see ha_discovery.cpp) and persist
// across reboots and OTA installs.
//
// The compiled constants in config.h (PERSON_TARGET, SCORE_MIN,
// LOST_TIMEOUT_MS, DWELL_START_MS) seed NVS on first boot only.

namespace canary::cfg {

struct DetectConfig {
  uint8_t  person_target;    // SSCMA class index reported as the subject
  uint8_t  score_min;        // 0–100 confidence threshold
  uint32_t lost_timeout_ms;  // silence before "subject left"
  uint32_t dwell_start_ms;   // sustained presence before "dwelling"
  uint8_t  profile;          // watch profile id (index into WATCH_PROFILES)
};

// Bounds (shared by the setters and the HA number entities)
constexpr uint8_t  DETECT_SCORE_MIN_LO   = 0;
constexpr uint8_t  DETECT_SCORE_MIN_HI   = 100;
constexpr uint32_t DETECT_LOST_MS_LO     = 250;
constexpr uint32_t DETECT_LOST_MS_HI     = 60000;
constexpr uint32_t DETECT_DWELL_MS_LO    = 1000;
constexpr uint32_t DETECT_DWELL_MS_HI    = 600000;

// Loaded once on first call (then cached). Safe from setup() onward.
const DetectConfig& detect();

// Setters clamp to the bounds above, persist to NVS, and update the cached
// struct (the FSM and vision manager read it every tick, so changes apply
// live). Return true if the stored value changed.
bool detect_set_person_target(uint8_t v);
bool detect_set_score_min(uint8_t v);
bool detect_set_lost_timeout_ms(uint32_t v);
bool detect_set_dwell_start_ms(uint32_t v);

// Select a watch profile (detect_profiles.h). Persists the profile id AND
// applies the profile's preset to the four settings above — selecting (or
// re-selecting) a profile is the one-step "tune this box for that job"
// action; each setting stays individually adjustable afterward. An unknown
// id is rejected untouched (returns false) so junk on the wire can never
// reset tuning. Returns true if the profile id or any setting changed.
bool detect_set_profile(uint8_t v);

} // namespace canary::cfg
