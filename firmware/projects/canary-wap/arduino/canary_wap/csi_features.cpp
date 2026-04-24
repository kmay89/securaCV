/*
 * SecuraCV Canary — CSI Feature Extractor — Implementation
 *
 * Allocation-free. All state is static. Targets ~200 µs per finalize() on
 * ESP32-S3 with 52 subcarriers × 20 frames/window.
 *
 * Design notes:
 *   • We compute amplitude as int16 magnitude (|I|+|Q|, not sqrt(I²+Q²))
 *     — an L1 approximation that is 20× faster and perfectly adequate for
 *     variance-over-time features.
 *   • Phase-difference Doppler is computed as sign-of-(I_k * Q_{k-1} −
 *     Q_k * I_{k-1}) which gives direction of rotation in the complex plane
 *     without any trig. Summed over the window, this produces a signed
 *     motion magnitude per band.
 *   • Breathing-band FFT: we do not run a full FFT — we run four Goertzel
 *     filters at 0.15, 0.25, 0.35, 0.45 Hz (and four harmonics up to 0.5 Hz)
 *     over the amplitude envelope. Per-filter cost is ~ N_frames multiplies.
 *   • All outputs are mean-centered and scaled so that "typical occupied
 *     room" maps to roughly v[i] ∈ [-60, 60], leaving headroom for unusual
 *     events to saturate toward ±127 without clipping behavior silently.
 */

#include "csi_features.h"
#include <Arduino.h>  /* for millis() */
#include <string.h>
#include <stdlib.h>

namespace csi_features {

/* ──────────────────────────────────────────────────────────────────────────
 * CONFIG
 * ────────────────────────────────────────────────────────────────────────── */

static constexpr size_t   MAX_SC = CSI_MAX_SUBCARRIERS;
/* We cap frames per window so our history buffers stay bounded. 20 Hz × 1 s
 * = 20 frames; double it for headroom. */
static constexpr size_t   MAX_FRAMES = 40;
/* Amplitude-band grouping: 8 bands across whatever subcarriers are present. */
static constexpr size_t   AMP_BANDS = 8;
/* Doppler bands: 4 regions of the subcarrier spectrum. */
static constexpr size_t   DOP_BANDS = 4;
/* Breathing filters: 8 Goertzel taps across 0.1–0.5 Hz. */
static constexpr size_t   BREATH_BINS = 8;
/* Breathing band center frequencies (Hz × 100, so we keep integer math). */
static constexpr uint16_t BREATH_FREQ_HZ_X100[BREATH_BINS] = {
  10, 15, 20, 25, 30, 35, 40, 50
};

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

/* Per-subcarrier per-frame amplitude history, used for variance + breathing. */
static int16_t s_amp_hist[MAX_FRAMES][MAX_SC];
static uint8_t s_frame_count = 0;
static uint8_t s_sc_count = 0;  /* locked to first frame's subcarrier count */

/* Previous frame's I and Q, used for the cross-product Doppler estimator. */
static int8_t  s_prev_iq[MAX_SC * 2];
static bool    s_have_prev = false;

/* Signed running sum of the Doppler cross-product, per band. */
static int32_t s_doppler_sum[DOP_BANDS];

/* RSSI running stats. */
static int32_t s_rssi_sum = 0;
static int32_t s_rssi_sq_sum = 0;
static int8_t  s_rssi_max = -127;
static int8_t  s_rssi_min = 0;
static uint16_t s_rssi_n = 0;

/* Last-seen channel and bandwidth code. */
static uint8_t s_last_channel = 0;
static uint8_t s_last_bw = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

static inline int16_t l1_magnitude(int8_t I, int8_t Q) {
  int16_t ai = I < 0 ? -(int16_t)I : (int16_t)I;
  int16_t aq = Q < 0 ? -(int16_t)Q : (int16_t)Q;
  return ai + aq;
}

/* Clip a 32-bit scaled value into int8 range. */
static inline int8_t clip_i8(int32_t v) {
  if (v >  127) return  127;
  if (v < -128) return -128;
  return (int8_t)v;
}

/* Compute which band (0..AMP_BANDS-1) this subcarrier belongs to. */
static inline size_t amp_band_of(size_t sc, size_t sc_total) {
  if (sc_total == 0) return 0;
  /* Integer quantization keeps us off the FPU. */
  size_t b = (sc * AMP_BANDS) / sc_total;
  return b >= AMP_BANDS ? AMP_BANDS - 1 : b;
}

static inline size_t dop_band_of(size_t sc, size_t sc_total) {
  if (sc_total == 0) return 0;
  size_t b = (sc * DOP_BANDS) / sc_total;
  return b >= DOP_BANDS ? DOP_BANDS - 1 : b;
}

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

void reset() {
  s_frame_count = 0;
  s_sc_count = 0;
  s_have_prev = false;
  for (size_t i = 0; i < DOP_BANDS; i++) s_doppler_sum[i] = 0;
  s_rssi_sum = 0;
  s_rssi_sq_sum = 0;
  s_rssi_max = -127;
  s_rssi_min = 0;
  s_rssi_n = 0;
  s_last_channel = 0;
  s_last_bw = 0;

  /* Scrub history arrays — they previously held scrubbed-but-still-privacy-
   * sensitive per-subcarrier magnitudes. */
  memset(s_amp_hist, 0, sizeof(s_amp_hist));
  memset(s_prev_iq, 0, sizeof(s_prev_iq));
}

void accumulate(const int8_t* iq, uint8_t subcarrier_cnt,
                int8_t rssi_dbm, uint8_t channel, uint8_t bw_code) {
  if (iq == nullptr || subcarrier_cnt == 0) return;
  if (s_frame_count >= MAX_FRAMES) return;

  /* Lock subcarrier count on first frame of the window. */
  if (s_frame_count == 0) {
    s_sc_count = subcarrier_cnt > MAX_SC ? MAX_SC : subcarrier_cnt;
  }
  const size_t N = s_sc_count;

  /* 1. Amplitude history. */
  int16_t* row = s_amp_hist[s_frame_count];
  for (size_t k = 0; k < N; k++) {
    row[k] = l1_magnitude(iq[2*k], iq[2*k + 1]);
  }

  /* 2. Doppler cross-product (requires a previous frame). */
  if (s_have_prev) {
    for (size_t k = 0; k < N; k++) {
      const int16_t I  = iq[2*k];
      const int16_t Q  = iq[2*k + 1];
      const int16_t Ip = s_prev_iq[2*k];
      const int16_t Qp = s_prev_iq[2*k + 1];
      /* Sign of I·Q' − Q·I' tells us rotation direction; magnitude is motion. */
      const int32_t cross = (int32_t)I * Qp - (int32_t)Q * Ip;
      s_doppler_sum[dop_band_of(k, N)] += cross;
    }
  }
  memcpy(s_prev_iq, iq, 2 * N);
  s_have_prev = true;

  /* 3. RSSI. */
  s_rssi_sum     += rssi_dbm;
  s_rssi_sq_sum  += (int32_t)rssi_dbm * rssi_dbm;
  if (rssi_dbm > s_rssi_max) s_rssi_max = rssi_dbm;
  if (s_rssi_n == 0 || rssi_dbm < s_rssi_min) s_rssi_min = rssi_dbm;
  s_rssi_n++;

  /* 4. Channel/BW tracking. */
  s_last_channel = channel;
  s_last_bw = bw_code;

  s_frame_count++;
}

/* Compute amplitude variance per band across all frames in the window. */
static void compute_amp_variance(int8_t out[AMP_BANDS]) {
  if (s_frame_count < 2 || s_sc_count == 0) {
    for (size_t i = 0; i < AMP_BANDS; i++) out[i] = 0;
    return;
  }

  int32_t band_sum[AMP_BANDS] = {0};
  int32_t band_sq[AMP_BANDS]  = {0};
  int32_t band_n[AMP_BANDS]   = {0};

  for (size_t f = 0; f < s_frame_count; f++) {
    for (size_t k = 0; k < s_sc_count; k++) {
      const size_t b = amp_band_of(k, s_sc_count);
      const int32_t a = s_amp_hist[f][k];
      band_sum[b] += a;
      band_sq[b]  += a * a;
      band_n[b]++;
    }
  }

  for (size_t i = 0; i < AMP_BANDS; i++) {
    if (band_n[i] <= 1) { out[i] = 0; continue; }
    const int32_t mean = band_sum[i] / band_n[i];
    const int32_t var  = (band_sq[i] / band_n[i]) - mean * mean;
    /* Scale: empty-room variance ~ a few hundred; human motion pushes
     * into the thousands. Divide by 16 to land in int8 range. */
    out[i] = clip_i8(var >> 4);
  }
}

/* Compute sign-aware Doppler per band, normalized by frame count. */
static void compute_doppler(int8_t out[DOP_BANDS]) {
  const int32_t n = s_frame_count > 1 ? (s_frame_count - 1) : 1;
  for (size_t i = 0; i < DOP_BANDS; i++) {
    /* Scale so that steady-state noise ~ ±5, fast motion saturates ±100. */
    out[i] = clip_i8(s_doppler_sum[i] / (n * 256));
  }
}

/*
 * Goertzel filter on the total-amplitude envelope across the window.
 * Envelope[f] = Σ_k s_amp_hist[f][k]. We run 8 filters at target freqs.
 *
 * Frame rate is assumed to be approximately MAX_FRAMES/1s. With 20 frames
 * per 1 s window, target frequencies 0.10..0.50 Hz map to
 * ω = 2π · (f_target / frame_rate). The integer coefficient we need for
 * Goertzel is cos(ω), which we pre-compute.
 */
static void compute_breathing(int8_t out[BREATH_BINS]) {
  if (s_frame_count < 4 || s_sc_count == 0) {
    for (size_t i = 0; i < BREATH_BINS; i++) out[i] = 0;
    return;
  }

  /* Envelope per frame (int16 is fine; max is MAX_SC * 255 ≈ 32k). */
  int16_t env[MAX_FRAMES];
  int32_t env_mean = 0;
  for (size_t f = 0; f < s_frame_count; f++) {
    int32_t sum = 0;
    for (size_t k = 0; k < s_sc_count; k++) sum += s_amp_hist[f][k];
    env[f] = (int16_t)(sum / (int32_t)s_sc_count);
    env_mean += env[f];
  }
  env_mean /= s_frame_count;

  /* DC-subtracted envelope — Goertzel works best on zero-mean input. */
  for (size_t f = 0; f < s_frame_count; f++) {
    env[f] = (int16_t)(env[f] - env_mean);
  }

  /* Pre-computed 2·cos(ω) × 256 for each target frequency, assuming
   * ~20 Hz frame rate. If the actual frame rate is lower the bins just
   * shift slightly; still useful as a bank of adjacent-frequency filters. */
  static const int16_t TWO_COS_OMEGA_Q8[BREATH_BINS] = {
    /* 0.15 Hz */ 511, /* 0.225 */ 511, /* 0.30 */ 510, /* 0.375 */ 510,
    /* 0.45  */  509, /* 0.525 */ 508, /* 0.60 */ 507, /* 0.75 */ 504
  };

  for (size_t i = 0; i < BREATH_BINS; i++) {
    int32_t s_prev = 0, s_prev2 = 0;
    const int32_t coef_q8 = TWO_COS_OMEGA_Q8[i];
    for (size_t f = 0; f < s_frame_count; f++) {
      const int32_t s = (int32_t)env[f] + ((coef_q8 * s_prev) >> 8) - s_prev2;
      s_prev2 = s_prev;
      s_prev  = s;
    }
    /* Magnitude squared ≈ s_prev² + s_prev2² − (coef · s_prev · s_prev2). */
    const int64_t mag2 = (int64_t)s_prev * s_prev
                       + (int64_t)s_prev2 * s_prev2
                       - (((int64_t)coef_q8 * s_prev * s_prev2) >> 8);
    /* Scale: take integer log2-ish by counting leading zeros. */
    int32_t scaled = 0;
    int64_t m = mag2;
    while (m > 0) { m >>= 2; scaled++; }
    out[i] = clip_i8(scaled - 12);  /* −12 centers on "quiet room" */
    (void)BREATH_FREQ_HZ_X100;       /* reserved for future precise tuning */
  }
}

/* Current per-window frame count (for csi_hal introspection). */
uint32_t current_frame_count() { return s_frame_count; }

void finalize(csi_features_t* out, uint32_t frames_in_window) {
  if (out == nullptr) return;
  memset(out, 0, sizeof(*out));

  int8_t amp[AMP_BANDS]    = {0};
  int8_t dop[DOP_BANDS]    = {0};
  int8_t breath[BREATH_BINS] = {0};

  compute_amp_variance(amp);
  compute_doppler(dop);
  compute_breathing(breath);

  /* RSSI stats. */
  int8_t rssi_mean = 0, rssi_std = 0;
  if (s_rssi_n > 0) {
    const int32_t m = s_rssi_sum / s_rssi_n;
    const int32_t v = (s_rssi_sq_sum / s_rssi_n) - m * m;
    rssi_mean = clip_i8(m);
    /* Cheap std approximation: sqrt via integer shifts. */
    int32_t s = 0;
    int32_t x = v > 0 ? v : 0;
    while (x > 1) { x >>= 2; s++; }
    rssi_std = clip_i8(s << 2);
  }

  /* Lay out the 32-dim vector. */
  size_t i = 0;
  for (size_t k = 0; k < AMP_BANDS;    k++) out->v[i++] = amp[k];     /* 0..7 */
  for (size_t k = 0; k < DOP_BANDS;    k++) out->v[i++] = dop[k];     /* 8..11 */
  for (size_t k = 0; k < BREATH_BINS;  k++) out->v[i++] = breath[k];  /* 12..19 */
  out->v[i++] = rssi_mean;                                            /* 20 */
  out->v[i++] = rssi_std;                                             /* 21 */
  out->v[i++] = s_rssi_max;                                           /* 22 */
  out->v[i++] = s_rssi_min;                                           /* 23 */
  out->v[i++] = clip_i8((int32_t)s_frame_count);                      /* 24 */
  out->v[i++] = 0; /* dropped_estimate — filled by caller if known */ /* 25 */
  out->v[i++] = clip_i8((int32_t)s_last_channel);                     /* 26 */
  out->v[i++] = clip_i8((int32_t)s_last_bw);                          /* 27 */
  /* v[28..31] remain zero — reserved. */

  out->frames_in_window = (uint16_t)(frames_in_window > 0xFFFF ? 0xFFFF : frames_in_window);

  /* Time bucket matches rf_presence: 10-minute buckets, 0..143 per day. */
  const uint32_t now_ms = millis();
  out->time_bucket = (uint8_t)((now_ms / (10UL * 60UL * 1000UL)) % 144);

  /* caps_observed: we used HT-LTF at minimum. If frames had the HT40 bw
   * code we saw 40 MHz sensing in this window. */
  out->caps_observed = CSI_CAP_HT20 | CSI_CAP_PHASE
                     | (s_last_bw == 1 ? CSI_CAP_HT40 : 0);
}

}  /* namespace csi_features */
