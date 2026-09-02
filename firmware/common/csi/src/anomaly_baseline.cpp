/**
 * @file anomaly_baseline.cpp
 * @brief Rolling-baseline anomaly detector.
 *
 * Algorithm (per tick):
 *   1. Reduce the current 1 Hz feature window to motion + breathing
 *      scalars (same indices as core_presence: phase-Doppler bands and
 *      the breathing-FFT band).
 *   2. Push into a 60-deep ring of recent scalars (~60 s of history).
 *   3. Compute the average over the ring.
 *   4. If current motion ≥ MIN_ANOMALY_MOTION AND
 *      current motion ≥ avg × SPIKE_RATIO/100 →
 *      emit `unusual_motion` with category=anomaly.
 *   5. Same for breathing with its own thresholds.
 *   6. Cooldown: only one emit per channel within COOLDOWN_TICKS so
 *      sustained activity doesn't generate a stream of anomalies (the
 *      bundler also collapses, but the cooldown prevents the chokepoint
 *      from spending its hourly ceiling on duplicates).
 *
 * Warmup: the first BASELINE_PRIME_TICKS windows after init() prime
 * the ring without emitting. A device coming online doesn't have a
 * baseline yet — every reading would otherwise look "anomalous."
 *
 * Per-coefficient settings (NVS-backed via Tuning Lab in PR 10).
 * Values are clamped at read time so a corrupted NVS slot or
 * out-of-range POST can't break the detector:
 *   anomaly.baseline.spike_ratio   — percent, default 250 (2.5×), range 110..1000
 *   anomaly.baseline.min_motion    — 0..100 scalar, default 60, range 1..100
 *   anomaly.baseline.min_breathing — 0..100 scalar, default 50, range 1..100
 *   anomaly.baseline.cooldown_sec  — seconds, default 600 (10 min), range 30..3600
 */

#include "anomaly_baseline.h"
#include "csi_event.h"

#include <stdint.h>
#include <string.h>

namespace {

/* csi_features_t.v[] indices — mirror core_presence.cpp so we stay
 * consistent with what core.presence calls "motion" and "breathing". */
constexpr int IDX_DOPPLER_BASE       = 8;
constexpr int IDX_DOPPLER_COUNT      = 4;
constexpr int IDX_BREATHING_FFT_BASE = 12;
constexpr int IDX_BREATHING_FFT_COUNT = 8;

constexpr size_t   RING_LEN              = 60;   /* ~60 s @ 1 Hz */
constexpr uint16_t BASELINE_PRIME_TICKS  = 60;   /* warmup before first emit */

/* Defaults exposed via the Tuning Lab (PR 10). Hard min/max are the
 * envelope a host operator can dial these to without breaking the
 * detector — values outside this range get clamped on read so that a
 * corrupted NVS slot or a too-aggressive Tuning Lab POST can't make the
 * module emit nonsense. */
constexpr uint16_t DEFAULT_SPIKE_RATIO   = 250;   /* percent (2.5×) */
constexpr uint16_t MIN_SPIKE_RATIO       = 110;   /* below 1.1× the gate is meaningless */
constexpr uint16_t MAX_SPIKE_RATIO       = 1000;  /* above 10× nothing ever fires */

constexpr uint8_t  DEFAULT_MIN_MOTION    = 60;
constexpr uint8_t  DEFAULT_MIN_BREATHING = 50;
constexpr uint8_t  MIN_FLOOR             = 1;     /* 0 disables the absolute floor */
constexpr uint8_t  MAX_FLOOR             = 100;   /* scalars are 0..100 */

constexpr uint16_t DEFAULT_COOLDOWN_SEC  = 600;   /* 10 min */
constexpr uint16_t MIN_COOLDOWN_SEC      = 30;    /* avoid notification floods */
constexpr uint16_t MAX_COOLDOWN_SEC      = 3600;  /* one per hour is the practical max */

uint8_t  s_motion_ring[RING_LEN];
uint8_t  s_breathing_ring[RING_LEN];
size_t   s_ring_head     = 0;
uint16_t s_warmup_left   = BASELINE_PRIME_TICKS;
uint16_t s_motion_cool   = 0;     /* ticks remaining in motion cooldown */
uint16_t s_breathing_cool= 0;

uint16_t s_spike_ratio   = DEFAULT_SPIKE_RATIO;
uint8_t  s_min_motion    = DEFAULT_MIN_MOTION;
uint8_t  s_min_breathing = DEFAULT_MIN_BREATHING;
uint16_t s_cooldown_ticks= DEFAULT_COOLDOWN_SEC;

template <typename T>
T clamp_range(int32_t v, T lo, T hi) {
  if (v < (int32_t)lo) return lo;
  if (v > (int32_t)hi) return hi;
  return (T)v;
}

/* Per-event ceilings are equal because csi_event::emit() enforces the
 * ceiling per-module, not per-event-type. If these differed, the event
 * declared first would set the effective cap and starve the other. The
 * shared cap below is the budget across both anomaly types combined. */
constexpr uint8_t ANOMALY_CEILING_PER_HOUR = 10;

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "unusual_motion",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_MOTION_SCORE,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ ANOMALY_CEILING_PER_HOUR,
  },
  {
    /* type_name */               "unusual_breathing",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_BREATHING_SCORE,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ ANOMALY_CEILING_PER_HOUR,
  },
};

uint8_t reduce_band(const int8_t* v, int from, int count) {
  int32_t s = 0;
  for (int i = from; i < from + count; ++i) {
    int8_t b = v[i];
    s += (b < 0) ? -(int32_t)b : (int32_t)b;
  }
  if (count <= 0) return 0;
  int32_t avg = s / count;
  if (avg > 100) avg = 100;
  return (uint8_t)avg;
}

uint8_t ring_average(const uint8_t* ring) {
  uint32_t sum = 0;
  for (size_t i = 0; i < RING_LEN; ++i) sum += ring[i];
  return (uint8_t)(sum / RING_LEN);
}

void on_init(const csi_module_settings_t* s) {
  /* Read each setting then clamp into a sane range. A corrupted NVS slot
   * or a Tuning Lab POST that overshoots the host clamps would otherwise
   * silently produce zero-coverage detectors (e.g. spike_ratio=0 fires
   * every tick) or notification floods (cooldown=0). */
  s_spike_ratio    = clamp_range<uint16_t>(
      csi_module_settings_int(s, "anomaly.baseline.spike_ratio",  DEFAULT_SPIKE_RATIO),
      MIN_SPIKE_RATIO, MAX_SPIKE_RATIO);
  s_min_motion     = clamp_range<uint8_t>(
      csi_module_settings_int(s, "anomaly.baseline.min_motion",   DEFAULT_MIN_MOTION),
      MIN_FLOOR, MAX_FLOOR);
  s_min_breathing  = clamp_range<uint8_t>(
      csi_module_settings_int(s, "anomaly.baseline.min_breathing", DEFAULT_MIN_BREATHING),
      MIN_FLOOR, MAX_FLOOR);
  s_cooldown_ticks = clamp_range<uint16_t>(
      csi_module_settings_int(s, "anomaly.baseline.cooldown_sec", DEFAULT_COOLDOWN_SEC),
      MIN_COOLDOWN_SEC, MAX_COOLDOWN_SEC);

  memset(s_motion_ring,    0, sizeof(s_motion_ring));
  memset(s_breathing_ring, 0, sizeof(s_breathing_ring));
  s_ring_head      = 0;
  s_warmup_left    = BASELINE_PRIME_TICKS;
  s_motion_cool    = 0;
  s_breathing_cool = 0;
}

void emit_unusual(const char* type, const char* state_label,
                  uint8_t motion, uint8_t breathing) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_ANOMALY;
  v.present_fields = CSI_FIELD_STATE_NAME
                   | CSI_FIELD_CONFIDENCE
                   | CSI_FIELD_TIME_BUCKET;
  if (motion > 0)    v.present_fields |= CSI_FIELD_MOTION_SCORE;
  if (breathing > 0) v.present_fields |= CSI_FIELD_BREATHING_SCORE;

  strncpy(v.state_name, state_label, sizeof(v.state_name) - 1);
  strncpy(v.confidence, "observed",  sizeof(v.confidence) - 1);
  v.motion_score    = motion;
  v.breathing_score = breathing;
  (void)csi_event_emit("anomaly.baseline", type, &v);
}

void on_tick(const csi_features_t* f) {
  if (!f) return;

  const uint8_t motion    = reduce_band(f->v, IDX_DOPPLER_BASE,       IDX_DOPPLER_COUNT);
  /* Peak, not mean — the shared reducer (csi_types.h): a real breath is one
   * Goertzel bin ≈40 and seven near zero, so the 8-bin mean (≈5) could never
   * reach min_breathing=50 and "unusual_breathing" only fired on broadband
   * noise. */
  const uint8_t breathing = csi_breathing_peak(f->v);

  /* Decay cooldown counters. */
  if (s_motion_cool    > 0) s_motion_cool--;
  if (s_breathing_cool > 0) s_breathing_cool--;

  /* During warmup we only fill the rolling buffer. After that we
   * compare current vs average and decide. */
  if (s_warmup_left > 0) {
    s_motion_ring[s_ring_head]    = motion;
    s_breathing_ring[s_ring_head] = breathing;
    s_ring_head = (s_ring_head + 1) % RING_LEN;
    s_warmup_left--;
    return;
  }

  const uint8_t  motion_avg    = ring_average(s_motion_ring);
  const uint8_t  breathing_avg = ring_average(s_breathing_ring);

  /* Threshold: current must clear an absolute floor AND beat the
   * rolling average by the configured ratio. The ratio guard is the
   * primary "out-of-pattern" signal — a quiet room with average=5
   * suddenly seeing motion=50 is unusual; the same motion=50 in a
   * room that's already averaging 40 is just normal activity. */
  if (s_motion_cool == 0
      && motion >= s_min_motion
      && (uint32_t)motion * 100u >= (uint32_t)motion_avg * s_spike_ratio) {
    emit_unusual("unusual_motion", "unusual_motion", motion, 0);
    s_motion_cool = s_cooldown_ticks;
  }
  if (s_breathing_cool == 0
      && breathing >= s_min_breathing
      && (uint32_t)breathing * 100u >= (uint32_t)breathing_avg * s_spike_ratio) {
    emit_unusual("unusual_breathing", "unusual_breathing", 0, breathing);
    s_breathing_cool = s_cooldown_ticks;
  }

  /* Append after the comparison so the current sample doesn't
   * artificially inflate the baseline it's being compared against. */
  s_motion_ring[s_ring_head]    = motion;
  s_breathing_ring[s_ring_head] = breathing;
  s_ring_head = (s_ring_head + 1) % RING_LEN;
}

void on_deinit() {
  s_warmup_left    = BASELINE_PRIME_TICKS;
  s_motion_cool    = 0;
  s_breathing_cool = 0;
}

const csi_module_t MODULE = {
  /* id */                 "anomaly.baseline",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             on_deinit,
};

}  /* namespace */

extern "C" const csi_module_t* anomaly_baseline_module(void) { return &MODULE; }
