#pragma once
#include <stdint.h>
#include <string.h>

// Watch profiles — per-use-case presets for the runtime detection config.
//
// One firmware, several jobs: the same Grove Vision AI V2 witness watches a
// living room for people or a litter box for the cat, and the difference is
// never a rebuild — it is which model SenseCraft loaded on the module plus
// the tuning below. Selecting a profile (HA "Watch profile" select →
// securacv/<id>/cfg/profile/set) applies that profile's preset to the four
// NVS-backed detection settings in one step; each of them stays individually
// tunable afterward, and re-selecting the profile restores its preset.
//
// A profile changes TUNING and LABELS only. The event vocabulary
// (presence_started / dwell_started / dwell_ended / presence_ended /
// interaction_likely), the signed witness envelope, and the free-signals
// claim set are identical across profiles — a litter-box visit is the same
// presence→dwell→interaction_likely shape a room walk-through is, read
// against a different subject (Invariant VI: no silent vocabulary growth).
//
// Pure hosted C++ (no Arduino): compiled by the ESP32 build, the browser
// emulator core, and firmware/tests_host/test_vision_profiles.cpp.

namespace canary::cfg {

struct WatchProfilePreset {
  const char* key;    // stable machine key ([a-z_]) — NVS/MQTT/JSON surfaces
  const char* label;  // HA select option label — what the user picks
  const char* subject;  // what the box is watching for (boot banner, logs)

  // Preset seeds for the four runtime detection settings (detect_config.h).
  uint8_t  target;           // SSCMA class index (model-dependent; tunable)
  uint8_t  score_min;        // 0–100 confidence threshold
  uint32_t lost_timeout_ms;  // silence before "subject left"
  uint32_t dwell_start_ms;   // sustained presence before "dwelling"

  // Fleet beacon detect-class token advertised while presence holds. Values
  // mirror FLEET_BEACON_DETECT_* in common/fleet_link/fleet_beacon.h — the
  // ObjectClass vocabulary and nothing beyond it; the cross-check lives in
  // firmware/tests_host/test_vision_profiles.cpp.
  uint8_t beacon_class;
};

// Index in this table == the persisted profile id (NVS det_profile) — append
// only, never reorder.
//
// room_presence — the classic optical witness: a person entered, lingered,
// interacted. Preset equals the compiled first-boot seeds in config.h.
//
// litter_box — a cat-detection model watching a litter box in an already-lit
// space (no low-light heroics needed; aim across the box, not into a lamp):
//   score 60   animal models calibrate lower than person models, and a cat
//              mid-dig is a strange shape — 70 misses real visits;
//   lost 4000  digging and turning drop frames constantly — a short timeout
//              fragments one visit into five;
//   dwell 8000 a real visit means staying in the box, not a walk-by sniff —
//              8 s separates "used it" from "checked it out".
inline constexpr WatchProfilePreset WATCH_PROFILES[] = {
    {"room_presence", "Room presence", "person", 0, 70, 1500, 10000,
     /*beacon_class=*/0x01},  // FLEET_BEACON_DETECT_PERSON
    {"litter_box", "Litter box", "cat", 0, 60, 4000, 8000,
     /*beacon_class=*/0x03},  // FLEET_BEACON_DETECT_ANIMAL
};

inline constexpr uint8_t WATCH_PROFILE_COUNT =
    (uint8_t)(sizeof(WATCH_PROFILES) / sizeof(WATCH_PROFILES[0]));

// Out-of-range ids (an NVS blob written by a newer firmware, junk on the
// wire) resolve to the default profile rather than reading past the table.
inline const WatchProfilePreset& watch_profile(uint8_t id) {
  return WATCH_PROFILES[(id < WATCH_PROFILE_COUNT) ? id : 0];
}

// Match an inbound profile string — the HA select sends the option label,
// scripts and the SPA send the machine key; accept both, exactly. Returns
// the profile id, or -1 when nothing matches (caller drops the command; a
// mangled payload must never reset tuning).
inline int watch_profile_from_text(const char* s) {
  if (!s) return -1;
  for (uint8_t i = 0; i < WATCH_PROFILE_COUNT; ++i) {
    if (strcmp(s, WATCH_PROFILES[i].key) == 0 ||
        strcmp(s, WATCH_PROFILES[i].label) == 0) {
      return (int)i;
    }
  }
  return -1;
}

}  // namespace canary::cfg
