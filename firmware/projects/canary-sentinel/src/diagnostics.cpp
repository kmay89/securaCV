#include "canary/diagnostics.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "canary/config.h"
#include "canary/log.h"

namespace canary::diag {

namespace {

Snapshot g_snap{};
uint32_t g_last_sample_ms = 0;
bool     g_sampled_once = false;

Level level_for(uint32_t free_heap) {
  if (free_heap < HEAP_EMERGENCY_BYTES) return Level::Emergency;
  if (free_heap < HEAP_CRITICAL_BYTES)  return Level::Critical;
  if (free_heap < HEAP_WARN_BYTES)      return Level::Warn;
  return Level::Normal;
}

// Entry threshold of a level — de-escalation out of it requires free heap
// above (threshold + HEAP_HYSTERESIS).
uint32_t entry_threshold(Level level) {
  switch (level) {
    case Level::Emergency: return HEAP_EMERGENCY_BYTES;
    case Level::Critical:  return HEAP_CRITICAL_BYTES;
    case Level::Warn:      return HEAP_WARN_BYTES;
    default:               return 0;
  }
}

}  // namespace

void loop(uint32_t now_ms) {
  if (g_sampled_once && (int32_t)(now_ms - g_last_sample_ms) < 1000) return;
  g_last_sample_ms = now_ms;
  g_sampled_once = true;

  g_snap.free_heap     = (uint32_t)ESP.getFreeHeap();
  g_snap.min_heap      = (uint32_t)ESP.getMinFreeHeap();
  g_snap.largest_block =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  const Level target = level_for(g_snap.free_heap);
  if (target > g_snap.level) {
    // Escalate immediately.
    g_snap.level = target;
    log_header("DIAG");
    canary::dbg_serial().printf("Heap pressure: %s (free=%lu min=%lu)\n",
                                level_name(g_snap.level),
                                (unsigned long)g_snap.free_heap,
                                (unsigned long)g_snap.min_heap);
  } else if (target < g_snap.level &&
             g_snap.free_heap >
                 entry_threshold(g_snap.level) + HEAP_HYSTERESIS) {
    // De-escalate one step at a time, only past the hysteresis band.
    g_snap.level = (Level)((uint8_t)g_snap.level - 1);
    log_header("DIAG");
    canary::dbg_serial().printf("Heap recovered: %s (free=%lu)\n",
                                level_name(g_snap.level),
                                (unsigned long)g_snap.free_heap);
  }
}

const Snapshot& get() { return g_snap; }

const char* level_name(Level level) {
  switch (level) {
    case Level::Warn:      return "warn";
    case Level::Critical:  return "critical";
    case Level::Emergency: return "emergency";
    default:               return "normal";
  }
}

uint32_t period_scale() {
  switch (g_snap.level) {
    case Level::Critical:  return 2;
    case Level::Emergency: return 5;
    default:               return 1;
  }
}

} // namespace canary::diag
