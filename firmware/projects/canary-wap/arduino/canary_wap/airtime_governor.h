/*
 * SecuraCV Canary — Airtime Governor
 *
 * Caps how much 2.4 GHz airtime the mesh (Opera ESP-NOW + chirp broadcasts)
 * is allowed to consume on the shared radio, so multi-Canary deployments
 * don't degrade the user's home WiFi.
 *
 * Model
 * ─────
 * Each TX is recorded with its byte count. We estimate airtime per packet as
 *   airtime_us ≈ PHY_PREAMBLE_US + (bytes * 8) / PHY_BIT_RATE_MBPS
 * Then we sum airtime over a rolling 10-second window and compare against a
 * configurable cap (default 2%).
 *
 * Two send classes
 * ────────────────
 *   - Routine: heartbeats, gossip, presence, peer-list refresh, chirp
 *     presence beacons. These call `try_reserve_routine()`. If the projected
 *     airtime would exceed the cap, the send is denied; the caller should
 *     simply skip this tick.
 *   - Urgent:  tamper alerts, OFFLINE_IMMINENT, power-loss notifications.
 *     These call `force_reserve_urgent()`. Always permitted, but still
 *     recorded so telemetry stays honest.
 *
 * Telemetry
 * ─────────
 * `current_airtime_pct()` returns the rolling-window utilization (0-100).
 * The firmware publishes this as `mesh_airtime_pct` over MQTT discovery
 * and the BLE console.
 *
 * Pure-logic module: no Arduino dependency. Time is injected via the
 * caller's `now_ms` so it is host-testable.
 */

#ifndef SECURACV_AIRTIME_GOVERNOR_H
#define SECURACV_AIRTIME_GOVERNOR_H

#include <stdint.h>
#include <stddef.h>

namespace airtime_governor {

// PHY parameters for ESP-NOW @ 1 Mbps long preamble (worst-case, deliberately
// conservative — real rates are typically higher, so we never under-count).
static const uint32_t PHY_PREAMBLE_US = 192;      // long preamble + headers
static const uint32_t PHY_BIT_RATE_KBPS = 1000;   // 1 Mbps fallback rate

static const uint32_t WINDOW_MS = 10000;           // rolling 10-second window
static const uint8_t DEFAULT_CAP_PCT = 2;          // 2% airtime cap (routine)

struct Stats {
  uint32_t window_ms;          // active window size
  uint32_t airtime_us;         // total airtime in window
  uint16_t airtime_pct_x100;   // utilization x100 (e.g. 215 = 2.15%)
  uint32_t routine_allowed;    // count of permitted routine sends
  uint32_t routine_denied;     // count of denied routine sends
  uint32_t urgent_sends;       // count of urgent sends (always allowed)
};

// Estimate the airtime cost of one packet (microseconds).
uint32_t estimate_airtime_us(size_t bytes);

// Initialize / reset the governor. Optional: pass cap_pct = 0 to keep default.
void init(uint8_t cap_pct);

// Attempt to reserve airtime for a routine (non-urgent) send.
//   now_ms: caller's millisecond clock
//   bytes:  packet size on the wire
// Returns true and records the send if allowed; returns false and records a
// denial otherwise.
bool try_reserve_routine(uint32_t now_ms, size_t bytes);

// Force-reserve airtime for an urgent send. Always returns true; the cost
// is still recorded so the rolling window reflects reality.
void force_reserve_urgent(uint32_t now_ms, size_t bytes);

// Telemetry — utilization expressed as percent × 100 (i.e. 215 = 2.15%).
uint16_t airtime_pct_x100(uint32_t now_ms);

// Full snapshot — for the BLE console + HA sensor publish path.
Stats snapshot(uint32_t now_ms);

} // namespace airtime_governor

#endif // SECURACV_AIRTIME_GOVERNOR_H
