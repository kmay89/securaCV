/*
 * SecuraCV Canary — CSI Feature Extractor — Implementation
 *
 * Allocation-free. All state is static. Targets ~200 µs per finalize() on
 * ESP32-S3 with 52 subcarriers × 20 frames/window.
 *
 * Design notes:
 *   • Amplitude is the TRUE magnitude √(I²+Q²) via integer sqrt. The L1
 *     |I|+|Q| shortcut is phase-dependent (up to √2 wobble under the
 *     per-frame common rotation) and poisoned the temporal variance.
 *   • AGC normalization: every frame's amplitude row is rescaled so its
 *     mean is AMP_NORM_MEAN before anything else looks at it. The ESP32
 *     front-end re-gains per packet; without this, gain flicker reads as
 *     whole-room "motion" and a strong static channel drowns a weak
 *     moving one.
 *   • Motion (amplitude path) is PER-SUBCARRIER TEMPORAL variance,
 *     averaged within each of 8 frequency bands. Pooling subcarriers and
 *     time into one variance (the v0 approach) mostly measured the static
 *     multipath profile of the room — a rich empty channel scored higher
 *     than a person moving in a flat one. Temporal variance isolates
 *     change, which is what motion is.
 *   • Motion (phase path) is a CFO-CORRECTED band rotation. Each ESP32
 *     CSI frame carries an arbitrary common phase offset (PLL/CFO), so a
 *     raw frame-to-frame cross product is offset noise, not Doppler. Per
 *     frame pair we estimate the common rotation from the all-subcarrier
 *     correlation C_tot = Σ z·conj(z_prev), then score each band by
 *     Im(C_band·conj(C_tot))/|C_tot| — the band's rotation RELATIVE to
 *     the common one. A static channel cancels exactly (any CFO); only
 *     genuine per-band channel change survives.
 *   • Breathing (0.10–0.45 Hz) is measured where it physically lives: on
 *     a cross-WINDOW envelope ring (last BREATH_RING window means, one
 *     per second), not inside a single 1 s window — a 1 s record cannot
 *     resolve 0.2 Hz at all. Eight Goertzel bins at 0.10 + 0.05·i Hz over
 *     that ring; bin i maps to (6 + 3·i) BPM, matching core_breathing.
 *   • All outputs are scaled so an empty room maps to ≈0 and typical
 *     occupied activity to roughly v[i] ∈ [-60, 60], leaving headroom for
 *     unusual events to saturate toward ±127 without silently clipping.
 */

#include "csi_features.h"
#include "csi_mem.h"
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
/* Breathing filters: 8 Goertzel taps across 0.10–0.45 Hz (6–27 BPM). */
static constexpr size_t   BREATH_BINS = 8;
/* Breathing band center frequencies (Hz × 100, so we keep integer math).
 * Bin i = 0.10 + 0.05·i Hz — core_breathing's bpm_from_bin() mirrors this. */
static constexpr uint16_t BREATH_FREQ_HZ_X100[BREATH_BINS] = {
  10, 15, 20, 25, 30, 35, 40, 45
};
/* Cross-window envelope ring: one mean-amplitude sample per finalized
 * window (1 Hz cadence). 64 windows ≈ 64 s ≈ 6–28 breath cycles. */
static constexpr size_t   BREATH_RING = 64;
/* Minimum ring fill before the breathing bins report anything — below
 * ~2 cycles of the slowest target the spectrum is meaningless. */
static constexpr size_t   BREATH_MIN_WINDOWS = 24;
/* Per-frame amplitude normalization target (AGC removal). */
static constexpr int32_t  AMP_NORM_MEAN = 64;

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

/* Per-subcarrier per-frame amplitude history, used for variance + breathing. */
/* PSRAM-resident (csi_mem.h): at 40x128 int16 this is 10 KB — the single
 * biggest CSI static — and it is only touched from task context, so it has
 * no business in the 320 KB internal DRAM bank the BLE stack competes for.
 * Allocated on first reset(); NULL means allocation failed and the feature
 * pipeline stays disabled (accumulate() refuses frames, so no consumer ever
 * dereferences it). */
static int16_t (*s_amp_hist)[MAX_SC] = nullptr;
static constexpr size_t AMP_HIST_BYTES =
    MAX_FRAMES * MAX_SC * sizeof(int16_t);
static uint8_t s_frame_count = 0;
static uint8_t s_sc_count = 0;  /* locked to first frame's subcarrier count */

/* Previous frame's I and Q, used for the CFO-corrected rotation estimator. */
static int8_t  s_prev_iq[MAX_SC * 2];
static bool    s_have_prev = false;

/* CFO-corrected per-band rotation: signed sum (direction) and magnitude
 * sum (detection). Fast motion aliases the per-pair rotation past ±π and
 * the SIGNED sum self-cancels — the magnitude sum doesn't, so detection
 * strength comes from the magnitudes and only the sign of the signed sum
 * is kept (slow coherent drift still reports direction). */
static int32_t  s_doppler_sum[DOP_BANDS];
static uint32_t s_doppler_mag_sum[DOP_BANDS];

/* Cross-window breathing envelope. One raw (pre-normalization) mean
 * amplitude per finalized window, most recent BREATH_RING entries.
 * Survives the per-window reset(); wiped only by reset_history() (called
 * from csi_hal stop/deinit) so no envelope shape outlives a sensing stop.
 * 128 B — small enough to stay a static, no csi_large_calloc needed. */
static int16_t s_env_ring[BREATH_RING];
static uint16_t s_env_ring_head = 0;   /* next write slot */
static uint16_t s_env_ring_len  = 0;   /* valid entries (≤ BREATH_RING) */
/* Sum of per-frame raw row means within the current window (envelope). */
static int32_t s_env_sum = 0;

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

/* Clip a 32-bit scaled value into int8 range. */
static inline int8_t clip_i8(int32_t v) {
  if (v >  127) return  127;
  if (v < -128) return -128;
  return (int8_t)v;
}

/* Integer square root via bit-by-bit method. Fixed-time per input size;
 * ~16 iterations for 32-bit inputs. No FPU, no libm. */
static uint32_t isqrt_u32(uint32_t n) {
  uint32_t root = 0;
  uint32_t bit = (uint32_t)1 << 30;  /* highest even bit ≤ 2^31 */
  while (bit > n) bit >>= 2;
  while (bit) {
    const uint32_t trial = root + bit;
    if (n >= trial) {
      n -= trial;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

/* True magnitude √(I²+Q²). NOT the L1 |I|+|Q| shortcut: the ESP32's
 * per-frame common phase rotation swings an L1 "amplitude" by up to
 * √2 per subcarrier even for a perfectly static channel, and that
 * phase-dependent wobble survives per-frame mean normalization (each
 * subcarrier sits at a different phase) — it read as permanent fake
 * motion. The true magnitude is rotation-invariant. ~1k isqrts/s at
 * 20 Hz × 52 subcarriers — negligible. */
static inline int16_t magnitude(int8_t I, int8_t Q) {
  const int32_t i32 = I, q32 = Q;
  return (int16_t)isqrt_u32((uint32_t)(i32 * i32 + q32 * q32));
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

  /* Scrub history arrays — they previously held scrubbed-but-still-privacy-
   * sensitive per-subcarrier magnitudes. */
  if (s_amp_hist == nullptr) s_amp_hist =
      (int16_t (*)[MAX_SC])csi_large_calloc(AMP_HIST_BYTES);
  if (s_amp_hist != nullptr) memset(s_amp_hist, 0, AMP_HIST_BYTES);
  memset(s_prev_iq, 0, sizeof(s_prev_iq));
}

void reset_history() {
  /* Everything reset() clears, PLUS the cross-window breathing envelope.
   * reset() runs at every window close, so the ring must NOT live there —
   * but when sensing stops (mute, deinit, watchdog restart) no envelope
   * shape may survive into the next run. Same privacy contract as the
   * amplitude history. */
  reset();
  memset(s_env_ring, 0, sizeof(s_env_ring));
  s_env_ring_head = 0;
  s_env_ring_len  = 0;
}

void accumulate(const int8_t* iq, uint8_t subcarrier_cnt,
                int8_t rssi_dbm, uint8_t channel, uint8_t bw_code) {
  if (iq == nullptr || subcarrier_cnt == 0) return;
  /* Lazy allocation HERE, not only in reset(): on a cold start the HAL
   * begins draining frames without a prior reset() (reset runs at stop()
   * and at window close), so hanging the buffer's existence on reset()
   * would zero the entire first window. NULL after a failed attempt
   * disables the pipeline (frames refused, so no consumer dereferences). */
  if (s_amp_hist == nullptr) {
    s_amp_hist = (int16_t (*)[MAX_SC])csi_large_calloc(AMP_HIST_BYTES);
    if (s_amp_hist == nullptr) return;
  }
  if (s_frame_count >= MAX_FRAMES) return;

  /* Lock subcarrier count on first frame of the window. */
  if (s_frame_count == 0) {
    s_sc_count = subcarrier_cnt > MAX_SC ? MAX_SC : subcarrier_cnt;
  }
  const size_t N = s_sc_count;

  /* 1. Amplitude history — AGC-normalized.
   * First pass: raw L1 magnitudes + their mean (the per-frame gain).
   * The raw mean also feeds the breathing envelope (breathing modulates
   * total received power; the normalized rows can't carry it because
   * their mean is pinned to AMP_NORM_MEAN by construction). */
  int16_t* row = s_amp_hist[s_frame_count];
  int32_t  raw_sum = 0;
  for (size_t k = 0; k < N; k++) {
    row[k] = magnitude(iq[2*k], iq[2*k + 1]);
    raw_sum += row[k];
  }
  const int32_t raw_mean = raw_sum / (int32_t)N;
  s_env_sum += raw_mean;
  /* Second pass: rescale so the row mean is AMP_NORM_MEAN. Per-packet
   * front-end gain changes now cancel instead of reading as motion.
   * Divide by the full raw_sum (not the truncated per-frame mean): the
   * mean's ±0.5 truncation is a ±2–4 % correlated wobble that reads as
   * fake variance on every subcarrier; raw_sum carries N× the precision.
   * Round the division for the same reason. */
  const int32_t denom = raw_sum > 0 ? raw_sum : 1;
  const int32_t numer_scale = AMP_NORM_MEAN * (int32_t)N;
  for (size_t k = 0; k < N; k++) {
    int32_t a = ((int32_t)row[k] * numer_scale + denom / 2) / denom;
    if (a > 0x7FFF) a = 0x7FFF;
    row[k] = (int16_t)a;
  }

  /* 2. CFO-corrected band rotation (requires a previous frame).
   * Per band b: C_b = Σ_k z_k·conj(z_prev,k)  (dot = Re, cross = Im).
   * The all-band total C_tot is dominated by the static paths, so its
   * angle IS the frame pair's common phase offset (CFO/PLL). The band's
   * motion signal is its rotation relative to that common angle:
   *   Im(C_b · conj(C_tot)) = cross_b·dot_tot − dot_b·cross_tot,
   * normalized by |C_tot|. Static channel ⇒ every band parallel to the
   * total ⇒ exact zero for ANY per-frame offset. */
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
    /* Normalize by |C_tot|² so the score is DIMENSIONLESS — RX gain
     * (which scales every C by gain²) cancels exactly, making the same
     * motion score the same number at −40 dBm and −75 dBm:
     *   resid/|C_tot|² = (|C_b|/|C_tot|)·sin(Δθ_b)  ∈ [−¼, ¼] per band
     * (band share ≈ ¼ of total). The <<9 puts a full-rotation band at
     * ≈128 per pair; finalize divides by pair count, so a sustained
     * full-window rotation saturates and typical walking lands 30–90. */
    const int64_t mag2 = dot_t * dot_t + cross_t * cross_t;  /* ≤ ~5.7e12 */
    /* Coherence floor: with |C_tot| < 64 there is no usable common
     * reference (deep fade, or bands canceling) and the normalized
     * ratio blows up — skip the pair instead of injecting garbage. */
    if (mag2 >= 4096) {
      const int64_t norm = (mag2 >> 9) + 1;
      for (size_t b = 0; b < DOP_BANDS; b++) {
        const int64_t resid = (int64_t)cross_b[b] * dot_t
                            - (int64_t)dot_b[b]   * cross_t;
        int64_t c = resid / norm;
        /* Physically meaningful max is ≈128 (full rotation × ¼ share);
         * clamp at ±512 so a pathological pair can't swamp the window. */
        if (c >  512) c =  512;
        if (c < -512) c = -512;
        s_doppler_sum[b] += (int32_t)c;
        s_doppler_mag_sum[b] += (uint32_t)(c < 0 ? -c : c);
      }
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

/* Per-subcarrier TEMPORAL variance, averaged within each band.
 * The amplitude rows are AGC-normalized (mean pinned to AMP_NORM_MEAN),
 * so what survives is genuine per-subcarrier change over the window —
 * i.e. motion — not the static multipath profile and not gain flicker.
 * An empty room lands ≈0 in ANY environment; that's what makes the
 * downstream thresholds portable across homes. */
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
    int64_t sq  = 0;        /* a can reach 64·N (≈7.3k at HT40's 114
                             * subcarriers) when one subcarrier dominates;
                             * 40·a² then brushes INT32_MAX — int64 keeps
                             * the accumulation defined at any N. */
    for (size_t f = 0; f < s_frame_count; f++) {
      const int32_t a = s_amp_hist[f][k];
      sum += a;
      sq  += (int64_t)a * a;
    }
    /* Single-step variance (no truncated-mean bias — same lesson as the
     * acoustic RMS): (Σa² − (Σa)²/F) / F. */
    int32_t var = (int32_t)((sq - ((int64_t)sum * sum) / F) / F);
    if (var < 0) var = 0;
    const size_t b = amp_band_of(k, s_sc_count);
    band_var_sum[b] += var;
    band_sc_n[b]++;
  }

  for (size_t i = 0; i < AMP_BANDS; i++) {
    if (band_sc_n[i] == 0) { out[i] = 0; continue; }
    const int32_t mean_var = band_var_sum[i] / band_sc_n[i];
    /* Normalized units: quantization noise ⇒ var ≲ 2; a person moving
     * modulates subcarriers ±20 % of AMP_NORM_MEAN ⇒ var ≈ 160. >>2
     * lands walking at ≈40 with saturation headroom. */
    out[i] = clip_i8(mean_var >> 2);
  }
}

/* CFO-corrected band rotation, averaged per frame pair. The per-pair
 * contributions are already dimensionless (normalized by |C_tot|², see
 * accumulate()). Strength = mean |contribution| (alias-proof); sign =
 * sign of the coherent sum (meaningful only for slow drift). */
static void compute_doppler(int8_t out[DOP_BANDS]) {
  const int32_t n = s_frame_count > 1 ? (s_frame_count - 1) : 1;
  for (size_t i = 0; i < DOP_BANDS; i++) {
    const int32_t mag = (int32_t)(s_doppler_mag_sum[i] / (uint32_t)n);
    out[i] = clip_i8(s_doppler_sum[i] < 0 ? -mag : mag);
  }
}

/*
 * Goertzel filter bank over the CROSS-WINDOW envelope ring.
 *
 * Breathing (0.10–0.45 Hz, 6–27 BPM) is far too slow to resolve inside a
 * single 1 s window — the v0 code that tried had eight numerically
 * near-identical DC filters and its "dominant bin" was noise. The
 * envelope ring collects one mean-amplitude sample per finalized window
 * (1 Hz sample rate), so 64 entries span ~64 s ≈ 6–28 breath cycles and
 * the eight target frequencies are genuinely distinct bins.
 */
static void compute_breathing(int8_t out[BREATH_BINS]) {
  for (size_t i = 0; i < BREATH_BINS; i++) out[i] = 0;
  const size_t n = s_env_ring_len;
  if (n < BREATH_MIN_WINDOWS) return;

  /* Chronological, DC-removed copy of the ring. */
  int32_t env[BREATH_RING];
  int32_t mean = 0;
  for (size_t j = 0; j < n; j++) {
    const size_t idx = (s_env_ring_head + BREATH_RING - n + j) % BREATH_RING;
    env[j] = s_env_ring[idx];
    mean  += env[j];
  }
  mean /= (int32_t)n;
  for (size_t j = 0; j < n; j++) env[j] -= mean;

  /* 2·cos(2π·f/fs) × 256 at fs = 1 window/s for f = 0.10 + 0.05·i Hz.
   * These are real, distinct coefficients — compare the v0 table where
   * every entry was ≈511 (all bins ≈ DC at the 20 Hz frame rate). */
  static const int16_t TWO_COS_OMEGA_Q8[BREATH_BINS] = {
    /* 0.10 Hz */  414, /* 0.15 */  301, /* 0.20 */  158, /* 0.25 */    0,
    /* 0.30 Hz */ -158, /* 0.35 */ -301, /* 0.40 */ -414, /* 0.45 */ -487
  };

  for (size_t i = 0; i < BREATH_BINS; i++) {
    int64_t s_prev = 0, s_prev2 = 0;
    const int32_t coef_q8 = TWO_COS_OMEGA_Q8[i];
    for (size_t j = 0; j < n; j++) {
      const int64_t s = (int64_t)env[j] + ((coef_q8 * s_prev) >> 8) - s_prev2;
      s_prev2 = s_prev;
      s_prev  = s;
    }
    /* Magnitude squared ≈ s_prev² + s_prev2² − (coef · s_prev · s_prev2). */
    int64_t mag2 = s_prev * s_prev
                 + s_prev2 * s_prev2
                 - ((coef_q8 * s_prev * s_prev2) >> 8);
    if (mag2 < 0) mag2 = 0;
    /* Log2 → int8, floored at 0 so "no periodic energy" reads as 0, not
     * a large negative that |abs| consumers would mistake for signal.
     * Incoherent ring noise (σ≈1) ⇒ mag² ≈ n ≈ 64 ⇒ log2 6 ⇒ 0. A ±2-unit
     * breathing envelope over a full ring ⇒ mag ≈ 2·32 ⇒ mag² ≈ 4e3 ⇒
     * log2 ≈ 12 ⇒ ≈ 40 (clears core_breathing's lock threshold of 30);
     * ±10 units ⇒ log2 ≈ 16.6 ⇒ ≈ 86; saturation at ±40 units. */
    int32_t log2_mag2 = 0;
    int64_t m = mag2;
    while (m > 1) { m >>= 1; log2_mag2++; }
    int32_t score = (log2_mag2 - 8) * 10;
    if (score < 0) score = 0;
    out[i] = clip_i8(score);
    (void)BREATH_FREQ_HZ_X100;       /* documented mapping; kept in sync */
  }
}

/* Current per-window frame count (for csi_hal introspection). */
uint32_t current_frame_count() { return s_frame_count; }

size_t envelope_len() { return s_env_ring_len; }

void note_missed_windows(uint32_t missed) {
  if (missed == 0 || s_env_ring_len == 0) return;   /* nothing to hold */
  if (missed > BREATH_RING) missed = BREATH_RING;
  const size_t last = (s_env_ring_head + BREATH_RING - 1) % BREATH_RING;
  const int16_t held = s_env_ring[last];
  for (uint32_t i = 0; i < missed; i++) {
    s_env_ring[s_env_ring_head] = held;
    s_env_ring_head = (uint16_t)((s_env_ring_head + 1) % BREATH_RING);
    if (s_env_ring_len < BREATH_RING) s_env_ring_len++;
  }
}

void finalize(csi_features_t* out, uint32_t frames_in_window) {
  if (out == nullptr) return;
  memset(out, 0, sizeof(*out));

  int8_t amp[AMP_BANDS]    = {0};
  int8_t dop[DOP_BANDS]    = {0};
  int8_t breath[BREATH_BINS] = {0};

  /* Append this window's mean raw envelope to the breathing ring before
   * running the filter bank, so the newest breath sample participates.
   * Windows with no frames push nothing — a supply gap must not inject
   * a fake zero-sample step into the spectrum. */
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

  /* RSSI stats. */
  int8_t rssi_mean = 0, rssi_std = 0;
  if (s_rssi_n > 0) {
    const int32_t m = s_rssi_sum / s_rssi_n;
    const int32_t v = (s_rssi_sq_sum / s_rssi_n) - m * m;
    rssi_mean = clip_i8(m);
    /* True integer sqrt: for variance 1600 gives 40, for 10000 gives 100. */
    rssi_std = clip_i8((int32_t)isqrt_u32((uint32_t)(v > 0 ? v : 0)));
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
