/*
 * SecuraCV Canary — WiFi CSI sensing (HAL + feature extractor)
 *
 * Backend: Espressif esp_wifi CSI callback API on ESP32-S3.
 *
 * Threading model:
 *   - ESP-IDF invokes csi_rx_cb() from the WiFi task (not an ISR).
 *     The callback scrubs identifiers, copies subcarriers into a single
 *     lock-free SPSC ring, and returns fast.
 *   - csi::process() runs on the main loop, drains whole frames into the
 *     feature aggregator, and — on a completed window — invokes the user
 *     callback synchronously.
 *
 * PRIVACY BARRIER:
 *   The ESP-IDF wifi_csi_info_t carries the source MAC, BSSID, frame header,
 *   and payload pointers. We touch ONLY rx_ctrl.rssi/channel/cwb and the
 *   subcarrier sample buffer. info->mac, info->dmac, info->hdr, info->payload
 *   are never read or copied.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_csi.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>

#include "log_level.h"
#include "securacv_witness.h"  /* log_health() */

extern "C" {
  #include <esp_wifi.h>
  #include <esp_err.h>
  #include <esp_system.h>
}
/* Shared with firmware/common/csi (on the include path via
 * -I../common/csi/src): the driver-config field names for both ESP-IDF
 * struct shapes, and the 52-tone L-LTF canonicalization every frame gets. */
#include "csi_idf_compat.h"
#include "csi_subcarriers.h"

/*
 * CSI compile-time gate. If a future arduino-esp32 release reorganizes the
 * wifi_csi_config_t struct, the wrapped ESP-IDF calls become a clean compile
 * + runtime no-op rather than a broken build.
 */
#if defined(CONFIG_ESP_WIFI_CSI_ENABLED) || !defined(CONFIG_IDF_TARGET)
  #define SECURACV_HAVE_CSI_API 1
#else
  #define SECURACV_HAVE_CSI_API 0
#endif

namespace csi {

/* ──────────────────────────────────────────────────────────────────────────
 * SECURITY PRIMITIVES
 * ────────────────────────────────────────────────────────────────────────── */

static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

/* ──────────────────────────────────────────────────────────────────────────
 * RING BUFFER (SPSC — WiFi task producer, main loop consumer)
 * ────────────────────────────────────────────────────────────────────────── */

struct CsiSlot {
  int8_t   rssi_dbm;
  uint8_t  bandwidth_code;          /* 0 = HT20, 1 = HT40 */
  uint8_t  channel;
  uint8_t  subcarrier_cnt;
  int8_t   iq[CSI_MAX_SUBCARRIERS * 2];  /* interleaved I,Q */
};

/* 16 slots × ~264 B = ~4 KB; sized for 20 Hz target so a 400 ms main-loop
 * stall can't lose a full window of CSI frames. */
static constexpr size_t RING_CAP = 16;
static CsiSlot s_ring[RING_CAP];
static std::atomic<uint32_t> s_head{0};
static std::atomic<uint32_t> s_tail{0};

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static bool s_running = false;
static bool s_start_pending = false;
static uint32_t s_start_retry_last_ms = 0;
static csi_config_t s_cfg = CSI_CONFIG_DEFAULT;
static FeaturesCallback s_cb = nullptr;

static std::atomic<uint32_t> s_frames_received{0};
static std::atomic<uint32_t> s_frames_dropped_rssi{0};
static std::atomic<uint32_t> s_frames_dropped_rate{0};
static std::atomic<uint32_t> s_frames_dropped_full{0};
static std::atomic<uint32_t> s_frames_dropped_short{0};  /* no L-LTF section */
static std::atomic<uint32_t> s_last_frame_ms{0};
static std::atomic<uint32_t> s_windows_emitted{0};
static std::atomic<uint32_t> s_windows_degraded{0};

static uint32_t s_window_start_ms = 0;
static uint32_t s_window_frames = 0;

static uint32_t s_rate_last_ms = 0;
static uint32_t s_rate_min_gap_ms = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * FEATURE EXTRACTOR — internal
 * ────────────────────────────────────────────────────────────────────────── */

namespace features {

  static constexpr size_t MAX_SC      = CSI_MAX_SUBCARRIERS;
  static constexpr size_t MAX_FRAMES  = 40;   /* 20 Hz × 1 s × 2 (headroom) */
  static constexpr size_t AMP_BANDS   = 8;
  static constexpr size_t DOP_BANDS   = 4;
  static constexpr size_t BREATH_BINS = 8;
  /* Cross-window breathing envelope ring: one mean-amplitude sample per
   * finalized window (1 Hz). 64 windows ≈ 6–28 breath cycles. Bins are
   * meaningless below ~2 cycles of the slowest target, hence the floor. */
  static constexpr size_t BREATH_RING        = 64;
  static constexpr size_t BREATH_MIN_WINDOWS = 24;
  /* Per-frame amplitude normalization target (AGC removal). */
  static constexpr int32_t AMP_NORM_MEAN = 64;

  /* Per-subcarrier per-frame amplitude history (variance), AGC-normalized. */
  static int16_t s_amp_hist[MAX_FRAMES][MAX_SC];
  static uint8_t s_frame_count = 0;
  static uint8_t s_sc_count = 0;

  /* Previous frame's I,Q for the CFO-corrected rotation estimator. */
  static int8_t s_prev_iq[MAX_SC * 2];
  static bool   s_have_prev = false;

  /* CFO-corrected per-band rotation: signed sum (direction) + magnitude
   * sum (detection; alias-proof against fast motion sign flips). */
  static int32_t  s_doppler_sum[DOP_BANDS];
  static uint32_t s_doppler_mag_sum[DOP_BANDS];

  /* Cross-window breathing envelope (survives per-window reset(); wiped
   * by reset_history() when sensing stops — privacy contract). */
  static int16_t  s_env_ring[BREATH_RING];
  static uint16_t s_env_ring_head = 0;
  static uint16_t s_env_ring_len  = 0;
  static int32_t  s_env_sum = 0;   /* per-window raw row-mean accumulator */

  /* RSSI running stats. */
  static int32_t  s_rssi_sum = 0;
  static int32_t  s_rssi_sq_sum = 0;
  static int8_t   s_rssi_max = -127;
  static int8_t   s_rssi_min = 0;
  static uint16_t s_rssi_n = 0;

  /* Last-seen channel and bandwidth code. */
  static uint8_t s_last_channel = 0;
  static uint8_t s_last_bw = 0;

  static inline int8_t clip_i8(int32_t v) {
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
  }

  /* Bit-by-bit integer sqrt; ~16 iterations for 32-bit, no FPU/libm. */
  static uint32_t isqrt_u32(uint32_t n) {
    uint32_t root = 0;
    uint32_t bit = (uint32_t)1 << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
      const uint32_t trial = root + bit;
      if (n >= trial) { n -= trial; root = (root >> 1) + bit; }
      else            { root >>= 1; }
      bit >>= 2;
    }
    return root;
  }

  /* True magnitude √(I²+Q²) — NOT the L1 |I|+|Q| shortcut, which wobbles
   * up to √2 per subcarrier under the ESP32's per-frame common rotation
   * and read as permanent fake motion. */
  static inline int16_t magnitude(int8_t I, int8_t Q) {
    const int32_t i32 = I, q32 = Q;
    return (int16_t)isqrt_u32((uint32_t)(i32 * i32 + q32 * q32));
  }

  static inline size_t amp_band_of(size_t sc, size_t sc_total) {
    if (sc_total == 0) return 0;
    size_t b = (sc * AMP_BANDS) / sc_total;
    return b >= AMP_BANDS ? AMP_BANDS - 1 : b;
  }
  static inline size_t dop_band_of(size_t sc, size_t sc_total) {
    if (sc_total == 0) return 0;
    size_t b = (sc * DOP_BANDS) / sc_total;
    return b >= DOP_BANDS ? DOP_BANDS - 1 : b;
  }

  static void reset() {
    s_frame_count = 0;
    s_sc_count = 0;
    s_have_prev = false;
    for (size_t i = 0; i < DOP_BANDS; i++) {
      s_doppler_sum[i] = 0;
      s_doppler_mag_sum[i] = 0;
    }
    s_env_sum = 0;
    s_rssi_sum = 0;
    s_rssi_sq_sum = 0;
    s_rssi_max = -127;
    s_rssi_min = 0;
    s_rssi_n = 0;
    s_last_channel = 0;
    s_last_bw = 0;
    /* Scrub history arrays — they previously held privacy-sensitive
     * per-subcarrier magnitudes. */
    memset(s_amp_hist, 0, sizeof(s_amp_hist));
    memset(s_prev_iq, 0, sizeof(s_prev_iq));
  }

  /* reset() plus the cross-window breathing envelope. Call when sensing
   * STOPS so no envelope shape survives a stop/mute boundary. */
  static void reset_history() {
    reset();
    memset(s_env_ring, 0, sizeof(s_env_ring));
    s_env_ring_head = 0;
    s_env_ring_len  = 0;
  }

  static void accumulate(const int8_t* iq, uint8_t subcarrier_cnt,
                         int8_t rssi_dbm, uint8_t channel, uint8_t bw_code) {
    if (iq == nullptr || subcarrier_cnt == 0) return;
    if (s_frame_count >= MAX_FRAMES) return;

    if (s_frame_count == 0) {
      s_sc_count = subcarrier_cnt > MAX_SC ? MAX_SC : subcarrier_cnt;
    }
    const size_t N = s_sc_count;

    /* 1. Amplitude history — AGC-normalized (mean pinned to AMP_NORM_MEAN
     * dividing by the high-precision raw_sum, rounded). The raw mean also
     * feeds the breathing envelope, which the normalized rows can't carry
     * (their mean is pinned by construction). */
    int16_t* row = s_amp_hist[s_frame_count];
    int32_t  raw_sum = 0;
    for (size_t k = 0; k < N; k++) {
      row[k] = magnitude(iq[2*k], iq[2*k + 1]);
      raw_sum += row[k];
    }
    s_env_sum += raw_sum / (int32_t)N;
    const int32_t denom = raw_sum > 0 ? raw_sum : 1;
    const int32_t numer_scale = AMP_NORM_MEAN * (int32_t)N;
    for (size_t k = 0; k < N; k++) {
      int32_t a = ((int32_t)row[k] * numer_scale + denom / 2) / denom;
      if (a > 0x7FFF) a = 0x7FFF;
      row[k] = (int16_t)a;
    }

    /* 2. CFO-corrected band rotation. Per band b: C_b = Σ z·conj(z_prev);
     * the all-band C_tot's angle is the frame pair's common phase offset
     * (CFO/PLL), so each band is scored by its rotation RELATIVE to it:
     * Im(C_b·conj(C_tot))/|C_tot|² — dimensionless, gain-invariant, and
     * exactly zero for a static channel under ANY per-frame offset. */
    if (s_have_prev) {
      int32_t dot_b[DOP_BANDS]   = {0};
      int32_t cross_b[DOP_BANDS] = {0};
      for (size_t k = 0; k < N; k++) {
        const int32_t I  = iq[2*k];
        const int32_t Q  = iq[2*k + 1];
        const int32_t Ip = s_prev_iq[2*k];
        const int32_t Qp = s_prev_iq[2*k + 1];
        const size_t  b  = dop_band_of(k, N);
        dot_b[b]   += I * Ip + Q * Qp;
        cross_b[b] += I * Qp - Q * Ip;
      }
      int64_t dot_t = 0, cross_t = 0;
      for (size_t b = 0; b < DOP_BANDS; b++) {
        dot_t   += dot_b[b];
        cross_t += cross_b[b];
      }
      const int64_t mag2 = dot_t * dot_t + cross_t * cross_t;
      /* Coherence floor: |C_tot| < 64 has no usable common reference. */
      if (mag2 >= 4096) {
        const int64_t norm = (mag2 >> 9) + 1;
        for (size_t b = 0; b < DOP_BANDS; b++) {
          const int64_t resid = (int64_t)cross_b[b] * dot_t
                              - (int64_t)dot_b[b]   * cross_t;
          int64_t c = resid / norm;
          if (c >  512) c =  512;
          if (c < -512) c = -512;
          s_doppler_sum[b]     += (int32_t)c;
          s_doppler_mag_sum[b] += (uint32_t)(c < 0 ? -c : c);
        }
      }
    }
    memcpy(s_prev_iq, iq, 2 * N);
    s_have_prev = true;

    /* 3. RSSI. */
    s_rssi_sum    += rssi_dbm;
    s_rssi_sq_sum += (int32_t)rssi_dbm * rssi_dbm;
    if (rssi_dbm > s_rssi_max) s_rssi_max = rssi_dbm;
    if (s_rssi_n == 0 || rssi_dbm < s_rssi_min) s_rssi_min = rssi_dbm;
    s_rssi_n++;

    /* 4. Channel/BW tracking. */
    s_last_channel = channel;
    s_last_bw = bw_code;

    s_frame_count++;
  }

  /* Per-subcarrier TEMPORAL variance, averaged within each band. The rows
   * are AGC-normalized, so what survives is genuine per-subcarrier change
   * over the window (motion) — not the static multipath profile and not
   * gain flicker. Empty room ≈ 0 in any environment. */
  static void compute_amp_variance(int8_t out[AMP_BANDS]) {
    if (s_frame_count < 2 || s_sc_count == 0) {
      for (size_t i = 0; i < AMP_BANDS; i++) out[i] = 0;
      return;
    }
    int32_t band_var_sum[AMP_BANDS] = {0};
    int32_t band_sc_n[AMP_BANDS]    = {0};
    const int32_t F = (int32_t)s_frame_count;
    for (size_t k = 0; k < s_sc_count; k++) {
      int32_t sum = 0;
      int64_t sq  = 0;   /* a ≤ 64·N ⇒ 40·a² brushes INT32_MAX at HT40 */
      for (size_t f = 0; f < s_frame_count; f++) {
        const int32_t a = s_amp_hist[f][k];
        sum += a;
        sq  += (int64_t)a * a;
      }
      /* Single-step variance — no truncated-mean bias. */
      int32_t var = (int32_t)((sq - ((int64_t)sum * sum) / F) / F);
      if (var < 0) var = 0;
      const size_t b = amp_band_of(k, s_sc_count);
      band_var_sum[b] += var;
      band_sc_n[b]++;
    }
    for (size_t i = 0; i < AMP_BANDS; i++) {
      if (band_sc_n[i] == 0) { out[i] = 0; continue; }
      /* Quantization noise ⇒ ≲2; walking ⇒ ≈160. >>2 lands int8 nicely. */
      out[i] = clip_i8((band_var_sum[i] / band_sc_n[i]) >> 2);
    }
  }

  /* Mean per-pair CFO-corrected rotation. Strength from the magnitude sum
   * (alias-proof); sign from the coherent sum (slow drift direction). */
  static void compute_doppler(int8_t out[DOP_BANDS]) {
    const int32_t n = s_frame_count > 1 ? (s_frame_count - 1) : 1;
    for (size_t i = 0; i < DOP_BANDS; i++) {
      const int32_t mag = (int32_t)(s_doppler_mag_sum[i] / (uint32_t)n);
      out[i] = clip_i8(s_doppler_sum[i] < 0 ? -mag : mag);
    }
  }

  /* Goertzel bank over the CROSS-WINDOW envelope ring. Breathing
   * (0.10–0.45 Hz, 6–27 BPM) cannot be resolved inside one 1 s window —
   * the previous in-window filter bank was eight near-identical DC taps
   * and its "dominant bin" was noise. Bin i ↔ 0.10+0.05·i Hz ↔ (6+3·i)
   * BPM, matching core_breathing's bpm_from_bin(). */
  static void compute_breathing(int8_t out[BREATH_BINS]) {
    for (size_t i = 0; i < BREATH_BINS; i++) out[i] = 0;
    const size_t n = s_env_ring_len;
    if (n < BREATH_MIN_WINDOWS) return;

    int32_t env[BREATH_RING];
    int32_t mean = 0;
    for (size_t j = 0; j < n; j++) {
      const size_t idx = (s_env_ring_head + BREATH_RING - n + j) % BREATH_RING;
      env[j] = s_env_ring[idx];
      mean  += env[j];
    }
    mean /= (int32_t)n;
    for (size_t j = 0; j < n; j++) env[j] -= mean;

    /* 2·cos(2π·f/fs) × 256 at fs = 1 window/s for f = 0.10 + 0.05·i Hz. */
    static const int16_t TWO_COS_OMEGA_Q8[BREATH_BINS] = {
      414, 301, 158, 0, -158, -301, -414, -487
    };

    for (size_t i = 0; i < BREATH_BINS; i++) {
      int64_t s_prev = 0, s_prev2 = 0;
      const int32_t coef_q8 = TWO_COS_OMEGA_Q8[i];
      for (size_t j = 0; j < n; j++) {
        const int64_t s = (int64_t)env[j] + ((coef_q8 * s_prev) >> 8) - s_prev2;
        s_prev2 = s_prev;
        s_prev  = s;
      }
      int64_t mag2 = s_prev * s_prev
                   + s_prev2 * s_prev2
                   - ((coef_q8 * s_prev * s_prev2) >> 8);
      if (mag2 < 0) mag2 = 0;
      /* Ring noise (σ≈1) ⇒ mag² ≈ 64 ⇒ 0; ±2-unit envelope ⇒ ≈40;
       * floored at 0 so |abs| consumers never mistake silence for signal. */
      int32_t log2_mag2 = 0;
      int64_t m = mag2;
      while (m > 1) { m >>= 1; log2_mag2++; }
      int32_t score = (log2_mag2 - 8) * 10;
      if (score < 0) score = 0;
      out[i] = clip_i8(score);
    }
  }

  static void finalize(csi_features_t* out, uint32_t frames_in_window) {
    if (out == nullptr) return;
    memset(out, 0, sizeof(*out));

    int8_t amp[AMP_BANDS]      = {0};
    int8_t dop[DOP_BANDS]      = {0};
    int8_t breath[BREATH_BINS] = {0};

    /* Append this window's mean raw envelope to the breathing ring before
     * running the filter bank. No-frame windows push nothing — a supply
     * gap must not inject a fake step into the spectrum. */
    if (s_frame_count > 0) {
      int32_t env = s_env_sum / (int32_t)s_frame_count;
      if (env > 0x7FFF) env = 0x7FFF;
      s_env_ring[s_env_ring_head] = (int16_t)env;
      s_env_ring_head = (uint16_t)((s_env_ring_head + 1) % BREATH_RING);
      if (s_env_ring_len < BREATH_RING) s_env_ring_len++;
    }

    compute_amp_variance(amp);
    compute_doppler(dop);
    compute_breathing(breath);

    int8_t rssi_mean = 0, rssi_std = 0;
    if (s_rssi_n > 0) {
      const int32_t m = s_rssi_sum / s_rssi_n;
      const int32_t v = (s_rssi_sq_sum / s_rssi_n) - m * m;
      rssi_mean = clip_i8(m);
      rssi_std  = clip_i8((int32_t)isqrt_u32((uint32_t)(v > 0 ? v : 0)));
    }

    size_t i = 0;
    for (size_t k = 0; k < AMP_BANDS;   k++) out->v[i++] = amp[k];     /*  0..7 */
    for (size_t k = 0; k < DOP_BANDS;   k++) out->v[i++] = dop[k];     /*  8..11 */
    for (size_t k = 0; k < BREATH_BINS; k++) out->v[i++] = breath[k];  /* 12..19 */
    out->v[i++] = rssi_mean;                                           /* 20 */
    out->v[i++] = rssi_std;                                            /* 21 */
    out->v[i++] = s_rssi_max;                                          /* 22 */
    out->v[i++] = s_rssi_min;                                          /* 23 */
    /* Per-window dropped-frame estimate: expected rate × window − actual. */
    const int32_t expected_frames =
        (int32_t)s_cfg.max_frame_rate_hz * (int32_t)CSI_WINDOW_MS / 1000;
    const int32_t dropped_estimate =
        expected_frames > (int32_t)s_frame_count
            ? expected_frames - (int32_t)s_frame_count : 0;

    out->v[i++] = clip_i8((int32_t)s_frame_count);                     /* 24 */
    out->v[i++] = clip_i8(dropped_estimate);                           /* 25 */
    out->v[i++] = clip_i8((int32_t)s_last_channel);                    /* 26 */
    out->v[i++] = clip_i8((int32_t)s_last_bw);                         /* 27 */
    /* v[28..31] remain zero — reserved. */

    out->frames_in_window = (uint16_t)(frames_in_window > 0xFFFF
                                       ? 0xFFFF : frames_in_window);

    /* 10-minute daily time bucket (0..143), per spec/canary_free_signals_v0.md
     * Invariant C and rf_presence's bucket convention. NOT the same width as
     * securacv_witness::time_bucket() (5 s, monotonic) — that helper is for
     * record sequencing, not for the daily-cycle privacy bucket the witness
     * record contract uses. When wall-clock time becomes available (GPS UTC),
     * a future change can replace millis() here without touching any caller. */
    const uint32_t now_ms = millis();
    out->time_bucket = (uint8_t)((now_ms / (10UL * 60UL * 1000UL)) % 144);

    out->caps_observed = CSI_CAP_HT20 | CSI_CAP_PHASE
                       | (s_last_bw == 1 ? CSI_CAP_HT40 : 0);
  }

}  /* namespace features */

/* ──────────────────────────────────────────────────────────────────────────
 * PRIVACY BARRIER: scrubbed metadata only — never info->mac/dmac/hdr/payload.
 * ────────────────────────────────────────────────────────────────────────── */

static inline void extract_scrubbed_metadata(const wifi_csi_info_t* info,
                                             CsiSlot* slot) {
  slot->rssi_dbm       = info->rx_ctrl.rssi;
  slot->channel        = info->rx_ctrl.channel;
  slot->bandwidth_code = (info->rx_ctrl.cwb == 1) ? 1 : 0;
  /* Explicitly do NOT touch info->mac / info->dmac / info->hdr / info->payload. */
}

/* ──────────────────────────────────────────────────────────────────────────
 * ESP-IDF CSI CALLBACK  (WiFi task context)
 * ────────────────────────────────────────────────────────────────────────── */

static void csi_rx_cb(void* /*ctx*/, wifi_csi_info_t* info) {
  if (info == nullptr || info->buf == nullptr || info->len == 0) return;

  const int8_t rssi = info->rx_ctrl.rssi;
  if (rssi < s_cfg.rssi_floor_dbm) {
    s_frames_dropped_rssi.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (s_rate_min_gap_ms > 0) {
    const uint32_t now = millis();
    if ((now - s_rate_last_ms) < s_rate_min_gap_ms) {
      s_frames_dropped_rate.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s_rate_last_ms = now;
  }

  const uint32_t head = s_head.load(std::memory_order_relaxed);
  const uint32_t tail = s_tail.load(std::memory_order_acquire);
  if ((head - tail) >= RING_CAP) {
    s_frames_dropped_full.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  CsiSlot* slot = &s_ring[head % RING_CAP];

  /* Wipe destination first so an early return can't leak prior frame bytes. */
  secure_wipe(slot->iq, sizeof(slot->iq));
  slot->subcarrier_cnt = 0;

  extract_scrubbed_metadata(info, slot);

  /* Canonicalize to the 52 L-LTF data+pilot tones in frequency order
   * (csi_subcarriers.h) so non-HT and HT frames never mix tone counts in a
   * window and null tones stay out of the AGC mean. A frame too short for
   * an L-LTF section is dropped unpublished. */
  const uint8_t tones = csi_lltf_select(info->buf, info->len,
                                        info->rx_ctrl.cwb == 1,
                                        info->first_word_invalid, slot->iq);
  if (tones == 0) {
    s_frames_dropped_short.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  slot->subcarrier_cnt = tones;

  s_head.store(head + 1, std::memory_order_release);
  s_frames_received.fetch_add(1, std::memory_order_relaxed);
  s_last_frame_ms.store(millis(), std::memory_order_relaxed);
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const csi_config_t& cfg) {
  if (s_initialized) return true;

  s_cfg = cfg;
  s_rate_min_gap_ms = (s_cfg.max_frame_rate_hz > 0)
                    ? (1000u / s_cfg.max_frame_rate_hz) : 0;

  secure_wipe(s_ring, sizeof(s_ring));
  s_head.store(0);
  s_tail.store(0);

  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "CSI HAL initialized", "scrub barrier active");

  /* Surface the channel/bandwidth_mhz advisory limitation explicitly so a
   * caller who set them doesn't silently get the AP/STA defaults. The
   * actual channel/bw used appears in feature-vector slots v[26]/v[27]. */
  if (s_cfg.channel != 0) {
    char detail[40];
    snprintf(detail, sizeof(detail),
             "requested ch=%u (advisory only)", (unsigned)s_cfg.channel);
    log_health(LOG_LEVEL_NOTICE, LOG_CAT_SENSOR,
               "CSI follows AP/STA channel; request ignored", detail);
  }
  return true;
}

void deinit() {
  if (!s_initialized) return;
  stop();
  secure_wipe(s_ring, sizeof(s_ring));
  s_cb = nullptr;
  s_initialized = false;
}

static int try_enable_csi_now() {
#if SECURACV_HAVE_CSI_API
  /* L-LTF on every frame + HT-LTF on HT frames, on whichever struct shape
   * this target's ESP-IDF exposes (csi_idf_compat.h). */
  wifi_csi_config_t cfg;
  csi_idf_fill_config(&cfg);

  esp_err_t err = esp_wifi_set_csi_config(&cfg);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    char detail[32];
    snprintf(detail, sizeof(detail), "err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "CSI config failed; sensing disabled", detail);
    return -1;
  }

  err = esp_wifi_set_csi_rx_cb(&csi_rx_cb, nullptr);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    char detail[32];
    snprintf(detail, sizeof(detail), "err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "CSI callback register failed", detail);
    return -1;
  }

  err = esp_wifi_set_csi(true);
  if (err == ESP_ERR_WIFI_NOT_STARTED) return 0;
  if (err != ESP_OK) {
    char detail[32];
    snprintf(detail, sizeof(detail), "err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "CSI enable failed", detail);
    return -1;
  }
  return 1;
#else
  (void)csi_rx_cb;
  return -1;
#endif
}

bool start() {
  if (!s_initialized) return false;
  /* Already running, or deferred-start already queued — no-op either way.
   * Without the s_start_pending check, repeat start() calls while WiFi is
   * still coming up would each emit a duplicate "deferred" log line. */
  if (s_running || s_start_pending) return true;

  const int r = try_enable_csi_now();
  if (r == 1) {
    s_window_start_ms = millis();
    s_window_frames = 0;
    s_running = true;
    s_start_pending = false;
    return true;
  }
  if (r == 0) {
    /* WiFi stack not up yet. Defer: process() retries each tick. */
    s_start_pending = true;
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
               "CSI start deferred — WiFi not yet running", nullptr);
    return true;
  }
#if !SECURACV_HAVE_CSI_API
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "CSI API not compiled into this WiFi driver build", nullptr);
#endif
  s_start_pending = false;
  return false;
}

void stop() {
  s_start_pending = false;
  if (!s_running) {
    features::reset_history();
    secure_wipe(s_ring, sizeof(s_ring));
    return;
  }
#if SECURACV_HAVE_CSI_API
  esp_wifi_set_csi(false);
  esp_wifi_set_csi_rx_cb(nullptr, nullptr);
#endif
  s_running = false;

  /* Drain ring + scrub extractor's static history so no residual CSI-derived
   * state leaks into a subsequent run. We're the consumer here, so we advance
   * tail to head — never the other way round (that would be the consumer
   * writing the producer's index, which races even with relaxed ordering).
   * After esp_wifi_set_csi(false) the producer task can no longer enqueue,
   * so this is well-defined. */
  s_tail.store(s_head.load(std::memory_order_acquire), std::memory_order_release);
  secure_wipe(s_ring, sizeof(s_ring));
  features::reset_history();
}

bool is_running() { return s_running; }

void set_features_callback(FeaturesCallback cb) { s_cb = cb; }

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN-LOOP PUMP
 * ────────────────────────────────────────────────────────────────────────── */

int process() {
  if (s_start_pending && !s_running) {
    const uint32_t now = millis();
    if ((now - s_start_retry_last_ms) >= 1000) {
      s_start_retry_last_ms = now;
      const int r = try_enable_csi_now();
      if (r == 1) {
        s_window_start_ms = now;
        s_window_frames = 0;
        s_running = true;
        s_start_pending = false;
        log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                   "CSI deferred start succeeded", nullptr);
      } else if (r == -1) {
        s_start_pending = false;  /* hard failure — give up */
      }
    }
  }

  if (!s_running) return 0;

  /* Drain available frames into the feature aggregator. */
  for (;;) {
    const uint32_t tail = s_tail.load(std::memory_order_relaxed);
    const uint32_t head = s_head.load(std::memory_order_acquire);
    if (tail == head) break;

    CsiSlot* slot = &s_ring[tail % RING_CAP];

    features::accumulate(slot->iq, slot->subcarrier_cnt,
                         slot->rssi_dbm, slot->channel,
                         slot->bandwidth_code);
    s_window_frames++;

    /* Scrub before advancing tail so the slot is zero if the producer
     * wraps around before we close the window. */
    secure_wipe(slot->iq, sizeof(slot->iq));
    slot->subcarrier_cnt = 0;

    s_tail.store(tail + 1, std::memory_order_release);
  }

  const uint32_t now_ms = millis();
  if ((now_ms - s_window_start_ms) < CSI_WINDOW_MS) return 0;

  /* A window that closes late stands for every whole window that elapsed
   * meanwhile; the feature layer holds the breathing envelope across the gap
   * so its one-sample-per-second time base survives a stall. */
  features::note_missed_windows((now_ms - s_window_start_ms) / CSI_WINDOW_MS - 1u);
  csi_features_t feats = {};
  features::finalize(&feats, s_window_frames);

  if (s_window_frames < (uint32_t)(s_cfg.max_frame_rate_hz / 2)) {
    s_windows_degraded.fetch_add(1, std::memory_order_relaxed);
  }
  s_windows_emitted.fetch_add(1, std::memory_order_relaxed);

  if (s_cb) s_cb(&feats);

  /* Scrub the local copy and reset the window. */
  secure_wipe(&feats, sizeof(feats));
  s_window_start_ms = now_ms;
  s_window_frames = 0;
  features::reset();
  return 1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ────────────────────────────────────────────────────────────────────────── */

uint32_t get_caps() {
  /* All ESP32 family CSI backends support HT20 with phase. HT40 is only
   * advertised on targets where the driver actually supports it (S3, S2),
   * so downstream fusion code doesn't take HT40 paths on a backend that
   * silently downgraded to 20 MHz. */
  uint32_t caps = CSI_CAP_HT20 | CSI_CAP_PHASE;
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  caps |= CSI_CAP_HT40;
#endif
  return caps;
}

bool get_stats(csi_stats_t* out) {
  if (!out) return false;
  out->frames_received     = s_frames_received.load(std::memory_order_relaxed);
  out->frames_dropped_rssi = s_frames_dropped_rssi.load(std::memory_order_relaxed);
  out->frames_dropped_rate = s_frames_dropped_rate.load(std::memory_order_relaxed);
  out->frames_dropped_full = s_frames_dropped_full.load(std::memory_order_relaxed);
  out->windows_emitted     = s_windows_emitted.load(std::memory_order_relaxed);
  out->windows_degraded    = s_windows_degraded.load(std::memory_order_relaxed);
  out->frames_dropped_short = s_frames_dropped_short.load(std::memory_order_relaxed);
  return true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * CONFORMANCE
 * ────────────────────────────────────────────────────────────────────────── */

bool conformance_check_no_mac_in_buffers() {
  /* Heuristic only — the structural guarantee is that we never copy
   * info->mac/info->bssid. This scans for 6+ consecutive non-zero bytes
   * with the locally-administered or multicast OUI bit set, which is
   * extremely unlikely in pure I/Q data clustered around zero. */
  for (size_t i = 0; i < RING_CAP; i++) {
    const int8_t* b = s_ring[i].iq;
    const size_t n = sizeof(s_ring[i].iq);
    size_t run = 0;
    for (size_t j = 0; j < n; j++) {
      if (b[j] != 0) {
        run++;
        if (run >= 6) {
          const uint8_t first = (uint8_t)b[j - 5];
          const bool lu_bit = (first & 0x02) != 0;
          const bool mc_bit = (first & 0x01) != 0;
          if (lu_bit || mc_bit) {
            char detail[64];
            snprintf(detail, sizeof(detail),
                     "slot=%u offset=%u", (unsigned)i, (unsigned)(j - 5));
            log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
                       "CSI conformance: suspicious byte run", detail);
          }
        }
      } else {
        run = 0;
      }
    }
  }
  return true;
}

}  /* namespace csi */

/* ──────────────────────────────────────────────────────────────────────────
 * C API SHIMS
 *
 * The trampoline is defined OUTSIDE extern "C" so its address has the
 * C++ function-pointer linkage that csi::set_features_callback expects.
 * ────────────────────────────────────────────────────────────────────────── */

namespace {
  csi_features_cb_t s_c_cb = nullptr;
  void* s_c_user = nullptr;

  void c_cb_trampoline(const csi_features_t* f) {
    if (s_c_cb) s_c_cb(f, s_c_user);
  }
}

extern "C" {

bool csi_init(const csi_config_t* config) {
  csi_config_t cfg = CSI_CONFIG_DEFAULT;
  if (config) {
    /* Copy every field verbatim. 0 is a valid setting for rssi_floor_dbm
     * (a caller that wants the default uses CSI_CONFIG_DEFAULT, not zero). */
    cfg.channel           = config->channel;
    cfg.bandwidth_mhz     = config->bandwidth_mhz;
    cfg.max_frame_rate_hz = config->max_frame_rate_hz;
    cfg.rssi_floor_dbm    = config->rssi_floor_dbm;
  }
  return csi::init(cfg);
}
void csi_deinit(void) { csi::deinit(); }
bool csi_start(void)  { return csi::start(); }
void csi_stop(void)   { csi::stop(); }
bool csi_is_running(void) { return csi::is_running(); }

void csi_set_features_callback(csi_features_cb_t cb, void* user_data) {
  s_c_cb = cb;
  s_c_user = user_data;
  csi::set_features_callback(cb ? c_cb_trampoline : nullptr);
}

int      csi_process(void)            { return csi::process(); }
uint32_t csi_get_caps(void)           { return csi::get_caps(); }
bool     csi_get_stats(csi_stats_t* o){ return csi::get_stats(o); }

}  /* extern "C" */

/* ──────────────────────────────────────────────────────────────────────────
 * csi_hal NAMESPACE SHIM
 *
 * The canary-wap build uses firmware/common/csi/src/csi_hal.cpp directly.
 * PIO cannot link that file (double-registration of esp_wifi_set_csi_rx_cb
 * would crash). This shim implements the csi_hal:: symbols the integration
 * layer needs, forwarding to the existing csi:: internals above and adding
 * the channel-lock + watchdog state that csi_hal.h declares.
 * ────────────────────────────────────────────────────────────────────────── */

#define SECURACV_CSI_TYPES_H
#include "csi_hal.h"

namespace csi_hal {

static uint8_t s_channel_lock = 0;
static bool    s_channel_lock_applied = false;

static uint32_t              s_wd_timeout_ms = WATCHDOG_DEFAULT_TIMEOUT_MS;
static WatchdogCallback      s_wd_cb = nullptr;
static uint32_t              s_wd_last_recovery_ms = 0;
static uint32_t              s_wd_recovery_count = 0;

bool init(const Config&) {
  return true;
}

void deinit() {
  csi::deinit();
}

bool start() {
  csi::s_last_frame_ms.store(0, std::memory_order_relaxed);
  s_wd_last_recovery_ms = 0;
  return csi::start();
}

void stop() {
  csi::stop();
}

bool is_running() {
  return csi::is_running();
}

void set_features_callback(FeaturesCallback cb) {
  csi::set_features_callback(cb);
}

int process() {
  int n = csi::process();

  /* Watchdog check — same logic as common/csi_hal.cpp. */
  if (s_wd_timeout_ms == 0) return n;
  if (!csi::is_running()) return n;

  const uint32_t last = csi::s_last_frame_ms.load(std::memory_order_relaxed);
  const uint32_t ref = (last == 0) ? csi::s_window_start_ms : last;
  const uint32_t now = millis();
  if ((int32_t)(now - ref) < (int32_t)s_wd_timeout_ms) return n;

  if (s_wd_last_recovery_ms != 0 &&
      (int32_t)(now - s_wd_last_recovery_ms) < (int32_t)WATCHDOG_RECOVERY_MIN_MS) {
    return n;
  }
  s_wd_last_recovery_ms = now;
  s_wd_recovery_count++;

  Serial.printf("[csi_hal] CSI silent for %ums; recovery attempt %u\n",
                (unsigned)(now - ref), (unsigned)s_wd_recovery_count);

  if (s_wd_cb) s_wd_cb(now - ref, s_wd_recovery_count);

#if SECURACV_HAVE_CSI_API
  esp_wifi_set_csi(false);
  esp_wifi_set_csi(true);
#endif

  return n;
}

bool get_stats(csi_stats_t* out) {
  return csi::get_stats(out);
}

bool set_channel_lock(uint8_t channel) {
  if (channel > 14) return false;
  s_channel_lock = channel;
  if (channel == 0) { s_channel_lock_applied = true; return true; }
  s_channel_lock_applied = false;
#if SECURACV_HAVE_CSI_API
  esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (err == ESP_OK) s_channel_lock_applied = true;
#endif
  return true;
}

uint8_t get_channel_lock() { return s_channel_lock; }

uint8_t get_observed_channel() {
  return csi::features::s_last_channel;
}

bool is_channel_in_sync() {
  if (s_channel_lock == 0) return true;
  return csi::features::s_last_channel == s_channel_lock;
}

void set_watchdog(uint32_t timeout_ms, WatchdogCallback cb) {
  s_wd_timeout_ms = timeout_ms;
  s_wd_cb = cb;
}

uint32_t get_watchdog_timeout_ms() { return s_wd_timeout_ms; }

uint32_t get_ms_since_last_frame() {
  const uint32_t last = csi::s_last_frame_ms.load(std::memory_order_relaxed);
  if (last == 0) return UINT32_MAX;
  return millis() - last;
}

uint32_t get_watchdog_recovery_count() { return s_wd_recovery_count; }

}  /* namespace csi_hal */
