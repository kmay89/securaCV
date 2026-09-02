// Host-side physics tests for the CSI feature extractor (csi_features.cpp).
//
// Compiles the REAL staged module against a stub Arduino.h (stubs/csi/) and
// feeds synthetic CSI frames with known physics — a static multipath
// channel, the ESP32's random per-frame common phase offset (CFO/PLL),
// per-packet AGC gain flicker, genuine motion, and a breathing-rate
// envelope — then asserts the 32-dim feature vector reads what a human
// would: nothing when nothing happens, motion when the channel changes,
// the right breathing bin when the envelope oscillates at a known rate.
//
// Covered:
//   1. Static channel + random per-frame CFO rotation → motion ≈ 0 on BOTH
//      the amplitude path v[0..7] and the rotation path v[8..11].
//      (Regression: the v0 cross-product Doppler was CFO noise, and the v0
//      L1 amplitude wobbled with rotation.)
//   2. Per-packet AGC gain flicker (±30 %) → amplitude variance ≈ 0.
//      (Regression: v0 had no per-frame normalization.)
//   3. A moving scatterer (progressive extra rotation + amplitude
//      modulation on the upper subcarriers) → strong v[8..11] response in
//      the affected bands and clear v[0..7] energy.
//   4. Breathing: 0.25 Hz sinusoidal envelope across 80 one-second windows
//      → dominant bin is bin 3 (0.10+0.05·3 Hz) and it dominates the bin
//      average (the same peak-dominance gate core_breathing applies).
//      Also: all bins are ZERO before BREATH_MIN_WINDOWS windows exist —
//      a 1 s record cannot resolve breathing and must not pretend to.
//   5. reset_history() wipes the cross-window envelope (privacy contract).
//
// Build/run: make (this dir). No Arduino runtime needed.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

#include "csi_features.h"
#include "csi_types.h"

// ── Test-controlled clock (csi_features uses millis() for time_bucket) ──
static unsigned long g_now_ms = 0;
unsigned long millis() { return g_now_ms; }

// ── Deterministic PRNG (no rand() seeding surprises across libcs) ──────
static uint32_t g_rng = 0x1234567u;
static uint32_t xr() {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return g_rng;
}
static double frand() { return (double)(xr() % 10000u) / 10000.0; }

// ── Synthetic channel ───────────────────────────────────────────────────
// 52 subcarriers, static amplitude profile + static phase profile, with
// controllable per-frame common rotation (CFO), gain, per-subcarrier
// extra rotation/amplitude (motion), and ±1 LSB quantization noise.
static constexpr int SC = 52;

struct Channel {
  double amp[SC];
  double phase[SC];
};

static Channel make_channel() {
  Channel c;
  for (int k = 0; k < SC; k++) {
    c.amp[k]   = 28.0 + 16.0 * std::sin(k / 3.1);   // rich multipath shape
    c.phase[k] = 0.4 * k + 0.9 * std::sin(k / 2.3); // arbitrary static phase
  }
  return c;
}

// Build one frame of interleaved int8 I/Q.
static void build_frame(const Channel& c, double cfo, double gain,
                        const double* extra_rot /* per-sc or null */,
                        const double* amp_scale /* per-sc or null */,
                        int8_t out[SC * 2]) {
  for (int k = 0; k < SC; k++) {
    const double a  = c.amp[k] * gain * (amp_scale ? amp_scale[k] : 1.0);
    const double ph = c.phase[k] + cfo + (extra_rot ? extra_rot[k] : 0.0);
    double I = a * std::cos(ph) + (frand() * 2.0 - 1.0);  // ±1 LSB noise
    double Q = a * std::sin(ph) + (frand() * 2.0 - 1.0);
    if (I > 127) I = 127;
    if (I < -128) I = -128;
    if (Q > 127) Q = 127;
    if (Q < -128) Q = -128;
    out[2 * k]     = (int8_t)std::lround(I);
    out[2 * k + 1] = (int8_t)std::lround(Q);
  }
}

// Run one 1-second window of `frames` frames through the extractor,
// mimicking csi_hal: accumulate each frame, finalize, then reset().
// `frame_cb` fills the per-frame knobs (cfo/gain/extra/scale).
template <typename F>
static csi_features_t run_window(const Channel& c, int frames, F frame_cb) {
  int8_t iq[SC * 2];
  for (int f = 0; f < frames; f++) {
    double cfo = 0.0, gain = 1.0;
    const double* extra = nullptr;
    const double* scale = nullptr;
    frame_cb(f, &cfo, &gain, &extra, &scale);
    build_frame(c, cfo, gain, extra, scale, iq);
    csi_features::accumulate(iq, SC, -55, 6, 0);
    g_now_ms += 50;  // ~20 Hz
  }
  csi_features_t out = {};
  csi_features::finalize(&out, (uint32_t)frames);
  csi_features::reset();
  return out;
}

static int max_abs(const int8_t* v, int from, int to) {
  int m = 0;
  for (int i = from; i < to; i++) {
    const int a = v[i] < 0 ? -v[i] : v[i];
    if (a > m) m = a;
  }
  return m;
}

// ── Tests ───────────────────────────────────────────────────────────────

static void test_static_channel_with_cfo_reads_empty() {
  csi_features::reset_history();
  const Channel c = make_channel();
  const csi_features_t w = run_window(c, 20,
    [](int, double* cfo, double*, const double**, const double**) {
      *cfo = frand() * 6.283185;  // fully random common rotation per frame
    });
  const int amp = max_abs(w.v, 0, 8);
  const int dop = max_abs(w.v, 8, 12);
  printf("    static+CFO: amp=%d dop=%d\n", amp, dop);
  assert(amp <= 4 && "static channel must not read as amplitude motion");
  assert(dop <= 4 && "random CFO must cancel in the rotation estimator");
  printf("ok  static channel + random CFO reads as empty\n");
}

static void test_agc_flicker_reads_empty() {
  csi_features::reset_history();
  const Channel c = make_channel();
  const csi_features_t w = run_window(c, 20,
    [](int, double* cfo, double* gain, const double**, const double**) {
      *cfo  = frand() * 6.283185;
      *gain = 0.7 + 0.6 * frand();  // ±30 % per-packet AGC flicker
    });
  const int amp = max_abs(w.v, 0, 8);
  printf("    AGC flicker: amp=%d\n", amp);
  assert(amp <= 4 && "per-packet gain flicker must normalize away");
  printf("ok  AGC gain flicker reads as empty\n");
}

static void test_motion_is_detected() {
  csi_features::reset_history();
  const Channel c = make_channel();
  static double extra[SC];
  static double scale[SC];
  const csi_features_t w = run_window(c, 20,
    [](int f, double* cfo, double*, const double** ex, const double** sc) {
      *cfo = frand() * 6.283185;  // CFO still present — must not mask motion
      for (int k = 0; k < SC; k++) {
        if (k >= SC / 2) {
          // Moving scatterer: progressive path-phase rotation + amplitude
          // fading on the upper half of the spectrum.
          extra[k] = 0.35 * f;
          scale[k] = 1.0 + 0.35 * std::sin(0.9 * f + k * 0.2);
        } else {
          extra[k] = 0.0;
          scale[k] = 1.0;
        }
      }
      *ex = extra;
      *sc = scale;
    });
  const int amp = max_abs(w.v, 0, 8);
  const int dop = max_abs(w.v, 8, 12);
  const int dop_lower = max_abs(w.v, 8, 10);   // bands 0..1 = static half
  printf("    motion: amp=%d dop=%d dop_lower=%d\n", amp, dop, dop_lower);
  assert(amp >= 10 && "moving scatterer must show amplitude motion energy");
  assert(dop >= 15 && "moving scatterer must show band rotation");
  assert(dop >= dop_lower &&
         "the moving half of the spectrum must respond at least as hard");
  printf("ok  motion detected through CFO (amp=%d, rotation=%d)\n", amp, dop);
}

static void test_breathing_bin_and_min_windows() {
  csi_features::reset_history();
  const Channel c = make_channel();

  // Phase A: only 10 windows — far below BREATH_MIN_WINDOWS. All bins must
  // be zero no matter what the envelope does.
  csi_features_t w = {};
  for (int t = 0; t < 10; t++) {
    w = run_window(c, 10,
      [t](int, double* cfo, double* gain, const double**, const double**) {
        *cfo  = frand() * 6.283185;
        *gain = 1.0 + 0.08 * std::sin(2.0 * 3.14159265 * 0.25 * t);
      });
  }
  assert(max_abs(w.v, 12, 20) == 0 &&
         "breathing bins must stay silent until the ring has enough windows");

  // Phase B: continue to 80 windows of a 0.25 Hz (15 BPM) breathing
  // envelope. Bin 3 (0.10 + 0.05·3 = 0.25 Hz) must dominate.
  for (int t = 10; t < 80; t++) {
    w = run_window(c, 10,
      [t](int, double* cfo, double* gain, const double**, const double**) {
        *cfo  = frand() * 6.283185;
        *gain = 1.0 + 0.08 * std::sin(2.0 * 3.14159265 * 0.25 * t);
      });
  }
  int best_bin = -1, best = -1;
  int sum = 0;
  for (int i = 0; i < 8; i++) {
    const int s = w.v[12 + i] < 0 ? -w.v[12 + i] : w.v[12 + i];
    printf("    breath bin %d (%.2f Hz): %d\n", i, 0.10 + 0.05 * i, s);
    sum += s;
    if (s > best) { best = s; best_bin = i; }
  }
  const int avg = sum / 8;
  assert(best_bin == 3 && "0.25 Hz breathing must land in bin 3 (15 BPM)");
  assert(best >= 25 && "breathing peak must clear the lock threshold range");
  assert(best >= avg + avg / 2 &&
         "peak must dominate 1.5x avg (core_breathing's gate)");
  printf("ok  0.25 Hz breathing lands in bin 3 (score=%d, avg=%d)\n",
         best, avg);
}

static void test_reset_history_wipes_breathing() {
  // Ring is full of breathing from the previous test's channel history.
  csi_features::reset_history();
  const Channel c = make_channel();
  const csi_features_t w = run_window(c, 10,
    [](int, double* cfo, double*, const double**, const double**) {
      *cfo = frand() * 6.283185;
    });
  assert(max_abs(w.v, 12, 20) == 0 &&
         "reset_history must scrub the cross-window envelope");
  printf("ok  reset_history wipes the breathing envelope\n");
}

// A late window stands for every window that elapsed meanwhile: the HAL
// reports the gap and the envelope ring holds its last sample that many
// times, so the Goertzel bank's one-sample-per-second time base survives a
// stall instead of compressing it (which read as a faster breath rate).
static void test_missed_windows_hold_the_envelope() {
  csi_features::reset_history();
  csi_features::reset();
  int8_t iq[SC * 2];
  memset(iq, 0, sizeof(iq));
  for (int w = 0; w < 3; w++) {
    for (int f = 0; f < 5; f++) csi_features::accumulate(iq, SC, -55, 6, 0);
    csi_features_t out = {};
    csi_features::finalize(&out, 5);
    csi_features::reset();
  }
  assert(csi_features::envelope_len() == 3);
  csi_features::note_missed_windows(0);
  assert(csi_features::envelope_len() == 3);
  csi_features::note_missed_windows(2);
  assert(csi_features::envelope_len() == 5);
  // A gap longer than the ring is capped at the ring: the spectrum restarts
  // from the held value rather than wrapping the head past the tail.
  csi_features::note_missed_windows(500);
  assert(csi_features::envelope_len() == 64);
  // An empty ring has nothing to hold.
  csi_features::reset_history();
  csi_features::note_missed_windows(4);
  assert(csi_features::envelope_len() == 0);
  std::printf("PASS test_missed_windows_hold_the_envelope\n");
}

int main() {
  test_static_channel_with_cfo_reads_empty();
  test_agc_flicker_reads_empty();
  test_motion_is_detected();
  test_breathing_bin_and_min_windows();
  test_reset_history_wipes_breathing();
  test_missed_windows_hold_the_envelope();
  printf("test_csi_features: all tests passed\n");
  return 0;
}
