/*
 * SecuraCV Canary — Sensing State Aggregator
 *
 * Allocation-free; all state is module-private. The CSI HAL feeds us the
 * 32-byte feature vector once per 1 s window; we distill it to four
 * human-readable scalars and a few bar-graph-friendly arrays for the UI.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_sensing.h"

#include <Arduino.h>
#include <string.h>

namespace {

  sensing_state_t s_state;
  bool s_initialized = false;

  /* Smoothing for the 0..100 scores so the UI doesn't jitter on transient
   * single-window spikes. EMA with α = 0.4 (favors recent data; reacts
   * inside ~3 windows). */
  uint8_t ema_u8(uint8_t prev, uint8_t next) {
    return (uint8_t)(((uint32_t)prev * 6 + (uint32_t)next * 4) / 10);
  }

  uint8_t clip_u8(int32_t v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
  }

  /* Fold the 8 amplitude-variance bands into a 0..100 motion score.
   * Empty room ≈ 0–10; person walking ≈ 40–70; rapid motion saturates 100. */
  uint8_t derive_motion(const int8_t v[CSI_FEATURE_DIM]) {
    /* Sum |v[0..7]| (amplitude variance is non-negative in our scaling). */
    int32_t amp = 0;
    for (int i = 0; i < 8; i++) {
      const int8_t b = v[i];
      amp += (b < 0) ? -(int32_t)b : (int32_t)b;
    }
    /* Sum |v[8..11]| (Doppler magnitude — ignores direction). */
    int32_t dop = 0;
    for (int i = 8; i < 12; i++) {
      const int8_t b = v[i];
      dop += (b < 0) ? -(int32_t)b : (int32_t)b;
    }
    /* Empirical scale: amp peaks ~ 600 + dop peaks ~ 300 at saturation.
     * Map (amp + 2*dop) / 12 → 0..100 then clip. */
    const int32_t score = (amp + 2 * dop) / 12;
    return (uint8_t)(score > 100 ? 100 : (score < 0 ? 0 : score));
  }

  /* Fold the 8 breathing-band Goertzel bins (slots 12..19) into a 0..100
   * breathing-likelihood score. The header doc already mapped bin values
   * to roughly [-60, 60] with saturation; we want the *peak* magnitude
   * across the band, not the sum, since breathing concentrates energy in
   * a single bin. */
  uint8_t derive_breathing(const int8_t v[CSI_FEATURE_DIM]) {
    int32_t peak = 0;
    for (int i = 12; i < 20; i++) {
      const int32_t b = v[i];
      const int32_t a = b < 0 ? -b : b;
      if (a > peak) peak = a;
    }
    /* peak ∈ [0, ~127]; scale to 0..100 with mild gain. */
    const int32_t score = (peak * 100) / 100;  /* identity, kept explicit */
    return (uint8_t)(score > 100 ? 100 : score);
  }

  uint8_t derive_label(uint8_t motion, uint8_t breathing,
                       uint32_t last_window_ms) {
    if (last_window_ms == 0) return SENSING_LABEL_OFFLINE;
    const uint32_t age = millis() - last_window_ms;
    if (age > SENSING_TTL_MS) return SENSING_LABEL_OFFLINE;

    if (motion >= 60)                         return SENSING_LABEL_ACTIVE;
    if (motion >= 25)                         return SENSING_LABEL_MOTION;
    if (motion >= 10 || breathing >= 35)      return SENSING_LABEL_PRESENCE;
    return SENSING_LABEL_QUIET;
  }

}  // namespace

extern "C" {

void sensing_init(void) {
  if (s_initialized) return;
  memset(&s_state, 0, sizeof(s_state));
  s_state.activity_label = SENSING_LABEL_OFFLINE;
  s_initialized = true;
}

void sensing_feed_csi(const csi_features_t* features) {
  if (features == nullptr) return;
  if (!s_initialized) sensing_init();

  const uint8_t raw_motion    = derive_motion(features->v);
  const uint8_t raw_breathing = derive_breathing(features->v);

  /* EMA-smooth the headline scores. */
  s_state.motion_score    = ema_u8(s_state.motion_score,    raw_motion);
  s_state.breathing_score = ema_u8(s_state.breathing_score, raw_breathing);

  /* Snapshot the bar-graph arrays verbatim (no smoothing — let the UI
   * decide how much to dampen visually). */
  for (int i = 0; i < 8; i++)  s_state.amp_bands[i]      = features->v[i];
  for (int i = 0; i < 4; i++)  s_state.doppler[i]        = features->v[8 + i];
  for (int i = 0; i < 8; i++)  s_state.breathing_bins[i] = features->v[12 + i];

  /* RSSI / frame health from the named slots. */
  s_state.rssi_dbm         = features->v[20];                          /* mean */
  s_state.rssi_std         = features->v[21];
  s_state.frames_in_window = features->frames_in_window;
  s_state.dropped_estimate = (uint16_t)((uint8_t)features->v[25]);
  s_state.channel          = (uint8_t)features->v[26];
  s_state.bandwidth_code   = (uint8_t)features->v[27];
  s_state.time_bucket      = features->time_bucket;
  s_state.last_window_ms   = millis();
  s_state.windows_seen++;

  s_state.activity_label = derive_label(
      s_state.motion_score, s_state.breathing_score, s_state.last_window_ms);
}

void sensing_tick(void) {
  if (!s_initialized) return;
  if (s_state.last_window_ms == 0) return;

  const uint32_t age = millis() - s_state.last_window_ms;
  if (age <= SENSING_TTL_MS) {
    /* Refresh the label even if no new feature came in (motion may have
     * stabilized into "quiet" via decay below). */
    s_state.activity_label = derive_label(
        s_state.motion_score, s_state.breathing_score, s_state.last_window_ms);
    return;
  }

  /* Past TTL — decay scores linearly to zero over the next TTL window. */
  const uint32_t over = age - SENSING_TTL_MS;
  if (over >= SENSING_TTL_MS) {
    s_state.motion_score    = 0;
    s_state.breathing_score = 0;
    s_state.activity_label  = SENSING_LABEL_OFFLINE;
    return;
  }
  const uint32_t remain = SENSING_TTL_MS - over;
  s_state.motion_score    = (uint8_t)(((uint32_t)s_state.motion_score    * remain) / SENSING_TTL_MS);
  s_state.breathing_score = (uint8_t)(((uint32_t)s_state.breathing_score * remain) / SENSING_TTL_MS);
  s_state.activity_label  = derive_label(
      s_state.motion_score, s_state.breathing_score, s_state.last_window_ms);
}

void sensing_snapshot(sensing_state_t* out) {
  if (out == nullptr) return;
  if (!s_initialized) {
    memset(out, 0, sizeof(*out));
    out->activity_label = SENSING_LABEL_OFFLINE;
    return;
  }
  *out = s_state;
}

const char* sensing_label_name(uint8_t label) {
  switch (label) {
    case SENSING_LABEL_OFFLINE:  return "offline";
    case SENSING_LABEL_QUIET:    return "quiet";
    case SENSING_LABEL_PRESENCE: return "presence";
    case SENSING_LABEL_MOTION:   return "motion";
    case SENSING_LABEL_ACTIVE:   return "active";
    default:                     return "unknown";
  }
}

}  /* extern "C" */
