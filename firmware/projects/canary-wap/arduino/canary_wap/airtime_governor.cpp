/*
 * SecuraCV Canary — Airtime Governor (implementation)
 *
 * Ring of recent 100 ms buckets of sends; sum airtime in the rolling window
 * and gate routine traffic against the configured cap.
 */

#include "airtime_governor.h"
#include "csi_mem.h"

namespace airtime_governor {

// The ring holds 100 ms BUCKETS, not sends. record() adds a send to the
// newest slot when it falls in the same 100 ms of the caller's clock, so
// the ring covers the most recent 256 non-empty buckets (at least 25.6 s)
// whatever the reservation rate: the slot it overwrites is always older
// than the window, and window_airtime_us() never loses in-window airtime.
//
// It used to hold one slot per send, sized on the guess that 256 sends
// "comfortably" cover a window. They do not above 25.6 reservations a
// second: the oldest in-window sends were overwritten, the window read low
// and the cap stopped holding. Host-measured on that ring: 200 Hz x 16 B
// for 10 s was allowed in full and read 0.82 % (true 6.41 %); the CSI
// probe's framed 75 B frames (792 us) held the 2 % cap only because they
// cleared the 781.25 us-per-slot line (cap_us / RING_SIZE) by 1.4 %, and
// a 3 % cap did not hold (12.7 % true, 2.0 % read).
//
// A slot's timestamp is its NEWEST send, so a bucket leaves the window
// with its last send: the window reads 10.0-10.1 s, never less, and the
// cap can only err toward denying. This assumes one caller clock that does
// not step back across a bucket (every caller is the loop task's millis());
// a step back opens a new slot, which only shortens how far back the ring
// reaches. Sizing is unchanged: 256 x 8 B = 2 KB of PSRAM (ram_audit.yml).
static constexpr size_t RING_SIZE = 256;
static constexpr uint32_t BUCKET_MS = 100;
static_assert(RING_SIZE * BUCKET_MS > WINDOW_MS + BUCKET_MS,
              "the bucket ring must span the window plus one bucket");

struct Slot {
  uint32_t ts_ms;
  uint32_t airtime_us;
};

/* PSRAM-resident (csi_mem.h): touched only from the loop-task send paths.
 * Sizing: RING_SIZE (256) slots x sizeof(Slot) (8 B: ts_ms + airtime_us)
 * = 2 KB. Allocated in init(); if that ever fails (heap exhausted at
 * boot), record() no-ops and the usage window reads 0 — the governor
 * fails open rather than silencing the alert channels. This file is
 * host-compiled (test_mesh_coexistence), so it cannot log itself; callers
 * check ring_ok() and log the failure (mesh_network::init does). */
static Slot* g_ring = nullptr;
static constexpr size_t RING_BYTES = RING_SIZE * sizeof(Slot);
static size_t g_head = 0;       // next write position
static size_t g_count = 0;      // number of valid entries (<= RING_SIZE)

static uint32_t g_routine_allowed = 0;
static uint32_t g_routine_denied = 0;
static uint32_t g_urgent_sends = 0;

// v0.3: distinct Beacon counters per spec/beacon_channel_v0.md §8 so
// HA MQTT can surface Beacon airtime separately from Opera tamper/power.
//
// Threading invariant (gemini P1 follow-up): all callers of
// `force_reserve_beacon` and `beacon_window_airtime_us` are the main loop
// task. Specifically:
//   - `force_reserve_beacon` is called from `emit_alert_frame` and
//     `emit_selftest` in beacon_channel.cpp; those run from
//     `beacon_channel::update()` (loop task) or from the
//     `cosign_pending_request` REST path (Bearer-gated, same task).
//   - The MQTT publish loop in canary_wap.ino reads via snapshot() and
//     also runs on the loop task.
// No ISR or networking-callback task mutates this state. If that
// invariant ever needs to relax, wrap the ring updates in a critical
// section (or atomic store) so partial reads cannot observe a torn
// multi-word `BeaconSlot`.
static uint32_t g_beacon_sends = 0;
static uint32_t g_beacon_airtime_us = 0;
struct BeaconSlot {
  uint32_t ts_ms;
  uint32_t airtime_us;
};
static constexpr size_t BEACON_RING_SIZE = 32;
static BeaconSlot g_beacon_ring[BEACON_RING_SIZE];
static size_t g_beacon_head = 0;
static size_t g_beacon_count = 0;

static uint8_t g_cap_pct = DEFAULT_CAP_PCT;

uint32_t estimate_airtime_us(size_t bytes) {
  // bits / kbps gives milliseconds; convert to microseconds.
  const uint32_t payload_us = (static_cast<uint32_t>(bytes) * 8u * 1000u) /
                              PHY_BIT_RATE_KBPS;
  return PHY_PREAMBLE_US + payload_us;
}

void init(uint8_t cap_pct) {
  if (!g_ring) g_ring = (Slot*)csi_large_calloc(RING_BYTES);
  g_head = 0;
  g_count = 0;
  g_routine_allowed = 0;
  g_routine_denied = 0;
  g_urgent_sends = 0;
  g_beacon_sends = 0;
  g_beacon_airtime_us = 0;
  g_beacon_head = 0;
  g_beacon_count = 0;
  g_cap_pct = (cap_pct > 0) ? cap_pct : DEFAULT_CAP_PCT;
  if (g_ring) {
    for (size_t i = 0; i < RING_SIZE; i++) {
      g_ring[i].ts_ms = 0;
      g_ring[i].airtime_us = 0;
    }
  }
  for (size_t i = 0; i < BEACON_RING_SIZE; i++) {
    g_beacon_ring[i].ts_ms = 0;
    g_beacon_ring[i].airtime_us = 0;
  }
}

/* True when the send-window ring exists; callers use this to health-log
 * an allocation failure once at boot (this host-compiled file can't). */
bool ring_ok() { return g_ring != nullptr; }

static void record(uint32_t now_ms, uint32_t airtime_us) {
  if (!g_ring) return;  /* alloc failed — governor fails open, window reads 0 */
  if (g_count > 0) {
    Slot& last = g_ring[(g_head + RING_SIZE - 1) % RING_SIZE];
    if (last.ts_ms / BUCKET_MS == now_ms / BUCKET_MS) {  /* same 100 ms bucket */
      /* Keep the bucket's NEWEST send time (signed: a same-bucket step
       * back keeps the later stamp), so the bucket ages out last. */
      if (static_cast<int32_t>(now_ms - last.ts_ms) > 0) last.ts_ms = now_ms;
      last.airtime_us += airtime_us;
      return;
    }
  }
  g_ring[g_head].ts_ms = now_ms;
  g_ring[g_head].airtime_us = airtime_us;
  g_head = (g_head + 1) % RING_SIZE;
  if (g_count < RING_SIZE) g_count++;
}

// Sum the buckets whose newest send lies in [now - WINDOW_MS, now]: every
// send in the window, plus up to 99 ms of sends that share a bucket with one.
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

void force_reserve_beacon(uint32_t now_ms, size_t bytes) {
  const uint32_t cost = estimate_airtime_us(bytes);
  record(now_ms, cost);
  // Distinct Beacon ring + counter.
  g_beacon_ring[g_beacon_head].ts_ms = now_ms;
  g_beacon_ring[g_beacon_head].airtime_us = cost;
  g_beacon_head = (g_beacon_head + 1) % BEACON_RING_SIZE;
  if (g_beacon_count < BEACON_RING_SIZE) g_beacon_count++;
  g_beacon_sends++;
}

static uint32_t beacon_window_airtime_us(uint32_t now_ms) {
  uint32_t total = 0;
  for (size_t i = 0; i < g_beacon_count; i++) {
    const BeaconSlot& s = g_beacon_ring[i];
    if (s.ts_ms == 0 && s.airtime_us == 0) continue;
    const uint32_t age = now_ms - s.ts_ms;
    if (age <= WINDOW_MS) total += s.airtime_us;
  }
  return total;
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
  s.beacon_sends = g_beacon_sends;
  s.beacon_airtime_us = beacon_window_airtime_us(now_ms);
  return s;
}

} // namespace airtime_governor
