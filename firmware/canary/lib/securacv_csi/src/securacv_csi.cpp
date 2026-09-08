/*
 * SecuraCV Canary — WiFi CSI sensing: product adapter over firmware/common/csi
 *
 * This file is deliberately small. The HAL (esp_wifi CSI callback, SPSC ring,
 * MAC/BSSID scrub barrier, deferred start, channel lock, silence watchdog)
 * and the 32-dim feature extractor live ONCE, in
 * firmware/common/csi/src/csi_hal.cpp and csi_features.cpp, which the canary
 * build compiles directly (platformio.ini build_src_filter). The single
 * esp_wifi CSI callback registration in the image is theirs.
 *
 * What stays here is the canary product's spelling of that API — the `csi::`
 * namespace main.cpp calls — plus the one product courtesy the shared HAL
 * does not do: a health-log notice when a caller asks for a channel the HAL
 * cannot honor. The portable C API (csi_is_running, csi_get_stats, ...) that
 * securacv_network and securacv_power_policy call is csi_hal.cpp's own.
 *
 * Do not add bodies here. firmware/scripts/check_csi_sync.sh fails the
 * build on a definition this file shares with the canonical HAL, on any
 * direct esp_wifi CSI driver call, and on a line count past its budget.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_csi.h"
#include "csi_hal.h"

#include <stdio.h>
#include <type_traits>

#include "log_level.h"
#include "securacv_witness.h"  /* log_health() */

static_assert(std::is_same<csi::FeaturesCallback, csi_hal::FeaturesCallback>::value,
              "csi:: and csi_hal:: must agree on the features callback type");

namespace csi {

static csi_hal::Config to_hal_config(const csi_config_t& cfg) {
  csi_hal::Config c = csi_hal::Config::defaults();
  /* Every field verbatim. 0 is a valid rssi_floor_dbm on this path — a caller
   * that wants the default uses CSI_CONFIG_DEFAULT. (The portable csi_init()
   * C shim in csi_hal.cpp is the one that reads 0 as "use default".) */
  c.channel           = cfg.channel;
  c.bandwidth_mhz     = cfg.bandwidth_mhz;
  c.max_frame_rate_hz = cfg.max_frame_rate_hz;
  c.rssi_floor_dbm    = cfg.rssi_floor_dbm;
  c.filter_foreign    = cfg.filter_foreign;
  return c;
}

bool init(const csi_config_t& cfg) {
  if (!csi_hal::init(to_hal_config(cfg))) return false;
  /* CSI rides the driver's AP/STA channel, so a requested channel is
   * advisory (csi_types.h). Say so once in the health log rather than let
   * the request vanish silently; the observed channel is in v[26]. */
  if (cfg.channel != 0) {
    char detail[40];
    snprintf(detail, sizeof(detail),
             "requested ch=%u (advisory only)", (unsigned)cfg.channel);
    log_health(LOG_LEVEL_NOTICE, LOG_CAT_SENSOR,
               "CSI follows AP/STA channel; request ignored", detail);
  }
  return true;
}

void deinit()                                    { csi_hal::deinit(); }
bool start()                                     { return csi_hal::start(); }
void stop()                                      { csi_hal::stop(); }
bool is_running()                                { return csi_hal::is_running(); }
void set_features_callback(FeaturesCallback cb)  { csi_hal::set_features_callback(cb); }
int  process()                                   { return csi_hal::process(); }
uint32_t get_caps()                              { return csi_hal::get_caps(); }
bool get_stats(csi_stats_t* out)                 { return csi_hal::get_stats(out); }
bool conformance_check_no_mac_in_buffers()       { return csi_hal::conformance_check_no_mac_in_buffers(); }
/* Transmitter filter (csi_hal.h "TRANSMITTER FILTER"): the got-IP handler
 * flags a refresh; process() does the driver call on the main loop. */
void request_bssid_refresh()                     { csi_hal::request_bssid_refresh(); }
bool has_associated_bssid()                      { return csi_hal::has_associated_bssid(); }

}  /* namespace csi */
