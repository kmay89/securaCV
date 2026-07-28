#pragma once
// Runtime radar reflexes — the NVS-backed twin of canary-vision's
// detect_config. The CS_* compile-time values in configs/canary-sense/*
// seed the very first boot; after that these eleven numbers live in NVS,
// are tunable from Home Assistant (number entities over cfg/*/set) AND
// from the USB serial tuning console (common/console/tuning_console.h),
// and can be pre-seeded by the browser flasher at install time. Same
// clamping posture as detect_config: every setter bounds its input, so a
// mangled payload can never latch a nonsense reflex.

#include <stdint.h>

namespace canary::cfg {

// Bounds — the same ranges the Sense Lab bench sliders teach (senselab.html),
// parsed into the flasher catalog so all three surfaces agree by construction.
inline constexpr uint32_t SENSE_DEBOUNCE_MS_LO = 0;
inline constexpr uint32_t SENSE_DEBOUNCE_MS_HI = 3000;
inline constexpr uint32_t SENSE_CLEAR_MS_LO    = 200;
inline constexpr uint32_t SENSE_CLEAR_MS_HI    = 10000;
inline constexpr uint32_t SENSE_STALL_MS_LO    = 1000;
inline constexpr uint32_t SENSE_STALL_MS_HI    = 20000;
inline constexpr uint32_t SENSE_NEAR_CM_LO     = 50;
inline constexpr uint32_t SENSE_NEAR_CM_HI     = 400;
inline constexpr uint32_t SENSE_MID_CM_LO      = 100;
inline constexpr uint32_t SENSE_MID_CM_HI      = 600;
inline constexpr uint32_t SENSE_VLOCK_MS_LO    = 500;
inline constexpr uint32_t SENSE_VLOCK_MS_HI    = 15000;
inline constexpr uint32_t SENSE_VLOST_MS_LO    = 1000;
inline constexpr uint32_t SENSE_VLOST_MS_HI    = 20000;
// Vitals plausibility bands (wellbeing): the min ranges sit strictly below
// the max ranges, so min < max holds by construction — no cross-field
// validation needed anywhere a value can enter.
inline constexpr uint32_t SENSE_BREATH_MIN_LO  = 3;
inline constexpr uint32_t SENSE_BREATH_MIN_HI  = 15;
inline constexpr uint32_t SENSE_BREATH_MAX_LO  = 16;
inline constexpr uint32_t SENSE_BREATH_MAX_HI  = 60;
inline constexpr uint32_t SENSE_HEART_MIN_LO   = 30;
inline constexpr uint32_t SENSE_HEART_MIN_HI   = 79;
inline constexpr uint32_t SENSE_HEART_MAX_LO   = 80;
inline constexpr uint32_t SENSE_HEART_MAX_HI   = 220;

struct SenseConfig {
  uint32_t present_debounce_ms;  // sustained target before "present"
  uint32_t clear_timeout_ms;     // no target before "clear"
  uint32_t stall_timeout_ms;     // no UART frame at all before "unknown"
  uint32_t near_cm;              // coarse band edge: <= near
  uint32_t mid_cm;               // coarse band edge: <= mid, else far
  uint32_t vitals_lock_ms;       // wellbeing: sustained vitals before "locked"
  uint32_t vitals_lost_ms;       // wellbeing: no vitals before "lost"
  uint32_t breath_min_bpm;       // wellbeing: plausible breathing band (reject noise)
  uint32_t breath_max_bpm;
  uint32_t heart_min_bpm;        // wellbeing: plausible heart band
  uint32_t heart_max_bpm;
};

// Lazy-loaded singleton (NVS on first touch; compiled defaults as the seed).
const SenseConfig& sense();

// Each returns true when the value CHANGED (after clamping); persistence is
// best-effort — a full NVS still applies the value in RAM for this boot.
bool sense_set_present_debounce_ms(uint32_t v);
bool sense_set_clear_timeout_ms(uint32_t v);
bool sense_set_stall_timeout_ms(uint32_t v);
bool sense_set_near_cm(uint32_t v);
bool sense_set_mid_cm(uint32_t v);
bool sense_set_vitals_lock_ms(uint32_t v);
bool sense_set_vitals_lost_ms(uint32_t v);
bool sense_set_breath_min_bpm(uint32_t v);
bool sense_set_breath_max_bpm(uint32_t v);
bool sense_set_heart_min_bpm(uint32_t v);
bool sense_set_heart_max_bpm(uint32_t v);

} // namespace canary::cfg
