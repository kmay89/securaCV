// Host tests for csi_wander_jitter.h — the second CSI feature extractor's
// math, on hand-built rows with answers worked out by hand.
//
// Pure header, no Arduino glue, no csi_features.cpp: the pipeline numbers
// (what a synthetic channel reads through the real extractor with the flag
// on) live in test_csi_features.cpp's flag-on build, test_csi_features_wj.
//
// Covered:
//   1. The flag defaults to 0 — nothing sets it in a shipped build.
//   2. isqrt_u32 is floor(√n) (it is now the library's only copy).
//   3. Wander is the POPULATION σ of the per-frame centroid, exact on
//      one-tone shifts (alternating, and an uneven 3:1 split that tells
//      population from sample variance apart).
//   4. Jitter is the exact mean |Δ| per tone per pair, ×16.
//   5. A static tilt (same profile every frame) reads 0 / 0.
//   6. A frame with no positive amplitude has no centroid; n == 0 and a
//      single-centroid window return 0; a negative entry counts as 0.
//   7. Both slots clip at 127 and floor at 0.
//   8. reset() leaves no memory.
//   9. A nullptr prev_row counts no pair.
//  10. Integer results track a double-precision reference on random rows.
//
// Build/run: make (this dir).

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef CSI_WANDER_JITTER
#define WJ_FLAG_SET_BY_BUILD 1
#endif
#include "csi_wander_jitter.h"

#ifndef WJ_FLAG_SET_BY_BUILD
static_assert(CSI_WANDER_JITTER == 0,
              "CSI_WANDER_JITTER must default to 0 (off in every shipped build)");
#endif

static_assert(sizeof(csi_wj::State) == 32, "State is 32 bytes");

using csi_wj::State;

static State fresh() {
  State s;
  std::memset(&s, 0xA5, sizeof(s));  // garbage, so reset() has work to do
  csi_wj::reset(&s);
  return s;
}

// A row of n tones with all its weight on tone `t`.
static void spike(int16_t* row, size_t n, size_t t, int16_t v) {
  for (size_t k = 0; k < n; k++) row[k] = 0;
  row[t] = v;
}

static void test_flag_default() {
#ifndef WJ_FLAG_SET_BY_BUILD
  assert(CSI_WANDER_JITTER == 0);
#endif
  printf("ok  CSI_WANDER_JITTER defaults to 0\n");
}

static void test_isqrt_is_floor_sqrt() {
  for (uint32_t n = 0; n < 200000u; n++) {
    const uint64_t r = csi_wj::isqrt_u32(n);
    assert(r * r <= n && (r + 1) * (r + 1) > n);
  }
  const uint32_t big[] = {0xFFFFFFFFu, 0xFFFE0001u, 0xFFFE0000u,
                          0x80000000u, 0x7FFFFFFFu, 1600u, 10000u};
  for (uint32_t n : big) {
    const uint64_t r = csi_wj::isqrt_u32(n);
    assert(r * r <= n && (r + 1) * (r + 1) > n);
  }
  assert(csi_wj::isqrt_u32(0xFFFFFFFFu) == 65535u);
  assert(csi_wj::isqrt_u32(1600u) == 40u);   // rssi_std's own comment
  printf("ok  isqrt_u32 is floor(sqrt(n))\n");
}

static void test_wander_exact_on_one_tone_shifts() {
  const size_t n = 4;
  int16_t a[n], b[n];
  spike(a, n, 1, 64);  // centroid tone 1 = 256 Q8
  spike(b, n, 2, 64);  // centroid tone 2 = 512 Q8

  // Alternating A B A B: mean 384, σ = 128 Q8 (half a tone) → v28 = 32.
  State s = fresh();
  const int16_t* prev = nullptr;
  for (int f = 0; f < 6; f++) {
    const int16_t* row = (f & 1) ? b : a;
    csi_wj::accumulate(&s, row, prev, n);
    prev = row;
  }
  assert(s.c_n == 6);
  assert(csi_wj::wander_q8(&s) == 128);
  assert(csi_wj::wander_i8(csi_wj::wander_q8(&s)) == 32);

  // A A A B: centroids 256,256,256,512. Population variance = 12288 → 110
  // (a sample variance would give 16384 → 128).
  s = fresh();
  prev = nullptr;
  const int16_t* seq[4] = {a, a, a, b};
  for (const int16_t* row : seq) {
    csi_wj::accumulate(&s, row, prev, n);
    prev = row;
  }
  assert(s.c_sum == 1280 && s.c_sq == 458752);
  assert(csi_wj::wander_q8(&s) == 110);
  assert(csi_wj::wander_i8(110) == 27);

  // Weight split 3:1 between tones 0 and 4 → centroid exactly tone 1.
  int16_t c[5] = {48, 0, 0, 0, 16};
  s = fresh();
  csi_wj::accumulate(&s, c, nullptr, 5);
  assert(s.c_sum == 256 && s.c_n == 1);
  printf("ok  wander is the exact population sigma of the centroid\n");
}

static void test_jitter_exact() {
  // ±d alternation on every tone, mean pinned at 64: |Δ| = 2d on every
  // tone of every pair → jitter_x16 = 32·d.
  const size_t n = 8;
  const int16_t d = 3;
  int16_t up[n], dn[n];
  for (size_t k = 0; k < n; k++) {
    up[k] = (int16_t)(64 + ((k & 1) ? -d : d));
    dn[k] = (int16_t)(64 + ((k & 1) ? d : -d));
  }
  State s = fresh();
  const int16_t* prev = nullptr;
  for (int f = 0; f < 5; f++) {
    const int16_t* row = (f & 1) ? dn : up;
    csi_wj::accumulate(&s, row, prev, n);
    prev = row;
  }
  assert(s.jit_pairs == 4);
  assert(s.jit_sum == (uint64_t)4 * n * 2 * d);
  assert(csi_wj::jitter_x16(&s, n) == 32 * d);          // 96
  assert(csi_wj::jitter_i8(csi_wj::jitter_x16(&s, n)) == 12);

  // One tone of eight steps by 5 each frame: mean |Δ| = 5/8 → ×16 = 10.
  int16_t r0[n], r1[n];
  for (size_t k = 0; k < n; k++) r0[k] = r1[k] = 64;
  r1[3] = 69;
  s = fresh();
  csi_wj::accumulate(&s, r0, nullptr, n);
  csi_wj::accumulate(&s, r1, r0, n);
  csi_wj::accumulate(&s, r0, r1, n);
  assert(csi_wj::jitter_x16(&s, n) == 10);
  assert(csi_wj::jitter_i8(10) == 1);
  printf("ok  jitter is the exact mean |delta| per tone per pair (x16)\n");
}

static void test_static_tilt_reads_zero() {
  // A strongly tilted profile, identical every frame: present, not moving.
  const size_t n = 52;
  int16_t tilt[n];
  for (size_t k = 0; k < n; k++) tilt[k] = (int16_t)(16 + 2 * k);
  State s = fresh();
  const int16_t* prev = nullptr;
  for (int f = 0; f < 20; f++) {
    csi_wj::accumulate(&s, tilt, prev, n);
    prev = tilt;
  }
  assert(s.c_n == 20 && s.jit_pairs == 19);
  assert(csi_wj::wander_q8(&s) == 0);
  assert(csi_wj::jitter_x16(&s, n) == 0);
  printf("ok  a static tilt reads wander 0 and jitter 0\n");
}

static void test_edges() {
  const size_t n = 4;
  int16_t zero[n] = {0, 0, 0, 0};
  int16_t neg[n]  = {-9, -9, -9, -9};
  int16_t a[n], b[n];
  spike(a, n, 0, 64);
  spike(b, n, 3, 64);

  // No positive amplitude → no centroid; the pair still counts.
  State s = fresh();
  csi_wj::accumulate(&s, a, nullptr, n);
  csi_wj::accumulate(&s, zero, a, n);
  csi_wj::accumulate(&s, neg, zero, n);
  csi_wj::accumulate(&s, b, neg, n);
  assert(s.c_n == 2 && "zero and all-negative rows have no centroid");
  assert(s.jit_pairs == 3);
  assert(csi_wj::wander_q8(&s) == 384);  // centroids 0 and 768 → σ 384

  // A negative entry counts as 0 toward the centroid.
  int16_t mixed[n] = {-500, 0, 0, 64};
  s = fresh();
  csi_wj::accumulate(&s, mixed, nullptr, n);
  assert(s.c_n == 1 && s.c_sum == 3 * 256);

  // n == 0 is a no-op, and jitter over zero tones is 0.
  s = fresh();
  csi_wj::accumulate(&s, a, nullptr, 0);
  assert(s.c_n == 0 && s.jit_pairs == 0);
  csi_wj::accumulate(&s, a, nullptr, n);
  csi_wj::accumulate(&s, b, a, n);
  assert(csi_wj::jitter_x16(&s, 0) == 0);

  // One centroid is not a spread.
  s = fresh();
  csi_wj::accumulate(&s, a, nullptr, n);
  assert(s.c_n == 1 && csi_wj::wander_q8(&s) == 0);

  // Null pointers are refused, not dereferenced.
  csi_wj::accumulate(nullptr, a, nullptr, n);
  csi_wj::accumulate(&s, nullptr, a, n);
  csi_wj::reset(nullptr);
  assert(csi_wj::wander_q8(nullptr) == 0);
  assert(csi_wj::jitter_x16(nullptr, n) == 0);
  printf("ok  edges: no centroid, n == 0, one frame, nulls\n");
}

static void test_clip() {
  // Centroid alternating tone 0 ↔ tone 7: σ = 3.5 tones = 896 Q8 → 224 → 127.
  const size_t n = 8;
  int16_t lo[n], hi[n];
  spike(lo, n, 0, 64);
  spike(hi, n, 7, 64);
  State s = fresh();
  const int16_t* prev = nullptr;
  for (int f = 0; f < 4; f++) {
    const int16_t* row = (f & 1) ? hi : lo;
    csi_wj::accumulate(&s, row, prev, n);
    prev = row;
  }
  assert(csi_wj::wander_q8(&s) == 896);
  assert(csi_wj::wander_i8(csi_wj::wander_q8(&s)) == 127);
  // |Δ| = 64 on two tones of eight → 16 per tone → ×16 = 256 → 32: no clip.
  assert(csi_wj::jitter_x16(&s, n) == 256);
  assert(csi_wj::jitter_i8(256) == 32);
  // A pair that swaps a full-scale tone clips.
  int16_t p[2] = {2000, 0}, q[2] = {0, 2000};
  s = fresh();
  csi_wj::accumulate(&s, p, nullptr, 2);
  csi_wj::accumulate(&s, q, p, 2);
  assert(csi_wj::jitter_x16(&s, 2) == 32000);
  assert(csi_wj::jitter_i8(csi_wj::jitter_x16(&s, 2)) == 127);
  // Floors: nothing negative reaches the slots.
  assert(csi_wj::wander_i8(-5) == 0 && csi_wj::jitter_i8(-1) == 0);
  assert(csi_wj::wander_i8(3) == 0 && csi_wj::jitter_i8(7) == 0);
  assert(csi_wj::wander_i8(508) == 127 && csi_wj::wander_i8(512) == 127);
  printf("ok  both slots clip at 127 and floor at 0\n");
}

static void test_reset_leaves_no_memory() {
  const size_t n = 8;
  int16_t lo[n], hi[n];
  spike(lo, n, 0, 64);
  spike(hi, n, 7, 64);
  State s = fresh();
  csi_wj::accumulate(&s, lo, nullptr, n);
  csi_wj::accumulate(&s, hi, lo, n);
  csi_wj::accumulate(&s, lo, hi, n);
  assert(csi_wj::wander_q8(&s) > 0 && csi_wj::jitter_x16(&s, n) > 0);
  csi_wj::reset(&s);
  assert(s.c_sum == 0 && s.c_sq == 0 && s.jit_sum == 0 &&
         s.c_n == 0 && s.jit_pairs == 0);
  assert(csi_wj::wander_q8(&s) == 0 && csi_wj::jitter_x16(&s, n) == 0);
  // The next window reads exactly what a never-used State reads.
  State f = fresh();
  csi_wj::accumulate(&s, hi, nullptr, n);
  csi_wj::accumulate(&f, hi, nullptr, n);
  csi_wj::accumulate(&s, hi, hi, n);
  csi_wj::accumulate(&f, hi, hi, n);
  assert(std::memcmp(&s, &f, sizeof(State)) == 0);
  printf("ok  reset() leaves no memory\n");
}

static void test_null_prev_counts_no_pair() {
  const size_t n = 4;
  int16_t a[n], b[n];
  spike(a, n, 0, 64);
  spike(b, n, 3, 64);
  State s = fresh();
  csi_wj::accumulate(&s, a, nullptr, n);
  csi_wj::accumulate(&s, b, nullptr, n);  // e.g. every frame a first frame
  assert(s.jit_pairs == 0 && s.jit_sum == 0);
  assert(csi_wj::jitter_x16(&s, n) == 0);
  assert(s.c_n == 2 && csi_wj::wander_q8(&s) == 384);  // wander still counts
  printf("ok  a nullptr prev_row counts no pair\n");
}

// Deterministic PRNG.
static uint32_t g_rng = 0xC0FFEEu;
static uint32_t xr() {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return g_rng;
}

static void test_tracks_a_float_reference() {
  // Random non-negative rows of the shape the pipeline hands over
  // (52 tones around a mean of 64): the integer wander stays within 2 Q8
  // of the double-precision σ, and jitter is the exact floor.
  const size_t n = 52;
  int16_t rows[40][n];
  for (int trial = 0; trial < 500; trial++) {
    const int frames = 2 + (int)(xr() % 39u);
    const int spread = 1 + (int)(xr() % 60u);
    State s = fresh();
    double c_sum = 0, c_sq = 0;
    uint64_t dsum = 0;
    for (int f = 0; f < frames; f++) {
      double num = 0, den = 0;
      for (size_t k = 0; k < n; k++) {
        rows[f][k] = (int16_t)(64 - spread + (int)(xr() % (uint32_t)(2 * spread + 1)));
        num += (double)k * rows[f][k];
        den += rows[f][k];
        if (f > 0) {
          const int x = rows[f][k] - rows[f - 1][k];
          dsum += (uint64_t)(x < 0 ? -x : x);
        }
      }
      const double c = 256.0 * num / den;
      c_sum += c;
      c_sq += c * c;
      csi_wj::accumulate(&s, rows[f], f ? rows[f - 1] : nullptr, n);
    }
    const double mean = c_sum / frames;
    double var = c_sq / frames - mean * mean;
    if (var < 0) var = 0;
    const double ref_w = std::sqrt(var);
    const int32_t w = csi_wj::wander_q8(&s);
    assert(std::fabs((double)w - ref_w) <= 2.0);
    const uint64_t ref_j = (dsum * 16u) / ((uint64_t)(frames - 1) * n);
    assert((uint64_t)csi_wj::jitter_x16(&s, n) == ref_j);
  }
  printf("ok  500 random windows track a double-precision reference\n");
}

int main() {
  test_flag_default();
  test_isqrt_is_floor_sqrt();
  test_wander_exact_on_one_tone_shifts();
  test_jitter_exact();
  test_static_tilt_reads_zero();
  test_edges();
  test_clip();
  test_reset_leaves_no_memory();
  test_null_prev_counts_no_pair();
  test_tracks_a_float_reference();
  printf("test_csi_wander_jitter: all tests passed\n");
  return 0;
}
