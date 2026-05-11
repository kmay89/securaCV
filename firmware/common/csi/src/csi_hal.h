/*
 * SecuraCV Canary — CSI HAL (ESP32 backend)
 * Version 0.1.0
 *
 * Implements the CSI interface defined in csi_types.h, backed by
 * Espressif's esp_wifi_set_csi_rx_cb() on ESP32-S3 / ESP32-C3.
 *
 * PRIVACY BARRIER (enforced in this file, tested in conformance checks):
 *   The ESP-IDF CSI callback delivers a wifi_csi_info_t that contains the
 *   source MAC, BSSID, and assorted frame metadata. This module copies out
 *   ONLY the subcarrier samples and aggregate RSSI/timing, then immediately
 *   zeroes the fields of the info struct that can identify a device.
 *
 *   No MAC, no BSSID, no FCS, no sequence number, no frame-control bits
 *   enter the ring buffer that feeds the feature extractor.
 *
 * See also: spec/canary_free_signals_v0.md (Invariant F).
 */

#ifndef SECURACV_CSI_HAL_H
#define SECURACV_CSI_HAL_H

#include <Arduino.h>
#include "csi_types.h"

namespace csi_hal {

/*
 * Thin C++ wrapper around the csi_* C API, sized for the canary-wap loop.
 *
 * Usage:
 *   csi_hal::Config cfg = csi_hal::Config::defaults();
 *   csi_hal::init(cfg);
 *   csi_hal::set_features_callback([](const csi_features_t* f) {
 *     rf_presence::feed_csi_window(f);  // Phase 3 wires this up
 *   });
 *   csi_hal::start();
 *   ...
 *   csi_hal::process();   // in main loop
 */

struct Config {
  uint8_t  channel;          /* 0 = follow current STA/AP channel */
  uint8_t  bandwidth_mhz;    /* 20 or 40 */
  uint16_t max_frame_rate_hz;
  int8_t   rssi_floor_dbm;

  static Config defaults() {
    return Config{
      /* channel */           0,
      /* bandwidth_mhz */      20,
      /* max_frame_rate_hz */  20,
      /* rssi_floor_dbm */     CSI_RSSI_NOISE_FLOOR_DBM
    };
  }
};

using FeaturesCallback = void (*)(const csi_features_t* features);

/* Lifecycle */
bool init(const Config& cfg);
void deinit();
bool start();
void stop();
bool is_running();

/* Callback registration (pass nullptr to unregister) */
void set_features_callback(FeaturesCallback cb);

/* Main-loop pump; returns windows emitted this call (0 or 1 steady-state) */
int process();

/* Introspection */
uint32_t get_caps();
bool     get_stats(csi_stats_t* out);

/* Conformance: scan the ring buffer for any byte sequence that looks like a
 * MAC address (6 consecutive non-zero bytes in the expected OUI ranges).
 * Used by the rf_presence conformance suite to prove no identifiers leaked
 * into the CSI data path. */
bool conformance_check_no_mac_in_buffers();

/* ────────────────────────────────────────────────────────────────────────
 * CHANNEL LOCK
 *
 * Pins the WiFi channel so ESP-NOW probes (csi_probe) and CSI rx land on
 * the same primary channel. Set channel=0 to clear the lock.
 *
 * The lock is advisory: setting it records the expected channel and, if
 * WiFi is currently up, calls esp_wifi_set_channel() with the request.
 * In WIFI_MODE_STA with an active association this may be overridden by
 * the AP — the observed channel is recorded from each CSI frame's
 * rx_ctrl.channel and is_channel_in_sync() reports the truth. The Hub
 * coordination in PR 4 acts on the in-sync signal.
 * ──────────────────────────────────────────────────────────────────────── */

bool    set_channel_lock(uint8_t channel);   /* 0 clears, 1-14 sets */
uint8_t get_channel_lock();                  /* 0 = no lock */
uint8_t get_observed_channel();              /* last channel seen in rx_ctrl */
bool    is_channel_in_sync();                /* true if no lock or observed==lock */

/* ────────────────────────────────────────────────────────────────────────
 * WATCHDOG
 *
 * Detects CSI silence — no frames received for `timeout_ms`. When silent,
 * fires the optional callback (for the integration layer to log to the
 * health chain) and toggles the CSI rx callback off/on as a gentle
 * recovery attempt. Subsequent recovery attempts are throttled to once
 * per WATCHDOG_RECOVERY_MIN_MS (10 s).
 *
 * Set timeout_ms = 0 to disable. The check runs inside process() so the
 * watchdog only fires while the main loop is alive (a hung main loop is
 * caught by the system watchdog, which is a separate concern).
 * ──────────────────────────────────────────────────────────────────────── */

using WatchdogCallback = void (*)(uint32_t silent_ms, uint32_t attempt);

constexpr uint32_t WATCHDOG_DEFAULT_TIMEOUT_MS = 5000;
constexpr uint32_t WATCHDOG_RECOVERY_MIN_MS    = 10000;

void     set_watchdog(uint32_t timeout_ms, WatchdogCallback cb);
uint32_t get_watchdog_timeout_ms();
uint32_t get_ms_since_last_frame();      /* UINT32_MAX if never received */
uint32_t get_watchdog_recovery_count();  /* cumulative since init */

}  /* namespace csi_hal */

#endif  /* SECURACV_CSI_HAL_H */
