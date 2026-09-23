/*
 * SecuraCV Canary — CSI second feature extractor: wander and jitter
 *
 * Two scalars per 1 s window, computed from the same AGC-normalized
 * amplitude rows csi_features.cpp already builds (each frame's true
 * magnitudes rescaled so the row mean is 64). Written into the reserved
 * v[28] / v[29] slots ONLY in a build compiled with -DCSI_WANDER_JITTER=1;
 * the flag is 0 in every shipped build, and then both slots stay zero.
 *
 *   wander (v[28])  population standard deviation, over the window's
 *                   frames, of the per-frame amplitude CENTROID
 *                   c = Σ k·a_k / Σ a_k (a tone index; kept in Q8, so 256
 *                   is one tone). v[28] = σ_c in Q8 >> 2, i.e. 64 per tone
 *                   of centroid spread, clipped to 0..127 (≈ 2 tones).
 *   jitter (v[29])  mean frame-to-frame |Δ a_k| per tone over the window's
 *                   consecutive frame pairs, in normalized-amplitude units
 *                   (64 ≙ the row mean), kept ×16. v[29] = that ×16 >> 3,
 *                   i.e. 128 × (mean |Δ| / row mean), clipped to 0..127.
 *
 * WITHIN-WINDOW ONLY. There is no baseline, no empty-room reference and no
 * memory across windows: reset() runs at every window close. So a room
 * whose multipath profile is tilted but still reads wander ≈ 0 — position
 * is not motion — and only a profile that MOVES during the window scores.
 *
 * Jitter's floor depends on amplitude. Normalization rescales the
 * quantization noise with the row, so a weak link (small I/Q magnitudes)
 * has a higher still-room jitter than a strong one: on the host fixture a
 * link at 0.35× gain reads about like 2 LSB of extra I/Q noise on a full
 * one. Read jitter against the same link's own still reading, never
 * against a fixed number.
 *
 * NO THRESHOLD is defined here or anywhere else, and no module reads
 * either slot. These are our own definitions, written in the style of
 * espressif/esp-radar's waveform_wander / waveform_jitter — esp-radar's
 * metric math is a closed binary, so this is not a port and makes no claim
 * to equal it. Host-tested on synthetic frames only
 * (canary-wap tests_host: test_csi_wander_jitter, test_csi_features_wj);
 * no bench numbers exist.
 *
 * Header-only C++11: no Arduino, no libm, no allocation, every function
 * static inline. The State lives in the including .cpp, so no header
 * changes layout with the flag and mixed-flag translation units stay
 * ODR-safe. Sized for one CSI window (≤ 128 tones, tens of frames —
 * csi_features caps a window at 40); the int64 sums have orders of
 * magnitude of headroom there.
 */

#ifndef SECURACV_CSI_WANDER_JITTER_H
#define SECURACV_CSI_WANDER_JITTER_H

#include <stddef.h>
#include <stdint.h>

/* 0 in every shipped build. Test it by value: #if CSI_WANDER_JITTER. */
#ifndef CSI_WANDER_JITTER
#define CSI_WANDER_JITTER 0
#endif

namespace csi_wj {

struct State {
  int64_t  c_sum;      /* Σ per-frame centroid, Q8 tone index */
  int64_t  c_sq;       /* Σ of its square */
  uint64_t jit_sum;    /* Σ over frame pairs of Σ_k |a_k − a_prev,k| */
  uint32_t c_n;        /* frames that had a centroid */
  uint32_t jit_pairs;  /* consecutive frame pairs */
};

/* Integer square root via bit-by-bit method: floor(√n). Fixed-time per
 * input size; ~16 iterations for 32-bit inputs. No FPU, no libm. The one
 * copy in the library — csi_features.cpp's magnitude and RSSI std use it
 * too. */
static inline uint32_t isqrt_u32(uint32_t n) {
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

/* Start a window: nothing carries over. */
static inline void reset(State* s) {
  if (s == nullptr) return;
  s->c_sum = 0;
  s->c_sq = 0;
  s->jit_sum = 0;
  s->c_n = 0;
  s->jit_pairs = 0;
}

/* Fold one frame in.
 *   row       this frame's normalized amplitudes, n tones
 *   prev_row  the previous frame's row IN THE SAME WINDOW, or nullptr on
 *             the window's first frame (then no pair is counted)
 * An amplitude is a magnitude, so a negative entry counts as 0 toward the
 * centroid; a frame with no positive amplitude has no centroid and is
 * skipped (its pair, if any, still counts toward jitter). */
static inline void accumulate(State* s, const int16_t* row,
                              const int16_t* prev_row, size_t n) {
  if (s == nullptr || row == nullptr || n == 0) return;
  int64_t num = 0, den = 0;
  for (size_t k = 0; k < n; k++) {
    const int64_t a = row[k] > 0 ? (int64_t)row[k] : 0;
    num += (int64_t)k * a;
    den += a;
  }
  if (den > 0) {
    const int64_t c_q8 = (num * 256) / den;
    s->c_sum += c_q8;
    s->c_sq  += c_q8 * c_q8;
    s->c_n++;
  }
  if (prev_row != nullptr) {
    uint64_t d = 0;
    for (size_t k = 0; k < n; k++) {
      const int32_t x = (int32_t)row[k] - (int32_t)prev_row[k];
      d += (uint64_t)(x < 0 ? -x : x);
    }
    s->jit_sum += d;
    s->jit_pairs++;
  }
}

/* σ of the centroid, Q8 tones (population variance, integer floor). 0
 * until two frames had a centroid. */
static inline int32_t wander_q8(const State* s) {
  if (s == nullptr || s->c_n < 2) return 0;
  const int64_t n = (int64_t)s->c_n;
  int64_t var = (s->c_sq - (s->c_sum * s->c_sum) / n) / n;
  if (var < 0) var = 0;
  if (var > (int64_t)0xFFFFFFFF) var = (int64_t)0xFFFFFFFF;
  return (int32_t)isqrt_u32((uint32_t)var);
}

/* Mean |Δ| per tone per frame pair, ×16. 0 with no pair or no tones. */
static inline int32_t jitter_x16(const State* s, size_t n) {
  if (s == nullptr || s->jit_pairs == 0 || n == 0) return 0;
  return (int32_t)((s->jit_sum * 16u) /
                   ((uint64_t)s->jit_pairs * (uint64_t)n));
}

static inline int8_t clip_pos_i8(int32_t v) {
  return (int8_t)(v < 0 ? 0 : (v > 127 ? 127 : v));
}

/* The int8 slot values: v[28] and v[29]. */
static inline int8_t wander_i8(int32_t q8) {
  return clip_pos_i8(q8 <= 0 ? 0 : q8 >> 2);
}
static inline int8_t jitter_i8(int32_t x16) {
  return clip_pos_i8(x16 <= 0 ? 0 : x16 >> 3);
}

}  /* namespace csi_wj */

#endif  /* SECURACV_CSI_WANDER_JITTER_H */
