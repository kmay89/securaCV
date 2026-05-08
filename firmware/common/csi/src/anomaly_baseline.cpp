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
 * Per-coefficient settings (NVS-backed via Tuning Lab in PR 10):
 *   anomaly.baseline.spike_ratio   — int, percent, default 250 (2.5×)
 *   anomaly.baseline.min_motion    — uint8, default 60
 *   anomaly.baseline.min_breathing — uint8, default 50
 *   anomaly.baseline.cooldown_sec  — int, default 600 (10 min)
 */

#include "anomaly_baseline.h"
#include "csi_event.h"

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

uint8_t  s_motion_ring[RING_LEN];
uint8_t  s_breathing_ring[RING_LEN];
size_t   s_ring_head     = 0;
uint16_t s_warmup_left   = BASELINE_PRIME_TICKS;
uint16_t s_motion_cool   = 0;     /* ticks remaining in motion cooldown */
uint16_t s_breathing_cool= 0;

uint16_t s_spike_ratio   = 250;   /* percent (2.5×) */
uint8_t  s_min_motion    = 60;
uint8_t  s_min_breathing = 50;
uint16_t s_cooldown_ticks= 600;

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "unusual_motion",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_MOTION_SCORE,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 6,
  },
  {
    /* type_name */               "unusual_breathing",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_BREATHING_SCORE,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 4,
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
  s_spike_ratio    = (uint16_t)csi_module_settings_int(s, "anomaly.baseline.spike_ratio",  250);
  s_min_motion     = (uint8_t) csi_module_settings_int(s, "anomaly.baseline.min_motion",   60);
  s_min_breathing  = (uint8_t) csi_module_settings_int(s, "anomaly.baseline.min_breathing",50);
  s_cooldown_ticks = (uint16_t)csi_module_settings_int(s, "anomaly.baseline.cooldown_sec", 600);

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
  const uint8_t breathing = reduce_band(f->v, IDX_BREATHING_FFT_BASE, IDX_BREATHING_FFT_COUNT);

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
