/*
 * SecuraCV Canary — Airtime Governor (implementation)
 *
 * Ring-buffer of recent sends; sum airtime in the rolling window and gate
 * routine traffic against the configured cap.
 */

#include "airtime_governor.h"

namespace airtime_governor {

// Ring-buffer capacity. At 1 Mbps and 250-byte max packets we get one
// "send slot" per ~2 ms; ten seconds of worst-case routine traffic would be
// 5000 slots, but the routine traffic budget is at most 2% of that. We size
// the ring to 256 entries which comfortably holds a window's worth of
// realistic mesh + chirp traffic.
static constexpr size_t RING_SIZE = 256;

struct Slot {
  uint32_t ts_ms;
  uint32_t airtime_us;
};

static Slot g_ring[RING_SIZE];
static size_t g_head = 0;       // next write position
static size_t g_count = 0;      // number of valid entries (<= RING_SIZE)

static uint32_t g_routine_allowed = 0;
static uint32_t g_routine_denied = 0;
static uint32_t g_urgent_sends = 0;

static uint8_t g_cap_pct = DEFAULT_CAP_PCT;

uint32_t estimate_airtime_us(size_t bytes) {
  // bits / kbps gives milliseconds; convert to microseconds.
  const uint32_t payload_us = (static_cast<uint32_t>(bytes) * 8u * 1000u) /
                              PHY_BIT_RATE_KBPS;
  return PHY_PREAMBLE_US + payload_us;
}

void init(uint8_t cap_pct) {
  g_head = 0;
  g_count = 0;
  g_routine_allowed = 0;
  g_routine_denied = 0;
  g_urgent_sends = 0;
  g_cap_pct = (cap_pct > 0) ? cap_pct : DEFAULT_CAP_PCT;
  for (size_t i = 0; i < RING_SIZE; i++) {
    g_ring[i].ts_ms = 0;
    g_ring[i].airtime_us = 0;
  }
}

static void record(uint32_t now_ms, uint32_t airtime_us) {
  g_ring[g_head].ts_ms = now_ms;
  g_ring[g_head].airtime_us = airtime_us;
  g_head = (g_head + 1) % RING_SIZE;
  if (g_count < RING_SIZE) g_count++;
}

// Sum airtime in [now - WINDOW_MS, now].
static uint32_t window_airtime_us(uint32_t now_ms) {
  // Handle millis() rollover by treating older timestamps as "out of window."
  // The window is 10 s, well under the 49-day rollover.
  uint32_t total = 0;
  for (size_t i = 0; i < g_count; i++) {
    const Slot& s = g_ring[i];
    if (s.ts_ms == 0 && s.airtime_us == 0) continue;
    const uint32_t age = now_ms - s.ts_ms;
    if (age <= WINDOW_MS) total += s.airtime_us;
  }
  return total;
}

// 2% of 10 s = 200 ms = 200,000 us, so cap_us = WINDOW_MS * 1000 * pct / 100.
static uint32_t cap_us() {
  return (static_cast<uint32_t>(WINDOW_MS) * 1000u *
          static_cast<uint32_t>(g_cap_pct)) / 100u;
}

bool try_reserve_routine(uint32_t now_ms, size_t bytes) {
  const uint32_t cost = estimate_airtime_us(bytes);
  if (window_airtime_us(now_ms) + cost > cap_us()) {
    g_routine_denied++;
    return false;
  }
  record(now_ms, cost);
  g_routine_allowed++;
  return true;
}

void force_reserve_urgent(uint32_t now_ms, size_t bytes) {
  record(now_ms, estimate_airtime_us(bytes));
  g_urgent_sends++;
}

uint16_t airtime_pct_x100(uint32_t now_ms) {
  // utilization = airtime_us / window_us; * 10000 to get pct_x100.
  const uint64_t used = window_airtime_us(now_ms);
  const uint64_t total = static_cast<uint64_t>(WINDOW_MS) * 1000u;
  if (total == 0) return 0;
  uint64_t pct100 = (used * 10000u) / total;
  if (pct100 > 0xFFFF) pct100 = 0xFFFF;
  return static_cast<uint16_t>(pct100);
}

Stats snapshot(uint32_t now_ms) {
  Stats s{};
  s.window_ms = WINDOW_MS;
  s.airtime_us = window_airtime_us(now_ms);
  s.airtime_pct_x100 = airtime_pct_x100(now_ms);
  s.routine_allowed = g_routine_allowed;
  s.routine_denied = g_routine_denied;
  s.urgent_sends = g_urgent_sends;
  return s;
}

} // namespace airtime_governor
