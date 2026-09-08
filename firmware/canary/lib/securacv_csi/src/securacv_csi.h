/*
 * SecuraCV Canary — WiFi CSI sensing (product adapter over firmware/common/csi)
 *
 * This header used to declare a second copy of the CSI HAL's types, and the
 * .cpp beside it carried a second copy of the HAL and the feature extractor;
 * the two were kept equal by hand (roadmap 22). Now:
 *
 *   - Every type, constant and the portable C API (csi_init, csi_get_stats,
 *     csi_is_running, ...) comes from the canonical csi_types.h. There is ONE
 *     csi_features_t in the build, so a translation unit may include this
 *     header together with csi_hal.h / csi_module.h.
 *   - The `csi::` namespace below is the canary product's historical spelling
 *     of the HAL API; securacv_csi.cpp implements it by delegating to
 *     csi_hal:: (firmware/common/csi/src/csi_hal.cpp, compiled into every
 *     canary env by platformio.ini's build_src_filter).
 *
 * The privacy invariants and the 32-byte feature layout are documented once,
 * in csi_types.h. firmware/scripts/check_csi_sync.sh fails the build if this
 * library re-declares that contract or grows a HAL body again.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CSI_H
#define SECURACV_CSI_H

#include "csi_types.h"

#ifdef __cplusplus

namespace csi {

  /* Same signature as csi_hal::FeaturesCallback; the adapter asserts it. */
  using FeaturesCallback = void (*)(const csi_features_t* features);

  /* Lifecycle. init() is safe before WiFi is up — start() defers itself and
   * process() retries the driver registration until WiFi is running. */
  bool init(const csi_config_t& cfg);
  void deinit();
  bool start();
  void stop();
  bool is_running();

  /* Callback registration (pass nullptr to unregister). Fires at ~1 Hz, one
   * per CSI_WINDOW_MS window; the pointer is owned by the caller and must be
   * consumed synchronously. */
  void set_features_callback(FeaturesCallback cb);

  /* Main-loop pump; returns windows emitted this call (0 or 1 steady-state).
   * Also runs the HAL's silence watchdog (csi_hal::set_watchdog). */
  int process();

  /* Introspection */
  uint32_t get_caps();
  bool     get_stats(csi_stats_t* out);

  /* Defense-in-depth heuristic scan of the HAL ring for MAC-shaped byte
   * runs (csi_hal.h). Always returns true; logs anything suspicious. */
  bool conformance_check_no_mac_in_buffers();

  /* Transmitter filter (csi_hal.h "TRANSMITTER FILTER"; the canonical HAL
   * holds the associated AP's BSSID and compares, never copies, a frame's
   * transmitter against it). request_bssid_refresh() is safe from any task
   * (the Wi-Fi event task's STA_GOT_IP handler) — process() re-reads the
   * BSSID on its next tick; has_associated_bssid() is false until the STA
   * has associated, and while false every frame is accepted. */
  void request_bssid_refresh();
  bool has_associated_bssid();

}  /* namespace csi */

#endif  /* __cplusplus */

#endif  /* SECURACV_CSI_H */
