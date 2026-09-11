/*
 * SecuraCV Canary — CSI HAL (ESP32 backend)
 * Version 0.1.0
 *
 * Implements the CSI interface defined in csi_types.h, backed by
 * Espressif's esp_wifi_set_csi_rx_cb() on ESP32-S3 / ESP32-C3.
 *
 * PRIVACY BARRIER (enforced in this file, tested in
 * csi_hal_transmitter_filter_test.cpp):
 *   The ESP-IDF CSI callback delivers a wifi_csi_info_t that contains the
 *   transmitter MAC, the destination MAC, and assorted frame metadata. This
 *   module copies out ONLY the subcarrier samples and aggregate RSSI/timing.
 *
 *   No MAC, no BSSID, no FCS, no sequence number, no frame-control bits
 *   enter the ring buffer that feeds the feature extractor, a stat, a log
 *   line or any wire format.
 *
 *   The transmitter filter (below) READS info->mac — six bytes, in place —
 *   to compare it against the BSSID of the AP this station is associated
 *   with, and hands the same pointer to the optional peer hook. It is a
 *   memcmp, not a copy: the bytes never leave the driver's struct.
 *
 *   The ONE identifier this HAL keeps is that associated BSSID, in a single
 *   file-static (s_assoc_bssid), read back from esp_wifi_sta_get_ap_info().
 *   It is never exported, never logged (the log line says "learned", not
 *   what), and wiped on deinit(). It is the address of the household's own
 *   router — the link the sensor listens on, not a device it observes.
 *
 *   Invariant F (spec/canary_free_signals_v0.md, Symmetry) is untouched:
 *   the filter selects the sensing LINK, it does not classify devices or
 *   people, and nothing about it reaches an event. frames_dropped_foreign
 *   is a driver counter beside frames_dropped_rssi, not an event field.
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
  /* Transmitter filter: accept frames only from the associated AP's BSSID
   * (and registered peers, see set_peer_filter). Off = the pre-filter
   * behavior, every decoded frame on the channel lands in the window. */
  bool     filter_foreign;

  static Config defaults() {
    return Config{
      /* channel */           0,
      /* bandwidth_mhz */      20,
      /* max_frame_rate_hz */  20,
      /* rssi_floor_dbm */     CSI_RSSI_NOISE_FLOOR_DBM,
      /* filter_foreign */     true
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
 * TRANSMITTER FILTER
 *
 * Every transmitter the radio can decode lands in the CSI callback:
 * neighbor APs' beacons, other households' stations, peer Canaries'
 * ESP-NOW probes, and the frames from the router this station is
 * associated with. Each link has its own channel response, so a 64-frame
 * window that alternates between links measures the DIFFERENCE between
 * links, and per-subcarrier variance reads as motion. The filter keeps
 * one link: frames whose transmitter address is the associated AP's BSSID
 * (beacons, echo replies to csi_traffic's pings, data to us) and, when a
 * peer hook is installed, registered peer Canaries. Everything else is
 * counted under csi_stats_t::frames_dropped_foreign and never buffered.
 *
 * Privacy: see the barrier note at the top of this file. The comparison
 * is a 6-byte memcmp against info->mac in place; the associated BSSID is
 * the single identifier the HAL holds, and it is never exported.
 *
 * Arming: the filter engages once a BSSID is known. Until the STA has
 * associated (AP-only install, captive-portal phase) there is nothing to
 * compare against and every frame is accepted — the pre-filter behavior,
 * reported by has_associated_bssid() == false. A held BSSID survives a
 * disconnect until a reassociation replaces it (roaming to another AP of
 * the same network is picked up by the poll below) or deinit() wipes it.
 *
 * Learning the BSSID: process() polls esp_wifi_sta_get_ap_info() from the
 * main loop — every second while none is held, every
 * TRANSMITTER_BSSID_POLL_MS once one is — so a consumer that never calls
 * anything below still converges within a second of association. The
 * integration layer's STA got-IP handler should call
 * request_bssid_refresh() to make that immediate.
 * ──────────────────────────────────────────────────────────────────────── */

constexpr uint32_t TRANSMITTER_BSSID_POLL_MS         = 10000;  /* once known */
constexpr uint32_t TRANSMITTER_BSSID_POLL_UNKNOWN_MS = 1000;   /* until known */

/* Re-read the associated AP's BSSID from the driver now. MAIN-LOOP (or
 * init-time) context only: it is the single writer of the held BSSID and
 * calls into the Wi-Fi driver, so never call it from a Wi-Fi event
 * handler or the CSI callback — use request_bssid_refresh() there.
 * Returns true when a BSSID is held afterwards (kept from before if the
 * STA is currently not associated). */
bool refresh_associated_bssid();

/* Ask process() to refresh on its next tick. Safe from any task (the
 * Arduino Wi-Fi event task included) — it only sets a flag. */
void request_bssid_refresh();

/* True when the filter has a BSSID to compare against (see "Arming"). */
bool has_associated_bssid();

/* Runtime mirror of Config::filter_foreign for a settings surface. Takes
 * effect on the next frame. */
void set_filter_foreign(bool on);
bool get_filter_foreign();

/* Optional second allow-list: registered peer Canaries. The hook runs in
 * the Wi-Fi task for every frame the BSSID compare rejected, receiving a
 * pointer INTO the driver's info->mac for the duration of the callback.
 * It must compare and return — never copy or log. The intended target is
 * csi_probe::has_peer, which already holds those addresses in its RAM
 * table; the HAL does not keep a second copy. nullptr clears the hook. */
using PeerFilter = bool (*)(const uint8_t* mac);
void set_peer_filter(PeerFilter fn);

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
