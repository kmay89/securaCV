/**
 * @file test_power_logic.cpp
 * @brief Host-build regression tests for the battery decision logic.
 *
 * Pins the behaviors both firmware trees rely on, in particular the
 * false-state fixes from PR #762:
 *
 *   - charging detection outranks the low/critical SoC bands (a dead
 *     battery on USB must report CHARGING, or the policy engine
 *     deep-sleeps a device that is plugged in and charging);
 *   - estimates report 0 ("unknown") instead of guessing.
 *
 * Build (mirrors the Mesh + Scout Host Tests CI job):
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/common/power/test_power_logic.cpp \
 *       -I firmware/common/power \
 *       -o /tmp/test_power_logic && /tmp/test_power_logic
 */

#include "power_logic.h"

#include <cassert>
#include <cstdio>

using namespace power_logic;

namespace {

void test_soc_endpoints_and_table_points() {
  /* At/above the table top is pinned to 100, at/below the floor to 0. */
  assert(voltage_to_soc(4500) == 100);
  assert(voltage_to_soc(4200) == 100);
  assert(voltage_to_soc(3000) == 0);
  assert(voltage_to_soc(2500) == 0);
  /* Exact table entries reproduce their listed SoC. */
  assert(voltage_to_soc(4150) == 95);
  assert(voltage_to_soc(3870) == 50);
  assert(voltage_to_soc(3700) == 15);
  assert(voltage_to_soc(3500) == 5);
  std::printf("PASS test_soc_endpoints_and_table_points\n");
}

void test_soc_interpolation_is_monotonic() {
  /* SoC must never increase as voltage falls — a non-monotonic curve
   * would let the policy engine flap between modes on a steady drain. */
  uint8_t prev = 100;
  for (uint16_t mv = 4250; mv >= 2950; mv--) {
    uint8_t soc = voltage_to_soc(mv);
    assert(soc <= prev);
    prev = soc;
  }
  /* Midpoint of the 3870->3920 segment (50% -> 60%) lands mid-band. */
  uint8_t mid = voltage_to_soc(3895);
  assert(mid == 55);
  std::printf("PASS test_soc_interpolation_is_monotonic\n");
}

void test_presence_window() {
  assert(!battery_voltage_in_range(2799));   /* dead cell / open pad */
  assert(battery_voltage_in_range(2800));
  assert(battery_voltage_in_range(3700));
  assert(battery_voltage_in_range(4400));
  assert(!battery_voltage_in_range(4401));   /* VBUS bleed, no cell */
  std::printf("PASS test_presence_window\n");
}

void test_charging_outranks_critical() {
  /* THE PR #762 FIX: a depleted battery (3% SoC region) with a rising
   * voltage trend is on USB and charging — it must classify CHARGING,
   * never CRITICAL, or the device deep-sleeps while plugged in. */
  assert(classify_hw_charge_state(3350, 3, 10, 15, 5) == CS_CHARGING);
  assert(classify_hw_charge_state(3350, 3,  3, 15, 5) == CS_CHARGING);
  /* Same voltage, falling trend: genuinely dying — CRITICAL. */
  assert(classify_hw_charge_state(3350, 3, -2, 15, 5) == CS_CRITICAL);
  /* Low band with rising trend is also charging. */
  assert(classify_hw_charge_state(3680, 12, 5, 15, 5) == CS_CHARGING);
  std::printf("PASS test_charging_outranks_critical\n");
}

void test_charge_state_bands() {
  /* Full: at/above 4150 mV with a flat trend (-1..+1). */
  assert(classify_hw_charge_state(4180, 97,  0, 15, 5) == CS_FULL);
  assert(classify_hw_charge_state(4150, 95,  1, 15, 5) == CS_FULL);
  assert(classify_hw_charge_state(4150, 95, -1, 15, 5) == CS_FULL);
  /* At full voltage but still rising fast: tail of the charge. */
  assert(classify_hw_charge_state(4150, 95,  4, 15, 5) == CS_CHARGING);
  /* Trend threshold is strictly > 2 mV/min. */
  assert(classify_hw_charge_state(3900, 55,  2, 15, 5) == CS_DISCHARGING);
  assert(classify_hw_charge_state(3900, 55,  3, 15, 5) == CS_CHARGING);
  /* SoC bands at falling/flat trend. */
  assert(classify_hw_charge_state(3700, 15, -2, 15, 5) == CS_LOW);
  assert(classify_hw_charge_state(3500,  5, -2, 15, 5) == CS_CRITICAL);
  assert(classify_hw_charge_state(3900, 55, -2, 15, 5) == CS_DISCHARGING);
  /* Configurable thresholds are honored. */
  assert(classify_hw_charge_state(3700, 15, -2, 10, 5) == CS_DISCHARGING);
  std::printf("PASS test_charge_state_bands\n");
}

void test_health_cycle_fade() {
  assert(health_pct_from_cycles(0, true) == 100);
  assert(health_pct_from_cycles(125, true) == 95);
  assert(health_pct_from_cycles(250, true) == 90);
  assert(health_pct_from_cycles(500, true) == 80);   /* rated life: -20% */
  assert(health_pct_from_cycles(1000, true) == 60);  /* clamp floor */
  assert(health_pct_from_cycles(100000, true) == 60);
  /* No battery: nothing to degrade, never a false "worn out". */
  assert(health_pct_from_cycles(100000, false) == 100);
  std::printf("PASS test_health_cycle_fade\n");
}

void test_runtime_estimate_refuses_to_guess() {
  /* Unknown cases all report 0, never a fabricated number. */
  assert(estimate_runtime_min(false, CS_DISCHARGING, -2, 3900) == 0);
  assert(estimate_runtime_min(true,  CS_CHARGING,    -2, 3900) == 0);
  assert(estimate_runtime_min(true,  CS_FULL,        -2, 4180) == 0);
  assert(estimate_runtime_min(true,  CS_UNKNOWN,     -2, 3900) == 0);
  assert(estimate_runtime_min(true,  CS_DISCHARGING,  0, 3900) == 0);
  assert(estimate_runtime_min(true,  CS_DISCHARGING,  2, 3900) == 0);
  assert(estimate_runtime_min(true,  CS_DISCHARGING, -2, 3300) == 0);
  assert(estimate_runtime_min(true,  CS_DISCHARGING, -2, 3100) == 0);
  std::printf("PASS test_runtime_estimate_refuses_to_guess\n");
}

void test_runtime_estimate_math() {
  /* 600 mV of headroom at -2 mV/min = 300 minutes. */
  assert(estimate_runtime_min(true, CS_DISCHARGING, -2, 3900) == 300);
  /* LOW and CRITICAL states still estimate. */
  assert(estimate_runtime_min(true, CS_LOW,      -2, 3700) == 200);
  assert(estimate_runtime_min(true, CS_CRITICAL, -1, 3500) == 200);
  /* Slowest measurable slope over the full LiPo window: still under
   * the cap, reported as-is. */
  assert(estimate_runtime_min(true, CS_DISCHARGING, -1, 4200) == 900);
  assert(estimate_runtime_min(true, CS_DISCHARGING, -1, 4400) == 1100);
  /* The 14-day cap is a clamp against absurd inputs (a corrupted
   * voltage reading), not a reachable state for in-range LiPo values. */
  assert(estimate_runtime_min(true, CS_DISCHARGING, -1, 65535) ==
         RUNTIME_CAP_MIN);
  std::printf("PASS test_runtime_estimate_math\n");
}

}  // namespace

int main() {
  test_soc_endpoints_and_table_points();
  test_soc_interpolation_is_monotonic();
  test_presence_window();
  test_charging_outranks_critical();
  test_charge_state_bands();
  test_health_cycle_fade();
  test_runtime_estimate_refuses_to_guess();
  test_runtime_estimate_math();
  std::printf("ALL POWER LOGIC TESTS PASSED\n");
  return 0;
}
