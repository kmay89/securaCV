// include/canary/glass_settings.h — runtime screen settings ("Quiet Glass"
// settings wave, docs/hardware/display_settings.md).
//
// One versioned blob holds the user's preferences; the per-panel black-point
// calibration lives under its OWN storage key so "reset settings" never
// wipes a floor the user spent a dark room finding (and re-running the
// wizard never touches the preferences).
//
// Writes are debounced: editors mutate RAM and call settings_mark_dirty();
// settings_loop() commits ~2 s after the last change, so a slider drag costs
// one flash write, not hundreds.
#pragma once
#include <stdint.h>

namespace canary::glass {

// "Screen at night" — one decision, not a pile of toggles. Dim keeps the
// calibrated glow; Off darkens the glass and a tap peeks the clock. The
// honesty veto in main.cpp still outranks Off: a dead link or a Warn+
// condition keeps the glow regardless (silence is never rendered as safety).
enum NightScreen : uint8_t {
  NIGHT_SCREEN_DIM = 0,
  NIGHT_SCREEN_OFF = 1,
};

struct Settings {
  uint8_t  day_pct;        // 20..100 — scales day + ambient brightness
  uint8_t  night_screen;   // NightScreen
  uint8_t  red_shift;      // 1 = red-shifted night palette (default)
  uint8_t  peek_s;         // 3 / 5 / 10 — dark-hours tap-to-peek window
  uint8_t  night_start_hh; // 0..23 local
  uint8_t  night_end_hh;   // 0..23 local
  uint16_t night_duty;     // night-profile duty (13-bit), floor-clamped
};

struct NightCal {
  bool     valid;
  uint16_t floor_duty;     // first night-profile duty this panel emits at
};

// The night backlight profile: 1 kHz / 13-bit gives ~30 distinguishable
// steps inside what the 8-bit day profile calls "duty 1" — that resolution
// is what makes a per-panel calibrated floor possible at all. Floors past
// ~5% mean something is wrong (inverted polarity, wrong pin), so the wizard
// warns instead of storing.
constexpr uint16_t NIGHT_DUTY_MAX   = 8191;
constexpr uint16_t NIGHT_FLOOR_CAP  = 410;   // ~5% — beyond this: suspicious
constexpr uint16_t NIGHT_FLOOR_DFLT = 64;    // uncalibrated fallback floor
constexpr int      NIGHT_STEPS      = 10;    // user-facing "level 1..10"

void settings_init();
const Settings& settings();
Settings& settings_mut();          // mutate, then settings_mark_dirty()
void settings_mark_dirty();
void settings_loop(uint32_t now_ms);
void settings_reset();             // defaults; calibration untouched

const NightCal& nightcal();
// Store the calibrated floor. RAM updates either way (tonight works even
// with a broken storage layer); the return says whether it survives a
// reboot — false = refused (suspicious floor) or flash write failed, and
// the wizard tells the truth on the glass.
bool nightcal_put(uint16_t floor_duty);

// Derived day-profile levels (0..255), scaled by day_pct.
uint8_t day_level();
uint8_t ambient_level();

// Effective night glow duty: the stored preference clamped to the panel's
// calibrated floor (or the conservative default floor when uncalibrated).
uint16_t night_duty_effective();

// ── Pure helpers (host-testable; no storage, no Arduino) ─────────────────

// Level 1..NIGHT_STEPS -> duty on a log curve from floor to ~10x floor.
// Log, not linear: at the bottom of a backlight the eye works in ratios.
inline uint16_t night_step_duty(uint16_t floor_duty, int step) {
  if (floor_duty < 1) floor_duty = 1;
  if (step < 1) step = 1;
  if (step > NIGHT_STEPS) step = NIGHT_STEPS;
  // duty = floor * 10^((step-1)/9), integerized: multiply by 1.2915^step-1
  // via a small table to keep it exact and monotone on-device and in tests.
  static const uint16_t kNum[NIGHT_STEPS] = {
      1000, 1292, 1668, 2154, 2783, 3594, 4642, 5995, 7743, 10000};
  uint32_t d = ((uint32_t)floor_duty * kNum[step - 1]) / 1000u;
  if (d > NIGHT_DUTY_MAX) d = NIGHT_DUTY_MAX;
  if (d < floor_duty) d = floor_duty;
  return (uint16_t)d;
}

// Nearest step for a stored duty (inverse of night_step_duty).
inline int night_duty_step(uint16_t floor_duty, uint16_t duty) {
  int best = 1;
  uint32_t best_err = UINT32_MAX;
  for (int i = 1; i <= NIGHT_STEPS; i++) {
    const uint16_t d = night_step_duty(floor_duty, i);
    const uint32_t err = d > duty ? (uint32_t)(d - duty) : (uint32_t)(duty - d);
    if (err < best_err) { best_err = err; best = i; }
  }
  return best;
}

// Quiet-hours window test, midnight-wrap aware. start==end = never night.
inline bool hours_in_window(int hh, int start_hh, int end_hh) {
  if (start_hh == end_hh) return false;
  if (start_hh < end_hh) return hh >= start_hh && hh < end_hh;
  return hh >= start_hh || hh < end_hh;
}

}  // namespace canary::glass
