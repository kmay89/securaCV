/*
 * SecuraCV Canary — WiFi CSI sensing
 *
 * Privacy-preserving environmental sensing from WiFi Channel-State-Information.
 * Detects motion, breathing, and micro-activity from subcarrier amplitude/phase
 * dynamics WITHOUT storing or transmitting any device identifier.
 *
 * PRIVACY INVARIANTS (enforced by the implementation, asserted at compile time):
 *   1. The source MAC and BSSID are scrubbed from the raw frame before the
 *      frame enters any buffer that outlives a single callback.
 *   2. Only aggregated, non-identifying features cross this interface.
 *   3. No subcarrier sample is exported — only the compressed feature vector.
 *   4. Features are bucketed (int8) so fine-grained side channels are lost.
 *   5. No per-frame timestamp is exported; only a coarse 10-minute time bucket.
 *
 * Feature-vector layout (32 int8 bytes, mean-centered, scaled):
 *   [ 0..7]  Subcarrier amplitude variance, 8 grouped bands (low → high freq)
 *   [ 8..11] Phase-difference Doppler, 4 sign-aware bands (direction of motion)
 *   [12..19] Breathing/micro-motion FFT, 0.1–0.5 Hz spectrum in 8 Goertzel bins
 *   [20..23] RSSI stats over the window: mean, std, max, min
 *   [24..27] Frame-rate health: frames_received, dropped_estimate, channel, bw
 *   [28..31] Reserved for future features (sounding, phase unwrap)
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CSI_H
#define SECURACV_CSI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

#define CSI_FEATURE_DIM            32
#define CSI_MAX_SUBCARRIERS        128
#define CSI_WINDOW_MS              1000
#define CSI_RSSI_NOISE_FLOOR_DBM   (-85)

/* ──────────────────────────────────────────────────────────────────────────
 * TYPES
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
  CSI_CAP_NONE          = 0,
  CSI_CAP_HT20          = 1 << 0,
  CSI_CAP_HT40          = 1 << 1,
  CSI_CAP_VHT80         = 1 << 2,
  CSI_CAP_PHASE         = 1 << 3,
  CSI_CAP_SOUNDING_11BF = 1 << 4,
} csi_cap_t;

typedef struct {
  int8_t   v[CSI_FEATURE_DIM];
  uint16_t frames_in_window;
  uint8_t  time_bucket;     /* 10-minute bucket (0..143), matches rf_presence */
  uint8_t  caps_observed;   /* csi_cap_t bitmask used in this window */
} csi_features_t;

/* The feature struct is the only thing that crosses the privacy barrier;
 * statically prove the int8 vector width so a refactor cannot widen it. */
#ifdef __cplusplus
static_assert(sizeof(((csi_features_t*)0)->v) == CSI_FEATURE_DIM,
              "CSI feature vector must be exactly 32 int8 bytes");
#endif

typedef struct {
  /* CSI piggy-backs on the WiFi driver's existing AP/STA mode, so channel
   * and bandwidth are determined by that mode — these fields are advisory
   * only and are NOT pushed into esp_wifi_set_channel() (doing so would
   * fight the AP/STA bring-up and stall associated clients). They are
   * recorded for diagnostics; the actual values used appear in the
   * feature vector at v[26] (channel) and v[27] (bandwidth_code). A
   * non-zero `channel` request emits a one-shot notice to health_log. */
  uint8_t  channel;            /* 0 = follow current STA/AP channel */
  uint8_t  bandwidth_mhz;      /* 20 or 40 — advisory only */
  uint16_t max_frame_rate_hz;  /* rate-limit; 0 = unlimited */
  int8_t   rssi_floor_dbm;     /* drop frames with RSSI below this dBm */
} csi_config_t;

#define CSI_CONFIG_DEFAULT { \
    /*.channel*/           0, \
    /*.bandwidth_mhz*/     20, \
    /*.max_frame_rate_hz*/ 20, \
    /*.rssi_floor_dbm*/    CSI_RSSI_NOISE_FLOOR_DBM \
}

typedef struct {
  uint32_t frames_received;
  uint32_t frames_dropped_rssi;
  uint32_t frames_dropped_rate;
  uint32_t frames_dropped_full;
  uint32_t windows_emitted;
  uint32_t windows_degraded;
  uint32_t frames_dropped_short;  /* no L-LTF section (< 128 bytes) */
} csi_stats_t;

#ifdef __cplusplus

namespace csi {

  using FeaturesCallback = void (*)(const csi_features_t* features);

  /* Lifecycle */
  bool init(const csi_config_t& cfg);
  void deinit();
  bool start();
  void stop();
  bool is_running();

  /* Callback registration (pass nullptr to unregister). The callback fires
   * at approximately 1 Hz (one per CSI_WINDOW_MS window). The features
   * pointer is owned by the caller and must be consumed synchronously. */
  void set_features_callback(FeaturesCallback cb);

  /* Main-loop pump; returns windows emitted this call (0 or 1 steady-state). */
  int process();

  /* Introspection */
  uint32_t get_caps();
  bool     get_stats(csi_stats_t* out);

  /* Conformance helper: heuristic scan of internal ring buffer for byte runs
   * that look like a MAC address. The structural guarantee is that the HAL
   * never copies info->mac/info->bssid; this is a defense-in-depth check
   * the integrator can call from a serial command. Always returns true; logs
   * via log_health() if it sees something suspicious. */
  bool conformance_check_no_mac_in_buffers();

}  /* namespace csi */

#endif  /* __cplusplus */

/* ──────────────────────────────────────────────────────────────────────────
 * PORTABLE C API SHIMS
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*csi_features_cb_t)(const csi_features_t* features, void* user_data);

bool     csi_init(const csi_config_t* config);
void     csi_deinit(void);
bool     csi_start(void);
void     csi_stop(void);
bool     csi_is_running(void);
void     csi_set_features_callback(csi_features_cb_t cb, void* user_data);
int      csi_process(void);
uint32_t csi_get_caps(void);
bool     csi_get_stats(csi_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_CSI_H */
