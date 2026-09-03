/**
 * @file csi_types.h
 * @brief Privacy-preserving WiFi CSI types (canonical, firmware/common/csi/)
 *
 * Abstract types and constants for CSI-based environmental sensing. Used by
 * the rf_presence fusion layer to detect motion, breathing, and micro-activity
 * from subcarrier amplitude/phase dynamics without storing or transmitting
 * any device identifier.
 *
 * PRIVACY INVARIANTS (enforced by every concrete backend):
 *   1. The source MAC and BSSID are scrubbed from the raw frame before the
 *      frame enters any buffer that outlives a single callback.
 *   2. Only aggregated, non-identifying features cross this interface.
 *   3. No subcarrier sample is exported — only the compressed feature vector.
 *   4. Features are bucketed (int8) so fine-grained side channels are lost.
 *   5. No per-frame timestamp is exported; only a coarse time bucket.
 *
 * BUILD CONSUMERS:
 *   - PlatformIO: pulled in via -I PROJECT_DIR/../../common (set in
 *     envs/platformio/canary-wap.ini); .cpp picked up by the
 *     common-tree source filter that already exists in canary-wap.ini.
 *   - Arduino CLI: pass --libraries firmware/common (the canary-wap Makefile
 *     does this); library.properties in this directory makes it discoverable.
 *   - Arduino IDE: a libraries/csi symlink next to the canary-wap sketch
 *     points here, so the IDE auto-discovers the library.
 *
 * Threat model and design record: docs/csi_wifi_sensing_research.md (the
 * open-source landscape and what was adopted) and spec/invariants.md;
 * spec/canary_free_signals_v0.md covers the non-CSI free signals.
 */

#ifndef SECURACV_CSI_TYPES_H
#define SECURACV_CSI_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

/** Feature-vector width exported to the fusion head. int8, zero-mean. */
#define CSI_FEATURE_DIM 32

/** Maximum subcarriers a slot can hold. The HAL canonicalizes every frame to
 *  the 52 L-LTF data+pilot tones (csi_subcarriers.h), so in practice each
 *  frame carries exactly 52; the headroom is for a future HE-LTF path. */
#define CSI_MAX_SUBCARRIERS 128

/** Window length for one feature vector, in milliseconds. */
#define CSI_WINDOW_MS 1000

/** Noise floor on CSI RSSI; frames below this are dropped without buffering. */
#define CSI_RSSI_NOISE_FLOOR_DBM (-85)

/* ──────────────────────────────────────────────────────────────────────────
 * TYPES
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Backend capability flags reported by csi_get_caps(). The fusion layer
 * uses these to decide which features are meaningful.
 */
typedef enum {
  CSI_CAP_NONE          = 0,
  CSI_CAP_HT20          = 1 << 0,  /* 20 MHz, 52 usable subcarriers */
  CSI_CAP_HT40          = 1 << 1,  /* 40 MHz, 108 usable subcarriers */
  CSI_CAP_VHT80         = 1 << 2,  /* 80 MHz, 234 usable subcarriers */
  CSI_CAP_PHASE         = 1 << 3,  /* Backend exports I/Q (phase usable) */
  CSI_CAP_SOUNDING_11BF = 1 << 4,  /* 802.11bf-2025 sounding frames available */
} csi_cap_t;

/**
 * CSI feature vector. This is the only structure that crosses the privacy
 * barrier into rf_presence. All fields are int8 so bucketing is baked in.
 *
 * Layout (all values are mean-centered and scaled to fit int8):
 *   [ 0..7]  Subcarrier amplitude variance, 8 grouped bands (low → high freq)
 *   [ 8..11] Phase-difference Doppler, 4 bands (sign-aware, direction of motion)
 *   [12..19] Breathing/micro-motion FFT, 0.1–0.5 Hz spectrum in 8 bins
 *   [20..23] RSSI stats over the window: mean, std, max, min (all int8 dBm-derived)
 *   [24..27] Frame-rate health: frames_received, frames_dropped, channel, bandwidth_code
 *   [28..31] Reserved for future features (v2.1: C6 sounding, v2.2: phase unwrap)
 *
 * Total: 32 bytes. No timestamps. No identifiers. No raw samples.
 */
typedef struct {
  int8_t   v[CSI_FEATURE_DIM];
  uint16_t frames_in_window;   /* aggregate only; used for sanity checks */
  uint8_t  time_bucket;        /* 10-minute bucket (0..143), same as rf_presence */
  uint8_t  caps_observed;      /* csi_cap_t bitmask of what this window used */
} csi_features_t;

/**
 * The one breathing reducer. v[12..19] are eight Goertzel bins (0.10 +
 * 0.05·i Hz); a clean breath lands in ONE bin (≈40 at a ±2-unit envelope)
 * and leaves the rest near zero, so the meaningful scalar is the peak, not
 * the mean — the mean of a real breath is ≈5 and never clears a 30-point
 * threshold. Every consumer (core.presence, anomaly.baseline, the dashboard
 * stream) calls this so "breathing = 40" means the same thing everywhere.
 */
static inline uint8_t csi_breathing_peak(const int8_t* v) {
  uint8_t peak = 0;
  for (int i = 12; i < 20; ++i) {
    const int a = v[i] < 0 ? -(int)v[i] : (int)v[i];
    if (a > peak) peak = (uint8_t)(a > 127 ? 127 : a);
  }
  return peak;
}

/* Compile-time size check. Use C++11 static_assert when available
 * (Arduino sketch is C++); fall back to C11 _Static_assert otherwise. */
#ifdef __cplusplus
static_assert(sizeof(((csi_features_t*)0)->v) == CSI_FEATURE_DIM,
              "CSI feature vector must be exactly 32 int8 bytes");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(((csi_features_t*)0)->v) == CSI_FEATURE_DIM,
               "CSI feature vector must be exactly 32 int8 bytes");
#endif

/**
 * Configuration passed to csi_init().
 */
typedef struct {
  /** ADVISORY ONLY. CSI rides the WiFi driver's existing AP/STA mode, so the
   *  channel is whatever that mode negotiated; the HAL records this request
   *  for diagnostics and does NOT push it into esp_wifi_set_channel() (that
   *  would fight the AP/STA bring-up). Channel pinning for ESP-NOW probes is
   *  csi_hal::set_channel_lock(). 0 = follow STA. */
  uint8_t  channel;

  /** ADVISORY ONLY (see `channel`). Every frame is reduced to the 52 L-LTF
   *  tones regardless of PPDU bandwidth; the observed bandwidth appears in
   *  the feature vector at v[27]. */
  uint8_t  bandwidth_mhz;

  /** Maximum frames/sec to accept (rate-limits the callback; 0 = unlimited). */
  uint16_t max_frame_rate_hz;

  /** Drop frames whose RSSI is below this (dBm, negative). 0 = use default. */
  int8_t   rssi_floor_dbm;
} csi_config_t;

#define CSI_CONFIG_DEFAULT { \
    .channel = 0, \
    .bandwidth_mhz = 20, \
    .max_frame_rate_hz = 20, \
    .rssi_floor_dbm = CSI_RSSI_NOISE_FLOOR_DBM \
}

/**
 * Callback invoked once per completed window (default 1 Hz). The feature
 * vector is owned by the caller and must be consumed synchronously; it may
 * be reused after this call returns.
 */
typedef void (*csi_features_cb_t)(const csi_features_t* features, void* user_data);

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Initialize the CSI backend. Safe to call before WiFi is fully up; the
 * backend defers callback registration until WiFi is ready.
 *
 * @return true on success. false if the backend is unavailable on this
 *         hardware (in which case the fusion layer gracefully omits CSI).
 */
bool csi_init(const csi_config_t* config);

/** Tear down the backend and scrub any residual sample buffers. */
void csi_deinit(void);

/** Start feeding features. No-op if already started. */
bool csi_start(void);

/** Stop feeding features. Residual buffers are scrubbed. */
void csi_stop(void);

/** True if CSI is initialized and actively producing features. */
bool csi_is_running(void);

/* ──────────────────────────────────────────────────────────────────────────
 * DATA FLOW
 * ────────────────────────────────────────────────────────────────────────── */

/**
 * Register the feature-window callback. The callback fires at approximately
 * 1 Hz (one per CSI_WINDOW_MS window). Pass NULL to unregister.
 */
void csi_set_features_callback(csi_features_cb_t cb, void* user_data);

/**
 * Pump the CSI pipeline. Call from the main loop. The backend drains its
 * ring buffer, produces a feature vector if a window has completed, and
 * invokes the callback synchronously. Returns the number of windows emitted
 * (0 or 1 per call in steady state).
 */
int csi_process(void);

/* ──────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ────────────────────────────────────────────────────────────────────────── */

/** Capability flags supported by this backend at runtime. */
uint32_t csi_get_caps(void);

/**
 * Runtime statistics. Aggregate counters only; no per-frame detail.
 */
typedef struct {
  uint32_t frames_received;      /* Total CSI frames since init. */
  uint32_t frames_dropped_rssi;  /* Dropped for RSSI < floor. */
  uint32_t frames_dropped_rate;  /* Dropped by rate limiter. */
  uint32_t frames_dropped_full;  /* Dropped because ring was full. */
  uint32_t windows_emitted;      /* Feature vectors produced. */
  uint32_t windows_degraded;     /* Windows where frames_in_window < target. */
  uint32_t frames_dropped_short; /* Dropped: no L-LTF section (< 128 bytes). */
  /* Breathing-envelope cadence (appended; never reorder the fields above).
   * The feature layer resamples the envelope onto a fixed 1 Hz grid keyed
   * by each window's close timestamp; these say how far the loop's real
   * pace was from one window per second. */
  uint32_t windows_held;     /* Grid slots filled by holding the previous sample (late closes). */
  uint32_t windows_merged;   /* Closes averaged into an already-filled slot (early closes). */
  uint32_t window_period_ms; /* Mean close-to-close interval, ms; 0 until two timed closes. */
} csi_stats_t;

bool csi_get_stats(csi_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_TYPES_H */
