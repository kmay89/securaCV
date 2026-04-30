/*
 * SecuraCV Canary — BLE Presence Sensor (continuous low-duty scan)
 *
 * Sister module to wifi_presence: a passive BLE scanner that runs always-on
 * at a low duty cycle and feeds every advertisement into the existing
 * Phase 4–10 pipeline (household IRK matching, familiar bloom, baseline,
 * notify). Without this, the consume side is wired but no producer is
 * actually calling rf_presence::feed_ble_scan() — the BLE-as-sensor role
 * exists only on paper.
 *
 * DUTY CYCLE
 * ──────────
 * Default scan parameters: 80 ms window per 320 ms interval = 25 % radio
 * duty. The remaining 75 % is available for advertising (so pairing /
 * console connections still work) and for connection events on existing
 * links. Drops to 8 % (80 ms / 1000 ms) while a console connection is
 * active so reads stay snappy.
 *
 * COORDINATION WITH bluetooth_channel
 * ────────────────────────────────────
 * NimBLE has ONE scanner singleton. The user-triggered scan in
 * `bluetooth_channel::start_scan()` (the "Scan" button in the SPA) needs a
 * higher duty cycle for ~10 s. That call invokes pause_for_user_scan()
 * before swapping in its own callbacks; resume_continuous_scan() restores
 * the presence-loop callbacks when the user scan finishes.
 *
 * SECURITY
 * ────────
 * - Read-only consumer. We never advertise, connect, or write anything
 *   on behalf of the presence loop — only listen for adverts.
 * - Every match goes through household::resolve_rpa_detailed() which uses
 *   the IRK-based check; a captured advertisement from yesterday will not
 *   resolve today (BLE LE Privacy guarantees the prand changes per ad).
 * - We do not log MAC addresses, OUIs, or names from the scanner — only
 *   the (resolved-slot, rssi, timestamp) tuple feeds rf_presence, exactly
 *   the same surface area as the existing wifi_presence module.
 * - Bounded session-token map upstream (Phase 11 red-team test #1) caps
 *   memory growth from a MAC-randomization flood.
 */

#ifndef SECURACV_BLE_PRESENCE_H
#define SECURACV_BLE_PRESENCE_H

#include "build_config.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include <stdint.h>
#include <stddef.h>

namespace ble_presence {

// Default scan parameters (units: 0.625 ms, the BLE link-layer scan slot).
// 128 * 0.625 = 80 ms window; 512 * 0.625 = 320 ms interval = 25 % duty.
static constexpr uint16_t SCAN_WINDOW_UNITS_NORMAL    = 128;
static constexpr uint16_t SCAN_INTERVAL_UNITS_NORMAL  = 512;

// Reduced duty cycle when a console connection is live — drops to 8 %.
static constexpr uint16_t SCAN_WINDOW_UNITS_REDUCED   = 128;
static constexpr uint16_t SCAN_INTERVAL_UNITS_REDUCED = 1600;  // 1000 ms

// ── Lifecycle ──────────────────────────────────────────────────────────────

// Initialize the presence scanner. Must be called AFTER NimBLEDevice::init
// (so getScan() returns a valid singleton). Idempotent.
bool init();

// Stop scanning, drop callbacks. Only used during full BLE deinit.
void deinit();

// Start the continuous low-duty scan. No-op if already running.
bool start();

// Stop the continuous scan. Used by `pause_for_user_scan()` internally;
// external callers usually want pause/resume instead so that the presence
// loop auto-resumes after the user scan finishes.
void stop();

bool is_running();

// ── Coordination with bluetooth_channel user scan ──────────────────────────

// Hand the NimBLE scanner over to a higher-duty user-triggered scan.
// Returns the conn-state we were in so the caller can pass it back to
// resume_continuous_scan() (currently unused but reserved for future
// "remember whether we were in reduced-duty mode" handoff).
void pause_for_user_scan();

// Re-arm continuous scan after a user-triggered scan completes. Safe to
// call even if pause_for_user_scan() wasn't called — checks state.
void resume_continuous_scan();

// Notify the presence loop that a console connection has come up or gone
// down. The duty cycle drops to SCAN_*_REDUCED while connected so the
// link gets more radio time, then restores to NORMAL on disconnect.
void notify_console_connected(bool connected);

// ── Diagnostics (anonymous aggregates only — no MAC / OUI / RSSI per device)
struct Stats {
  uint32_t adverts_seen;          // monotonic count of all adverts processed
  uint32_t adverts_resolved_household; // count where household::resolve_rpa matched
  uint32_t adverts_dropped_busy;  // dropped because callback budget exhausted
  uint32_t pause_count;           // times user scan preempted the loop
  bool     running;
  bool     reduced_duty;
};
bool get_stats(Stats* out);

}  // namespace ble_presence

#endif // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#endif // SECURACV_BLE_PRESENCE_H
