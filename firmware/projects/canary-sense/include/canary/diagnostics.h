#pragma once
#include <stdint.h>

// Heap health monitor — a lightweight port of the ESP32-S3 canary tree's
// securacv_diagnostics heap monitor + feature-degradation ladder for the
// headless MQTT variants. Sampled at 1 Hz from the main loop; the level is
// published in the status heartbeat (HA diagnostic sensor) and consumed by
// main.cpp to shed load (stretched publish cadence) under memory pressure.
// Identical to canary-vision's diagnostics module.

namespace canary::diag {

// Degradation ladder. Escalation is immediate; de-escalation requires the
// free heap to clear the entry threshold by HEAP_HYSTERESIS (config.h), so
// the level never flaps around a boundary.
enum class Level : uint8_t {
  Normal = 0,
  Warn,        // free < HEAP_WARN_BYTES — report only
  Critical,    // free < HEAP_CRITICAL_BYTES — shed load (2x cadence)
  Emergency,   // free < HEAP_EMERGENCY_BYTES — survival mode (5x cadence)
};

struct Snapshot {
  uint32_t free_heap     = 0;  // bytes free now
  uint32_t min_heap      = 0;  // low-water mark since boot
  uint32_t largest_block = 0;  // biggest single allocation possible
  Level    level         = Level::Normal;
};

// Call every loop() pass; samples at 1 Hz internally.
void loop(uint32_t now_ms);

// Latest snapshot (valid from the first loop() call onward).
const Snapshot& get();

// Stable lowercase name for MQTT payloads ("normal" / "warn" / ...).
const char* level_name(Level level);

// Multiplier main.cpp applies to its publish cadence at the current level:
// 1 (Normal/Warn), 2 (Critical), 5 (Emergency).
uint32_t period_scale();

} // namespace canary::diag
