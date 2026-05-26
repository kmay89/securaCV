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
  sensing_witness_cb_t s_witness_cb = nullptr;

  /* Fire the witness callback if registered, then zero the local
   * struct so the per-event payload doesn't outlive the call. */
  void fire_witness(uint8_t kind, uint8_t confidence,
                    uint8_t time_bucket, uint8_t category) {
    if (s_witness_cb == nullptr) return;
    sensing_witness_event_t we = {};
    we.kind        = kind;
    we.confidence  = confidence;
    we.time_bucket = time_bucket;
    we.category    = category;
    s_witness_cb(&we);
    /* Volatile zero so the optimizer can't elide the wipe. */
    volatile uint8_t* p = (volatile uint8_t*)&we;
    for (size_t i = 0; i < sizeof(we); i++) p[i] = 0;
  }

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

/* TTL for the last_audio_event_* fields: keep them sticky for 30 s so a
 * 1 Hz dashboard poll never misses a single-shot T3/T4 event. After this
 * window expires the snapshot reverts to event_type=0 ("none"). */
static constexpr uint32_t ACOUSTIC_TTL_MS = 30000;

/* TTL for last_touch_event_* — same rationale, slightly longer because
 * touch events (panic, tamper) are rarer and a slow operator may want
 * a longer window to catch the indicator on the dashboard. */
static constexpr uint32_t TOUCH_TTL_MS    = 60000;

/* TTL for IR activity. Keep short — IR is high-frequency (several
 * presses per minute when someone's actively using a remote) so we
 * don't want the dashboard pill to lag reality. */
static constexpr uint32_t IR_TTL_MS       = 10000;

/* TTL for the temperature-drift event — long because the underlying
 * detector itself fires at most once per 5 minutes (suppress_ms). */
static constexpr uint32_t TEMP_DRIFT_TTL_MS = 300000;

extern "C" {

void sensing_init(void) {
  if (s_initialized) return;
  memset(&s_state, 0, sizeof(s_state));
  s_state.activity_label = SENSING_LABEL_OFFLINE;
  s_initialized = true;
}

void sensing_feed_audio_event(uint8_t event_type, uint8_t confidence,
                              uint16_t cycle_count, uint8_t time_bucket) {
  if (!s_initialized) sensing_init();
  s_state.last_audio_event_type  = event_type;
  s_state.last_audio_event_conf  = confidence;
  s_state.last_audio_event_count = cycle_count;
  s_state.last_audio_event_ms    = millis();
  /* Adopt the audio module's bucket so a CSI-disabled build still
   * surfaces a coherent time_bucket on the Sensing snapshot. CSI's own
   * feed_csi() call will overwrite this if both sensors are running —
   * the buckets are computed from millis() in both modules with the
   * same width, so they will always agree. */
  s_state.time_bucket = time_bucket;

  /* T3 (smoke) and T4 (CO) are emergency events; sign them into the
   * witness chain so the operator has tamper-evident proof later. The
   * `category` byte carries the low byte of cycle_count — useful to
   * tell "alarm sounded once" from "alarm has been going for minutes."
   * Phase 2b transient events (knock / doorbell / glass-break) are also
   * witness-worthy: a later operator may need a signed record of when
   * something noisy happened, especially glass-break for insurance. */
  if (event_type == 1 /* AUDIO_EVENT_T3_SMOKE_ALARM */) {
    fire_witness(SENSING_WITNESS_AUDIO_T3, confidence, time_bucket,
                 (uint8_t)(cycle_count & 0xFF));
  } else if (event_type == 2 /* AUDIO_EVENT_T4_CO_ALARM */) {
    fire_witness(SENSING_WITNESS_AUDIO_T4, confidence, time_bucket,
                 (uint8_t)(cycle_count & 0xFF));
  } else if (event_type == 3 /* AUDIO_EVENT_KNOCK */) {
    fire_witness(SENSING_WITNESS_KNOCK, confidence, time_bucket,
                 (uint8_t)(cycle_count & 0xFF));
  } else if (event_type == 4 /* AUDIO_EVENT_DOORBELL */) {
    fire_witness(SENSING_WITNESS_DOORBELL, confidence, time_bucket,
                 (uint8_t)(cycle_count & 0xFF));
  } else if (event_type == 5 /* AUDIO_EVENT_GLASS_BREAK */) {
    fire_witness(SENSING_WITNESS_GLASS_BREAK, confidence, time_bucket,
                 (uint8_t)(cycle_count & 0xFF));
  }
}

void sensing_feed_mic_mute_event(bool muted, uint8_t source,
                                 uint8_t time_bucket) {
  if (!s_initialized) sensing_init();
  /* No aggregator state field — mute is a config/audit event, not a
   * "what is the room doing now?" signal. The witness record IS the
   * artifact; the dashboard reads the live mic state from the audio
   * HAL directly via /api/status. */
  fire_witness(muted ? SENSING_WITNESS_MIC_MUTE : SENSING_WITNESS_MIC_UNMUTE,
               /*confidence*/ 100, time_bucket, source);
}

void sensing_feed_touch_event(uint8_t event_type, uint8_t confidence,
                              uint8_t pad_channel, uint8_t time_bucket) {
  if (!s_initialized) sensing_init();
  s_state.last_touch_event_type  = event_type;
  s_state.last_touch_event_conf  = confidence;
  s_state.last_touch_pad_channel = pad_channel;
  s_state.last_touch_event_ms    = millis();
  s_state.time_bucket            = time_bucket;

  /* Silent panic and enclosure tamper are both witness-worthy. Approach
   * (event_type==3) is too high-rate / low-stakes to sign every time. */
  if (event_type == 1 /* TOUCH_EVENT_SILENT_PANIC */) {
    fire_witness(SENSING_WITNESS_TOUCH_PANIC, confidence, time_bucket,
                 pad_channel);
  } else if (event_type == 2 /* TOUCH_EVENT_ENCLOSURE_TAMPER */) {
    fire_witness(SENSING_WITNESS_TOUCH_TAMPER, confidence, time_bucket,
                 pad_channel);
  }
}

void sensing_feed_ir_event(uint8_t category, uint8_t hash_bucket,
                           uint8_t confidence, uint8_t time_bucket) {
  if (!s_initialized) sensing_init();
  s_state.last_ir_category     = category;
  s_state.last_ir_hash_bucket  = hash_bucket;
  s_state.last_ir_confidence   = confidence;
  s_state.last_ir_event_ms     = millis();
  s_state.time_bucket          = time_bucket;
}

void sensing_feed_temp_drift_event(uint8_t confidence, uint8_t time_bucket) {
  if (!s_initialized) sensing_init();
  s_state.last_temp_drift_conf = confidence;
  s_state.last_temp_drift_ms   = millis();
  s_state.time_bucket          = time_bucket;
  /* Sustained ±5 °C drift is a tamper indicator (case opened, device
   * relocated). Always witness it; envsens already self-suppresses for
   * 5 min after firing so this isn't chatty. */
  fire_witness(SENSING_WITNESS_TEMP_DRIFT, confidence, time_bucket, 0);
}

void sensing_feed_vision_event(uint8_t event_type, uint8_t confidence,
                               uint8_t zone, uint8_t time_bucket) {
  if (!s_initialized) sensing_init();
  s_state.last_vision_event_type = event_type;
  s_state.last_vision_confidence = confidence;
  s_state.last_vision_zone       = zone;
  s_state.last_vision_event_ms   = millis();
  s_state.time_bucket            = time_bucket;
  if (event_type == 1) {
    fire_witness(SENSING_WITNESS_VISION_MOTION, confidence, time_bucket, zone);
  } else if (event_type == 3) {
    fire_witness(SENSING_WITNESS_VISION_PERSON, confidence, time_bucket, zone);
  }
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

  /* Age out the acoustic event field after ACOUSTIC_TTL_MS. */
  if (s_state.last_audio_event_ms != 0 &&
      (millis() - s_state.last_audio_event_ms) > ACOUSTIC_TTL_MS) {
    s_state.last_audio_event_type  = 0;  /* AUDIO_EVENT_NONE */
    s_state.last_audio_event_conf  = 0;
    s_state.last_audio_event_count = 0;
    s_state.last_audio_event_ms    = 0;
  }

  /* Age out the touch event field after TOUCH_TTL_MS. */
  if (s_state.last_touch_event_ms != 0 &&
      (millis() - s_state.last_touch_event_ms) > TOUCH_TTL_MS) {
    s_state.last_touch_event_type  = 0;  /* TOUCH_EVENT_NONE */
    s_state.last_touch_event_conf  = 0;
    s_state.last_touch_pad_channel = 0;
    s_state.last_touch_event_ms    = 0;
  }

  /* Age out the IR activity field after IR_TTL_MS. */
  if (s_state.last_ir_event_ms != 0 &&
      (millis() - s_state.last_ir_event_ms) > IR_TTL_MS) {
    s_state.last_ir_category    = 0;
    s_state.last_ir_hash_bucket = 0;
    s_state.last_ir_confidence  = 0;
    s_state.last_ir_event_ms    = 0;
  }

  /* Age out the temp-drift event field after TEMP_DRIFT_TTL_MS. */
  if (s_state.last_temp_drift_ms != 0 &&
      (millis() - s_state.last_temp_drift_ms) > TEMP_DRIFT_TTL_MS) {
    s_state.last_temp_drift_conf = 0;
    s_state.last_temp_drift_ms   = 0;
  }

  /* Age out vision events: motion 10s, person 30s. */
  if (s_state.last_vision_event_ms != 0) {
    uint32_t vision_ttl = (s_state.last_vision_event_type == 3) ? 30000 : 10000;
    if ((millis() - s_state.last_vision_event_ms) > vision_ttl) {
      s_state.last_vision_event_type = 0;
      s_state.last_vision_confidence = 0;
      s_state.last_vision_zone       = 0;
      s_state.last_vision_event_ms   = 0;
    }
  }

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

void sensing_set_witness_callback(sensing_witness_cb_t cb) {
  s_witness_cb = cb;
}

}  /* extern "C" */
