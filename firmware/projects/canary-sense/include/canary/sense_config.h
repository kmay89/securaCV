#pragma once
// Runtime radar reflexes — the NVS-backed twin of canary-vision's
// detect_config. The CS_* compile-time values in configs/canary-sense/*
// seed the very first boot; after that these seven numbers live in NVS,
// are tunable from Home Assistant (number entities over cfg/*/set), and
// can be pre-seeded by the browser flasher at install time. Same clamping
// posture as detect_config: every setter bounds its input, so a mangled
// payload can never latch a nonsense reflex.

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

struct SenseConfig {
  uint32_t present_debounce_ms;  // sustained target before "present"
  uint32_t clear_timeout_ms;     // no target before "clear"
  uint32_t stall_timeout_ms;     // no UART frame at all before "unknown"
  uint32_t near_cm;              // coarse band edge: <= near
  uint32_t mid_cm;               // coarse band edge: <= mid, else far
  uint32_t vitals_lock_ms;       // wellbeing: sustained vitals before "locked"
  uint32_t vitals_lost_ms;       // wellbeing: no vitals before "lost"
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

} // namespace canary::cfg
