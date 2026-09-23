/*
 * SecuraCV Canary WAP — runtime device-config clamps (host-testable)
 *
 * Arduino-free. The Device tab's "Save Configuration" persists three
 * operator settings; each is clamped here so a bad or hostile value can't
 * push the device outside a safe envelope — most importantly the time
 * bucket, which coarsens event time (Invariant III) and may only ever be
 * made COARSER than the compile-time floor, never finer.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CONFIG_LOGIC_H
#define SECURACV_CONFIG_LOGIC_H

#include <stdint.h>

namespace config_logic {

// The compile-time floor of the event-time bucket: the ten-minute grid
// (Invariant III). canary_wap.ino defines TIME_BUCKET_MS FROM this constant
// (and static_asserts it), test_config_logic.cpp pins its value, and
// regression_check.sh holds the Device tab's configTimeBucket min= to it, so a
// revert to a finer floor fails a gate instead of shipping quietly.
constexpr uint32_t kTimeBucketFloorMs = 600000u;  // ten minutes
static_assert(kTimeBucketFloorMs >= 600000u && kTimeBucketFloorMs % 600000u == 0,
              "time-bucket floor must be a whole multiple of the ten-minute grid "
              "(Invariant III)");

// Event-time coarsening bucket (Invariant III). The user may only widen it
// (more coarsening = more privacy); a request below the compile-time floor is
// raised to the floor. Privacy can therefore only increase, never decrease.
// The value loaded from NVS passes through this clamp at every boot, so one
// persisted under the old 5 s floor runs at the floor from the first boot
// after an in-place upgrade (widen-only). NVS itself keeps the old number
// until a save writes a different value; the boot clamp makes that harmless.
inline uint32_t clamp_time_bucket_ms(uint32_t requested_ms, uint32_t floor_ms) {
  return requested_ms < floor_ms ? floor_ms : requested_ms;
}

// Record emission rate, bounded to a sane range so a 0 (busy-loop) or an
// absurd value can't wedge the record loop.
inline uint32_t clamp_record_interval_ms(uint32_t requested_ms, uint32_t min_ms,
                                         uint32_t max_ms) {
  if (requested_ms < min_ms) return min_ms;
  if (requested_ms > max_ms) return max_ms;
  return requested_ms;
}

// Health-log store threshold. Clamped to [0 (DEBUG) .. max_level]; max_level is
// WARNING (3), so ERROR (4) and CRITICAL (5) are ALWAYS stored regardless of
// the setting — a user can quiet INFO/NOTICE noise but can never silence a
// real fault.
inline uint8_t clamp_log_level(uint32_t requested, uint8_t max_level) {
  return requested > max_level ? max_level : (uint8_t)requested;
}

}  // namespace config_logic

#endif  // SECURACV_CONFIG_LOGIC_H
