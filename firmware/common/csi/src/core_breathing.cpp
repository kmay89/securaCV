/**
 * @file core_breathing.cpp
 * @brief Confidence-gated breathing detector.
 *
 * The numeric BPM (CSI_FIELD_BREATHING_RATE) is P1 — it only emits when the
 * user has opted in to "Detailed metrics." When the user has not opted in
 * (default), this module still emits the `breathing_confirmed` state at P0
 * but with the BPM field zeroed by the privacy chokepoint.
 *
 * The 8-bin breathing FFT lives at v[12..19]. We pick the dominant bin and
 * map it to BPM using the band's 0.1..0.5 Hz coverage:
 *   bin index i (0..7) ↔ centre frequency (0.1 + i*0.05) Hz
 *                     ↔ BPM = 60 * f
 */

#include "core_breathing.h"
#include "csi_event.h"

#include <string.h>

namespace {

constexpr int IDX_BREATHING_BASE = 12;
constexpr int BREATHING_BIN_COUNT = 8;

uint16_t s_consecutive_locks   = 0;
uint8_t  s_lock_threshold      = 30;   /* peak score required for "lock" */
uint8_t  s_confirm_window      = 20;   /* ~20 s sustained for confirmed   */
bool     s_emitted_confirmed   = false;
uint8_t  s_last_dominant_bin   = 0;

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "breathing_confirmed",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_DURATION_SEC
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_BREATHING_SCORE
                                 | CSI_FIELD_BREATHING_RATE   /* P1 — see privacy class below */
                                 | CSI_FIELD_DOMINANT_SIGNAL,
    /* privacy */                 CSI_PRIVACY_P1,
    /* default_ceiling_per_hour */ 4,
  },
  {
    /* type_name */               "breathing_lost",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_TIME_BUCKET,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 4,
  },
};

uint8_t bin_score(int8_t s) { return (uint8_t)((s < 0) ? -(int)s : (int)s); }

uint8_t dominant_bin(const int8_t* v, uint8_t* peak_score_out) {
  uint8_t best_bin = 0;
  uint8_t best_score = 0;
  for (int i = 0; i < BREATHING_BIN_COUNT; ++i) {
    const uint8_t s = bin_score(v[IDX_BREATHING_BASE + i]);
    if (s > best_score) { best_score = s; best_bin = (uint8_t)i; }
  }
  if (peak_score_out) *peak_score_out = best_score;
  return best_bin;
}

/* Peak score must dominate the average; otherwise it's broadband noise. */
bool peak_dominates(const int8_t* v, uint8_t peak_score) {
  uint32_t sum = 0;
  for (int i = 0; i < BREATHING_BIN_COUNT; ++i) {
    sum += bin_score(v[IDX_BREATHING_BASE + i]);
  }
  const uint8_t avg = (uint8_t)(sum / BREATHING_BIN_COUNT);
  return peak_score >= (uint8_t)(avg + (avg >> 1));   /* peak > 1.5 × avg */
}

uint8_t bpm_from_bin(uint8_t bin) {
  /* (0.1 + bin*0.05) Hz × 60 = (6 + bin*3) BPM. Range 6..27 BPM. */
  return (uint8_t)(6 + bin * 3);
}

void on_init(const csi_module_settings_t* s) {
  s_lock_threshold     = (uint8_t)csi_module_settings_int(s, "core.breathing.lock_threshold", 30);
  s_confirm_window     = (uint8_t)csi_module_settings_int(s, "core.breathing.confirm_seconds", 20);
  s_consecutive_locks  = 0;
  s_emitted_confirmed  = false;
  s_last_dominant_bin  = 0;
}

void emit_confirmed(uint8_t peak, uint8_t bin) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME
                   | CSI_FIELD_CONFIDENCE
                   | CSI_FIELD_TIME_BUCKET
                   | CSI_FIELD_BREATHING_SCORE
                   | CSI_FIELD_BREATHING_RATE
                   | CSI_FIELD_DOMINANT_SIGNAL;
  strncpy(v.state_name,      "breathing_nearby", sizeof(v.state_name) - 1);
  strncpy(v.confidence,      "confirmed",        sizeof(v.confidence) - 1);
  strncpy(v.dominant_signal, "breathing",        sizeof(v.dominant_signal) - 1);
  v.breathing_score    = peak;
  v.breathing_rate_bpm = bpm_from_bin(bin);
  (void)csi_event_emit("core.breathing", "breathing_confirmed", &v);
}

void emit_lost() {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_CONFIDENCE | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, "breathing_lost", sizeof(v.state_name) - 1);
  strncpy(v.confidence, "observed",       sizeof(v.confidence) - 1);
  (void)csi_event_emit("core.breathing", "breathing_lost", &v);
}

void on_tick(const csi_features_t* f) {
  if (!f) return;

  uint8_t peak_score = 0;
  const uint8_t bin = dominant_bin(f->v, &peak_score);

  const bool locked = (peak_score >= s_lock_threshold)
                      && peak_dominates(f->v, peak_score);

  if (locked) {
    if (s_consecutive_locks < 0xffff) s_consecutive_locks++;
    s_last_dominant_bin = bin;
    if (!s_emitted_confirmed && s_consecutive_locks >= s_confirm_window) {
      s_emitted_confirmed = true;
      emit_confirmed(peak_score, bin);
    } else if (s_emitted_confirmed && (s_consecutive_locks % 30u) == 0) {
      /* Re-emit every ~30 s so the bundler updates duration/BPM. */
      emit_confirmed(peak_score, bin);
    }
  } else {
    if (s_emitted_confirmed) emit_lost();
    s_emitted_confirmed = false;
    s_consecutive_locks = 0;
  }
}

void on_dismiss(uint32_t /*event_id*/) {
  /* User said "that wasn't breathing" — raise the lock threshold a notch. */
  if (s_lock_threshold < 100) s_lock_threshold++;
}

void on_deinit() {
  s_consecutive_locks  = 0;
  s_emitted_confirmed  = false;
}

const csi_module_t MODULE = {
  /* id */                 "core.breathing",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ on_dismiss,
  /* deinit */             on_deinit,
};

}  /* namespace */

extern "C" {
const csi_module_t* core_breathing_module(void) { return &MODULE; }
}
