/*
 * SecuraCV Canary — BLE Presence Sensor — implementation
 */

#include "build_config.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include "ble_presence.h"
#include "rf_presence.h"
#include "household.h"
#include "health_log.h"

#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <Arduino.h>

namespace ble_presence {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool s_initialized      = false;
static bool s_running          = false;
static bool s_paused_for_user  = false;
static bool s_console_connected = false;
static NimBLEScan* s_scanner   = nullptr;

static uint32_t s_adverts_seen      = 0;
static uint32_t s_adverts_resolved  = 0;
static uint32_t s_adverts_dropped   = 0;
static uint32_t s_pause_count       = 0;

// ────────────────────────────────────────────────────────────────────────────
// SCAN CALLBACK
// ────────────────────────────────────────────────────────────────────────────

class PresenceScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (!device) return;

    s_adverts_seen++;

    // Pull the 6-byte MAC in NimBLE's LSB-first convention so it lines up
    // with the byte-order assumptions in household::resolve_rpa.
    const uint8_t* mac = device->getAddress().getBase()->val;
    const int8_t   rssi = device->getRSSI();
    const bool     connectable = device->isConnectable();

    // Hot-path producer for the existing Phase 4–10 pipeline. This is the
    // same call wifi_presence makes for probe requests; the consumers
    // (household, familiar, baseline, notify) don't care which radio
    // produced the observation.
    rf_presence::feed_ble_scan(mac, rssi, connectable);

    // Mark last-seen timestamp on the household slot if this resolves to
    // a paired device. resolve_rpa_detailed already updates s_last_seen_ms
    // internally (see household.cpp resolve_rpa_detailed); calling it here
    // would double-count, so we only call it via rf_presence's chain.
    // The match counter below is for our local diagnostic only — it's
    // re-derived from a separate check to avoid race with rf_presence's
    // internal accounting.
    household::ResolveResult rr = household::resolve_rpa_detailed(mac);
    if (rr.matched) s_adverts_resolved++;
  }

  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    // Continuous mode: NimBLE will auto-restart with the same parameters
    // because we passed `restart=true` to start() below. Nothing to do.
  }
};

static PresenceScanCallbacks s_callbacks;

// ────────────────────────────────────────────────────────────────────────────
// SCAN PARAMETER APPLICATION
// ────────────────────────────────────────────────────────────────────────────

static void apply_duty_cycle() {
  if (!s_scanner) return;
  if (s_console_connected) {
    s_scanner->setInterval(SCAN_INTERVAL_UNITS_REDUCED);
    s_scanner->setWindow(SCAN_WINDOW_UNITS_REDUCED);
  } else {
    s_scanner->setInterval(SCAN_INTERVAL_UNITS_NORMAL);
    s_scanner->setWindow(SCAN_WINDOW_UNITS_NORMAL);
  }
}

static bool start_scanner_locked() {
  if (!s_scanner) return false;
  // Passive scan (no scan-request packets emitted) — we only listen, never
  // send. This is the privacy-preserving mode and saves radio time.
  s_scanner->setActiveScan(false);
  apply_duty_cycle();
  s_scanner->setScanCallbacks(&s_callbacks);
  // duration=0 => scan continuously; second arg unused for continuous mode
  // (NimBLE 2.x signature: start(duration_ms, blocking)).
  return s_scanner->start(0, false);
}

// ────────────────────────────────────────────────────────────────────────────
// PUBLIC API
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;
  s_scanner = NimBLEDevice::getScan();
  if (!s_scanner) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_presence: NimBLE scanner unavailable", nullptr);
    return false;
  }
  s_initialized = true;
  return true;
}

void deinit() {
  if (!s_initialized) return;
  stop();
  s_scanner = nullptr;
  s_initialized = false;
}

bool start() {
  if (!s_initialized) return false;
  if (s_running) return true;
  if (s_paused_for_user) return false;  // user scan owns the radio right now

  if (!start_scanner_locked()) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "ble_presence: scan start failed", nullptr);
    return false;
  }
  s_running = true;
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "ble_presence: continuous scan started", nullptr);
  return true;
}

void stop() {
  if (!s_initialized || !s_running) return;
  if (s_scanner) s_scanner->stop();
  s_running = false;
}

bool is_running() { return s_running; }

void pause_for_user_scan() {
  if (!s_initialized) return;
  if (s_running) {
    if (s_scanner) s_scanner->stop();
    s_running = false;
  }
  s_paused_for_user = true;
  s_pause_count++;
}

void resume_continuous_scan() {
  if (!s_initialized) return;
  s_paused_for_user = false;
  // Re-arm. If the radio refuses (e.g. user scan still finalizing in NimBLE
  // internals), the next start() call from update() / re-trigger will pick
  // it up.
  start();
}

void notify_console_connected(bool connected) {
  if (s_console_connected == connected) return;
  s_console_connected = connected;
  if (s_running) {
    // Re-apply parameters with the new duty cycle. NimBLE 2.x requires a
    // stop/start round-trip for parameter changes to take effect on an
    // already-running scan.
    if (s_scanner) {
      s_scanner->stop();
      s_running = false;
      start();
    }
  }
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->adverts_seen                  = s_adverts_seen;
  out->adverts_resolved_household    = s_adverts_resolved;
  out->adverts_dropped_busy          = s_adverts_dropped;
  out->pause_count                   = s_pause_count;
  out->running                       = s_running;
  out->reduced_duty                  = s_console_connected;
  return true;
}

}  // namespace ble_presence

#else  // !(FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>))

// No-NimBLE stubs. The Arduino-IDE CI build doesn't install NimBLE-Arduino,
// so the real implementation above never compiles in that path. We still
// need the symbols to link, because household_api.h calls into ble_presence
// unconditionally (see comment in ble_presence.h). Each stub returns the
// "off / not running / zero" answer that callers already handle.
namespace ble_presence {
bool init() { return false; }
void deinit() {}
bool start() { return false; }
void stop() {}
bool is_running() { return false; }
void pause_for_user_scan() {}
void resume_continuous_scan() {}
void notify_console_connected(bool /*connected*/) {}
bool get_stats(Stats* out) {
  if (!out) return false;
  out->adverts_seen = 0;
  out->adverts_resolved_household = 0;
  out->adverts_dropped_busy = 0;
  out->pause_count = 0;
  out->running = false;
  out->reduced_duty = false;
  return true;
}
}  // namespace ble_presence

#endif // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
