/*
 * SecuraCV — Battery decision logic (pure, host-testable)
 *
 * The arithmetic core of the battery monitor: SoC lookup, charge-state
 * classification, presence range check, cycle-fade health estimate, and
 * the trend-based runtime estimate. No Arduino/ESP-IDF dependencies, no
 * state — every function is a pure function of its arguments, so the
 * host test suite (test_power_logic.cpp, run in CI) can pin the exact
 * behaviors the firmware relies on:
 *
 *   - charging detection outranks the low/critical SoC bands, so a
 *     depleted battery on USB reports CHARGING and the policy engine
 *     never deep-sleeps a device that is plugged in and charging;
 *   - estimates report "unknown" (0) instead of guessing.
 *
 * Consumed by both firmware trees:
 *   - firmware/canary/lib/securacv_power/src/securacv_power.cpp
 *     (ACTIVE, includes via -I firmware/common as "power/power_logic.h")
 *   - firmware/projects/canary-wap/arduino/canary_wap/power_monitor.h
 *     (COMPATIBILITY, committed sketch-local copy; CI's
 *     check_power_sync.sh guards against drift)
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_POWER_LOGIC_H
#define SECURACV_POWER_LOGIC_H

#include <stddef.h>
#include <stdint.h>

namespace power_logic {

/* Charge-state codes. Numerically identical to charge_state_t
 * (firmware/canary) and ChargeState (canary-wap), so call sites cast
 * directly instead of maintaining a mapping table. */
enum ChargeStateCode : uint8_t {
  CS_UNKNOWN     = 0,
  CS_CHARGING    = 1,
  CS_FULL        = 2,
  CS_DISCHARGING = 3,
  CS_LOW         = 4,
  CS_CRITICAL    = 5,
  CS_NO_BATTERY  = 6,
};

/* Single-cell LiPo plausibility window for the presence check. Below
 * the floor the "battery" is a disconnected pad or a destroyed cell;
 * above the ceiling the reading is USB VBUS bleeding through the
 * charger with no cell attached. */
constexpr uint16_t LIPO_VALID_MIN_MV       = 2800;
constexpr uint16_t VBUS_BLEED_THRESHOLD_MV = 4400;

/* Charge-state classification thresholds. */
constexpr uint16_t FULL_DETECT_MV        = 4150;  // at/above + flat trend = full
constexpr int16_t  CHARGE_TREND_MV_MIN   = 2;     // trend above this = charging

/* Runtime estimate bounds. */
constexpr uint16_t RUNTIME_CUTOFF_MV  = 3300;
constexpr uint32_t RUNTIME_CAP_MIN    = 14UL * 24UL * 60UL;  // 14 days

/* Cycle-fade health model: consumer LiPo loses ~20% capacity over
 * ~500 full cycles; below 60% the linear model stops being meaningful
 * and the cell should simply be replaced. */
constexpr uint32_t HEALTH_FADE_PCT_PER_RATED_LIFE = 20;
constexpr uint32_t HEALTH_RATED_CYCLES            = 500;
constexpr uint32_t HEALTH_FADE_CAP_PCT            = 40;

/* LiPo discharge curve: voltage (mV) -> SoC (%), descending. */
struct SocPoint { uint16_t mv; uint8_t pct; };
constexpr SocPoint SOC_TABLE[] = {
  {4200, 100}, {4150,  95}, {4110,  90}, {4080,  85},
  {4020,  80}, {3980,  70}, {3920,  60}, {3870,  50},
  {3830,  40}, {3790,  30}, {3750,  20}, {3700,  15},
  {3630,  10}, {3500,   5}, {3300,   2}, {3000,   0},
};
constexpr size_t SOC_TABLE_LEN = sizeof(SOC_TABLE) / sizeof(SOC_TABLE[0]);

/* Piecewise-linear interpolation over SOC_TABLE. */
inline uint8_t voltage_to_soc(uint16_t mv) {
  if (mv >= SOC_TABLE[0].mv) return 100;
  if (mv <= SOC_TABLE[SOC_TABLE_LEN - 1].mv) return 0;
  for (size_t i = 0; i < SOC_TABLE_LEN - 1; i++) {
    if (mv >= SOC_TABLE[i + 1].mv) {
      uint16_t v_range = SOC_TABLE[i].mv - SOC_TABLE[i + 1].mv;
      uint8_t  p_range = SOC_TABLE[i].pct - SOC_TABLE[i + 1].pct;
      if (v_range == 0) return SOC_TABLE[i].pct;
      uint16_t v_offset = mv - SOC_TABLE[i + 1].mv;
      return SOC_TABLE[i + 1].pct +
             (uint8_t)((uint32_t)v_offset * p_range / v_range);
    }
  }
  return 0;
}

/* True when a reading is plausibly a connected single-cell LiPo. */
inline bool battery_voltage_in_range(uint16_t mv) {
  return mv >= LIPO_VALID_MIN_MV && mv <= VBUS_BLEED_THRESHOLD_MV;
}

/*
 * Classify the charge state from a calibrated voltage reading and its
 * trend (HW ADC mode).
 *
 * ORDERING IS LOAD-BEARING: full/charging detection runs BEFORE the
 * low/critical SoC bands so a depleted battery on USB reports CHARGING
 * (power source USB). Checking SoC first would make the policy engine
 * see CRITICAL + battery power and deep-sleep a device that is plugged
 * in and charging.
 */
inline uint8_t classify_hw_charge_state(uint16_t battery_mv,
                                        uint8_t  soc_pct,
                                        int16_t  trend_mv_per_min,
                                        uint8_t  soc_low_pct,
                                        uint8_t  soc_critical_pct) {
  if (battery_mv >= FULL_DETECT_MV &&
      trend_mv_per_min >= -1 && trend_mv_per_min <= 1) {
    return CS_FULL;
  }
  if (trend_mv_per_min > CHARGE_TREND_MV_MIN) return CS_CHARGING;
  if (soc_pct <= soc_critical_pct)            return CS_CRITICAL;
  if (soc_pct <= soc_low_pct)                 return CS_LOW;
  return CS_DISCHARGING;
}

/* Cycle-fade capacity estimate, percent. 100 with no battery (nothing
 * to degrade); clamped to 60 because below that the model is noise. */
inline uint8_t health_pct_from_cycles(uint32_t charge_cycles,
                                      bool battery_present) {
  if (!battery_present) return 100;
  uint32_t fade =
      (charge_cycles * HEALTH_FADE_PCT_PER_RATED_LIFE) / HEALTH_RATED_CYCLES;
  if (fade > HEALTH_FADE_CAP_PCT) fade = HEALTH_FADE_CAP_PCT;
  return (uint8_t)(100 - fade);
}

/*
 * Minutes until the low-battery cutoff at the measured discharge slope.
 * Coarse ("hours vs. days"); returns 0 for "unknown" rather than
 * guessing: no battery, not in a discharging state, trend flat/rising,
 * or voltage already at/below the cutoff. Capped at 14 days — slower
 * slopes are below measurement resolution.
 */
inline uint32_t estimate_runtime_min(bool     battery_present,
                                     uint8_t  charge_state,
                                     int16_t  trend_mv_per_min,
                                     uint16_t voltage_mv) {
  if (!battery_present) return 0;
  if (charge_state != CS_DISCHARGING &&
      charge_state != CS_LOW &&
      charge_state != CS_CRITICAL) return 0;
  if (trend_mv_per_min >= 0) return 0;
  if (voltage_mv <= RUNTIME_CUTOFF_MV) return 0;
  uint32_t mins = (uint32_t)(voltage_mv - RUNTIME_CUTOFF_MV) /
                  (uint32_t)(-trend_mv_per_min);
  return (mins > RUNTIME_CAP_MIN) ? RUNTIME_CAP_MIN : mins;
}

}  // namespace power_logic

#endif  // SECURACV_POWER_LOGIC_H
