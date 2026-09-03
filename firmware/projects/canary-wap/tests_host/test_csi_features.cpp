// Host-side physics tests for the CSI feature extractor (csi_features.cpp).
//
// Compiles the REAL staged module against a stub Arduino.h (stubs/csi/) and
// feeds synthetic CSI frames with known physics — a static multipath
// channel, the ESP32's random per-frame common phase offset (CFO/PLL),
// per-packet AGC gain flicker, a simulated front-end AGC, genuine motion,
// and a breathing-rate envelope — then asserts the 32-dim feature vector
// reads what a human would: nothing when nothing happens, motion when the
// channel changes, the right breathing bin when the channel is re-weighted
// at a known rate.
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
//   4. Breathing: a 0.25 Hz re-weighting of the multipath across the
//      channel (per-subcarrier amplitude modulation whose sign rotates
//      across the band, as a moving reflector produces) WITH the front-end
//      AGC simulated on every frame → dominant bin is bin 3 (0.10+0.05·3
//      Hz) and it dominates the bin average (the same peak-dominance gate
//      core_breathing applies). Also: all bins are ZERO before
//      BREATH_MIN_WINDOWS windows exist — a 1 s record cannot resolve
//      breathing and must not pretend to.
//   5. Per-packet gain flicker sustained over a full ring is NOT breathing
//      (regression: the envelope used to be raw received power, which the
//      driver's AGC steps through and a gain-modulated fixture faked).
//   6. reset_history() wipes the cross-window envelope (privacy contract).
//   7. Cadence: windows closing every 700 ms and every 1300 ms still land a
//      12 BPM (0.20 Hz) breath in bin 2, because the ring is fed on a
//      fixed 1 Hz grid keyed by the close timestamps (early closes merge,
//      late closes hold); a 3 s stall holds the sample and the cadence
//      counters/mean period report it; an empty timed window advances the
//      grid with held copies that a real window then replaces.
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

static constexpr double kTwoPi = 6.283185307179586;

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

// Simulated front-end AGC. When on, every frame is rescaled so its mean
// magnitude is AGC_TARGET before quantization — what the ESP32 receiver
// does per packet, and what the breathing path has to survive: whole-frame
// received power carries nothing past it.
static bool g_agc = false;
static constexpr double AGC_TARGET = 30.0;

// Build one frame of interleaved int8 I/Q.
static void build_frame(const Channel& c, double cfo, double gain,
                        const double* extra_rot /* per-sc or null */,
                        const double* amp_scale /* per-sc or null */,
                        int8_t out[SC * 2]) {
  double I[SC], Q[SC];
  double sum_mag = 0.0;
  for (int k = 0; k < SC; k++) {
    const double a  = c.amp[k] * gain * (amp_scale ? amp_scale[k] : 1.0);
    const double ph = c.phase[k] + cfo + (extra_rot ? extra_rot[k] : 0.0);
    I[k] = a * std::cos(ph);
    Q[k] = a * std::sin(ph);
    sum_mag += a;
  }
  double s = 1.0;
  if (g_agc && sum_mag > 0.0) s = AGC_TARGET / (sum_mag / SC);
  for (int k = 0; k < SC; k++) {
    double i = I[k] * s + (frand() * 2.0 - 1.0);  // ±1 LSB noise
    double q = Q[k] * s + (frand() * 2.0 - 1.0);
    if (i > 127) i = 127;
    if (i < -128) i = -128;
    if (q > 127) q = 127;
    if (q < -128) q = -128;
    out[2 * k]     = (int8_t)std::lround(i);
    out[2 * k + 1] = (int8_t)std::lround(q);
  }
}

// Breathing as the physics delivers it: a chest moving ~1 cm changes the
// reflected path's phase against the static paths, so each subcarrier's
// amplitude is re-weighted with a sign that rotates across the channel
// (excess delay × subcarrier spacing). Whole-frame power barely moves —
// which is why a raw-power envelope only "saw" breathing in a fixture with
// no AGC.
static void breath_shape(double t_s, double rate_hz, double depth,
                         double scale[SC]) {
  const double b = std::sin(kTwoPi * rate_hz * t_s);
  for (int k = 0; k < SC; k++) scale[k] = 1.0 + depth * std::sin(0.12 * k) * b;
}

// Run one 1-second window of `frames` frames through the extractor,
// mimicking csi_hal: accumulate each frame, finalize, then reset().
// `frame_cb` fills the per-frame knobs (cfo/gain/extra/scale). Untimed:
// one ring sample per call.
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

// Timed variant — the HAL's real contract: frames sit at wall times
// start_ms + 50·f and the window closes at close_ms, which is handed to
// finalize so the envelope lands on the 1 Hz grid. `frame_cb` gets the
// frame's wall time in seconds.
template <typename F>
static csi_features_t run_window_timed(const Channel& c, int frames,
                                       uint32_t start_ms, uint32_t close_ms,
                                       F frame_cb) {
  int8_t iq[SC * 2];
  for (int f = 0; f < frames; f++) {
    double cfo = 0.0, gain = 1.0;
    const double* extra = nullptr;
    const double* scale = nullptr;
    const double t_s = (start_ms + 50.0 * f) / 1000.0;
    frame_cb(t_s, &cfo, &gain, &extra, &scale);
    build_frame(c, cfo, gain, extra, scale, iq);
    csi_features::accumulate(iq, SC, -55, 6, 0);
  }
  g_now_ms = close_ms;
  csi_features_t out = {};
  csi_features::finalize(&out, (uint32_t)frames, close_ms);
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

// Dominant breathing bin, its score and the bin average (printed).
static void breath_peak(const csi_features_t& w, int* bin, int* best, int* avg) {
  *bin = -1; *best = -1;
  int sum = 0;
  for (int i = 0; i < 8; i++) {
    const int s = w.v[12 + i] < 0 ? -w.v[12 + i] : w.v[12 + i];
    printf("    breath bin %d (%.2f Hz): %d\n", i, 0.10 + 0.05 * i, s);
    sum += s;
    if (s > *best) { *best = s; *bin = i; }
  }
  *avg = sum / 8;
}

// ── Tests ───────────────────────────────────────────────────────────────

static void test_static_channel_with_cfo_reads_empty() {
  csi_features::reset_history();
  const Channel c = make_channel();
  const csi_features_t w = run_window(c, 20,
    [](int, double* cfo, double*, const double**, const double**) {
      *cfo = frand() * kTwoPi;  // fully random common rotation per frame
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
      *cfo  = frand() * kTwoPi;
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
      *cfo = frand() * kTwoPi;  // CFO still present — must not mask motion
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
  static double scale[SC];
  g_agc = true;   // the receiver re-gains every packet: power carries nothing

  // Phase A: only 10 windows — far below BREATH_MIN_WINDOWS. All bins must
  // be zero no matter what the envelope does.
  csi_features_t w = {};
  for (int t = 0; t < 10; t++) {
    w = run_window(c, 10,
      [t](int, double* cfo, double*, const double**, const double** sc) {
        *cfo = frand() * kTwoPi;
        breath_shape((double)t, 0.25, 0.05, scale);
        *sc = scale;
      });
  }
  assert(max_abs(w.v, 12, 20) == 0 &&
         "breathing bins must stay silent until the ring has enough windows");

  // Phase B: continue to 80 windows of a 0.25 Hz (15 BPM) breath. Bin 3
  // (0.10 + 0.05·3 = 0.25 Hz) must dominate — through the AGC.
  for (int t = 10; t < 80; t++) {
    w = run_window(c, 10,
      [t](int, double* cfo, double*, const double**, const double** sc) {
        *cfo = frand() * kTwoPi;
        breath_shape((double)t, 0.25, 0.05, scale);
        *sc = scale;
      });
  }
  g_agc = false;
  int best_bin, best, avg;
  breath_peak(w, &best_bin, &best, &avg);
  assert(best_bin == 3 && "0.25 Hz breathing must land in bin 3 (15 BPM)");
  assert(best >= 25 && "breathing peak must clear the lock threshold range");
  assert(best >= avg + avg / 2 &&
         "peak must dominate 1.5x avg (core_breathing's gate)");
  printf("ok  0.25 Hz breathing lands in bin 3 through the AGC (score=%d, avg=%d)\n",
         best, avg);
}

// The old envelope was raw received power, so ±30 % per-packet gain flicker
// sustained over a ring read as broadband "breathing" energy in every bin
// (and a gain-modulated fixture faked a breath). Band shares of the
// normalized frame do not move with gain at all.
static void test_gain_flicker_is_not_breathing() {
  csi_features::reset_history();
  const Channel c = make_channel();
  csi_features_t w = {};
  for (int t = 0; t < 80; t++) {
    w = run_window(c, 10,
      [](int, double* cfo, double* gain, const double**, const double**) {
        *cfo  = frand() * kTwoPi;
        *gain = 0.7 + 0.6 * frand();
      });
  }
  const int m = max_abs(w.v, 12, 20);
  printf("    80 s of gain flicker: max breathing bin=%d\n", m);
  assert(m <= 5 && "per-packet gain flicker must not read as breathing");
  printf("ok  sustained gain flicker is not breathing\n");
}

static void test_reset_history_wipes_breathing() {
  // Ring is full of breathing from the previous test's channel history.
  csi_features::reset_history();
  const Channel c = make_channel();
  const csi_features_t w = run_window(c, 10,
    [](int, double* cfo, double*, const double**, const double**) {
      *cfo = frand() * kTwoPi;
    });
  assert(max_abs(w.v, 12, 20) == 0 &&
         "reset_history must scrub the cross-window envelope");
  printf("ok  reset_history wipes the breathing envelope\n");
}

// Untimed hosts: a late window stands for every window that elapsed
// meanwhile; note_missed_windows holds the last sample that many times so
// the Goertzel bank's one-sample-per-second time base survives a stall.
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

// Drive `n` timed windows at a fixed close-to-close cadence with a 12 BPM
// (0.20 Hz) breath referenced to WALL time, through the AGC. Whatever the
// loop's pace, the grid must put the breath in bin 2.
static csi_features_t run_cadence(const Channel& c, uint32_t period_ms, int n) {
  static double scale[SC];
  csi_features_t w = {};
  uint32_t t = 5000;  // arbitrary boot offset
  for (int i = 0; i < n; i++) {
    const uint32_t start = t, close = t + period_ms;
    w = run_window_timed(c, 10, start, close,
      [](double t_s, double* cfo, double*, const double**, const double** sc) {
        *cfo = frand() * kTwoPi;
        breath_shape(t_s, 0.20, 0.05, scale);
        *sc = scale;
      });
    t = close;
  }
  return w;
}

static void test_cadence_700ms_merges_early_windows() {
  csi_features::reset_history();
  const Channel c = make_channel();
  g_agc = true;
  const uint32_t held0 = csi_features::held_windows();
  const uint32_t merged0 = csi_features::merged_windows();
  const csi_features_t w = run_cadence(c, 700, 100);   // 70 s of wall time
  g_agc = false;
  assert(csi_features::envelope_len() == 64 && "70 grid seconds fill the ring");
  assert(csi_features::held_windows() == held0 &&
         "closing early never skips a grid slot");
  const uint32_t merged = csi_features::merged_windows() - merged0;
  printf("    700 ms cadence: merged=%u period=%u ms\n",
         (unsigned)merged, (unsigned)csi_features::window_period_ms());
  assert(merged >= 25 && "10/7 windows per second means ~30 same-slot merges");
  assert(csi_features::window_period_ms() == 700 &&
         "mean close-to-close interval must report the real cadence");
  int best_bin, best, avg;
  breath_peak(w, &best_bin, &best, &avg);
  assert(best_bin == 2 && "12 BPM must land in bin 2 at a 700 ms cadence");
  assert(best >= 25 && best >= avg + avg / 2);
  printf("ok  700 ms cadence still reads 12 BPM in bin 2 (score=%d)\n", best);
}

static void test_cadence_1300ms_holds_across_late_windows() {
  csi_features::reset_history();
  const Channel c = make_channel();
  g_agc = true;
  const uint32_t held0 = csi_features::held_windows();
  const uint32_t merged0 = csi_features::merged_windows();
  const csi_features_t w = run_cadence(c, 1300, 100);  // 130 s of wall time
  g_agc = false;
  assert(csi_features::envelope_len() == 64);
  assert(csi_features::merged_windows() == merged0 &&
         "closing late never lands two windows in one slot");
  const uint32_t held = csi_features::held_windows() - held0;
  printf("    1300 ms cadence: held=%u period=%u ms\n",
         (unsigned)held, (unsigned)csi_features::window_period_ms());
  assert(held >= 25 && "0.3 missed seconds per window means ~30 holds");
  // The mean period is since boot: 99 intervals of 700 ms preceded these.
  assert(csi_features::window_period_ms() > 700 &&
         csi_features::window_period_ms() < 1300);
  int best_bin, best, avg;
  breath_peak(w, &best_bin, &best, &avg);
  assert(best_bin == 2 && "12 BPM must land in bin 2 at a 1300 ms cadence");
  assert(best >= 25 && best >= avg + avg / 2);
  printf("ok  1300 ms cadence still reads 12 BPM in bin 2 (score=%d)\n", best);
}

static void test_stall_holds_and_reports() {
  csi_features::reset_history();
  const Channel c = make_channel();
  auto quiet = [](double, double* cfo, double*, const double**, const double**) {
    *cfo = frand() * kTwoPi;
  };
  const uint32_t held0 = csi_features::held_windows();
  const uint32_t merged0 = csi_features::merged_windows();
  // Three on-time windows: closes at 1000, 2000, 3000.
  for (uint32_t s = 0; s < 3000; s += 1000) run_window_timed(c, 10, s, s + 1000, quiet);
  assert(csi_features::envelope_len() == 3);
  const uint32_t period_before = csi_features::window_period_ms();
  // The loop stalls: the window that started at 3000 closes at 6000.
  run_window_timed(c, 10, 3000, 6000, quiet);
  assert(csi_features::envelope_len() == 6 &&
         "a 3 s stall holds 2 copies, then the late window's own sample");
  assert(csi_features::held_windows() - held0 == 2);
  assert(csi_features::window_period_ms() > period_before &&
         "the 3000 ms interval must pull the mean period up");
  // An empty timed window (radio quiet, no frames) still advances the grid.
  csi_features_t out = {};
  g_now_ms = 8000;
  csi_features::finalize(&out, 0, 8000);
  csi_features::reset();
  assert(csi_features::envelope_len() == 8 && "two more grid seconds, both held");
  assert(csi_features::held_windows() - held0 == 4);
  // A real window landing in the held slot REPLACES the held copy (no merge)…
  run_window_timed(c, 10, 8000, 8500, quiet);
  assert(csi_features::envelope_len() == 8);
  assert(csi_features::merged_windows() == merged0);
  // …and a second real window in the same slot is averaged in.
  run_window_timed(c, 10, 8500, 8900, quiet);
  assert(csi_features::envelope_len() == 8);
  assert(csi_features::merged_windows() - merged0 == 1);
  printf("ok  a stall holds the envelope and the cadence stats report it\n");
}

int main() {
  test_static_channel_with_cfo_reads_empty();
  test_agc_flicker_reads_empty();
  test_motion_is_detected();
  test_breathing_bin_and_min_windows();
  test_gain_flicker_is_not_breathing();
  test_reset_history_wipes_breathing();
  test_missed_windows_hold_the_envelope();
  test_cadence_700ms_merges_early_windows();
  test_cadence_1300ms_holds_across_late_windows();
  test_stall_holds_and_reports();
  printf("test_csi_features: all tests passed\n");
  return 0;
}
