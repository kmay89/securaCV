/**
 * @file core_presence.cpp
 * @brief core.presence — state-change emitter for the headline UI.
 *
 * Confidence model (mirrors the plan's "What CSI Can and Cannot See" table):
 *   tentative → low SNR or just powered on. Hero plate stays on prior state.
 *   observed  → motion or breathing visible, sustained at least 3 windows.
 *   confirmed → sustained at least 20 windows AND high SNR.
 *
 * The bundler in csi_event handles the duration aggregation so we just emit
 * a state_changed event whenever the state name flips.
 */

#include "core_presence.h"
#include "csi_event.h"

#include <string.h>

namespace {

/* csi_features_t.v[] layout — mirrored from csi_features.h:11-18 */
constexpr int IDX_DOPPLER_BASE       = 8;   /* phase-Doppler bands [8..11] */
constexpr int IDX_BREATHING_FFT_BASE = 12;  /* breathing FFT       [12..19] */
constexpr int IDX_RSSI_MEAN          = 20;  /* RSSI stats          [20..23] */
constexpr int IDX_RSSI_MAX           = 22;
constexpr int IDX_RSSI_MIN           = 23;

/* State machine. */
enum State {
  STATE_EMPTY = 0,
  STATE_SUBTLE,
  STATE_QUIET,
  STATE_ACTIVE,
  STATE_TOGETHER,
  STATE__COUNT
};

const char* STATE_NAMES[STATE__COUNT] = {
  "empty", "subtle", "quiet", "active", "together"
};

State    s_current_state         = STATE_EMPTY;
uint16_t s_windows_in_state      = 0;
uint8_t  s_pet_mode              = 0;            /* 0 = off, 1 = on (suppresses small-pet false positives) */
uint8_t  s_motion_threshold      = 35;           /* 0..127, tunable */
uint8_t  s_active_threshold      = 75;           /* 0..127 */
uint8_t  s_breathing_threshold   = 30;           /* 0..127 */
uint16_t s_pet_mode_required_win = 30;           /* 30 windows ~= 30 seconds */

/* Multipath shimmer filter: RSSI swing >threshold WITHOUT Doppler
 * is a non-human artifact (HVAC, neighbor WiFi, fan). Reject by
 * returning STATE_EMPTY so presence doesn't advance. */
uint8_t  s_shimmer_rssi_swing   = 8;            /* dB swing threshold */
uint8_t  s_shimmer_doppler_floor = 30;           /* Doppler below this = no real motion */
bool     s_shimmer_enabled       = true;

/* Manifest — defines exactly which fields the chokepoint will let through. */
const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "presence_changed",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_CONFIDENCE
                                 | CSI_FIELD_DURATION_SEC
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_MOTION_SCORE
                                 | CSI_FIELD_BREATHING_SCORE
                                 | CSI_FIELD_DOMINANT_SIGNAL
                                 | CSI_FIELD_BUNDLED_COUNT
                                 | CSI_FIELD_DISMISSED,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 6,
  },
};

/* Reduce a v[] band's signed-byte vector into an unsigned 0..127 magnitude. */
uint8_t reduce_magnitude(const int8_t* v, int from, int to) {
  int32_t sum = 0;
  for (int i = from; i < to; ++i) {
    int8_t s = v[i];
    sum += (s < 0) ? -(int32_t)s : (int32_t)s;
  }
  const int n = to - from;
  if (n <= 0) return 0;
  int32_t avg = sum / n;
  if (avg > 127) avg = 127;
  return (uint8_t)avg;
}

const char* confidence_for(uint16_t windows_in_state, uint8_t magnitude) {
  if (windows_in_state >= 20 && magnitude >= 60) return "confirmed";
  if (windows_in_state >= 3) return "observed";
  return "tentative";
}

State derive_target_state(uint8_t motion, uint8_t breathing, uint16_t streak) {
  /* Together = high motion + sustained — single antenna can't count, but the
   * feature variance signature widens noticeably with multiple movers. */
  /* The gate must stay reachable: reduce_magnitude caps at 127, and the
   * quiet preset's active threshold (90) plus the low-sensitivity offset
   * (+20) plus this margin (+20) used to land at 140 — "together" could
   * never fire on the quiet preset. Clamp the gate to 127. */
  const int together_gate = (s_active_threshold + 20 > 127) ? 127 : (s_active_threshold + 20);
  if (motion >= together_gate && streak >= 5) return STATE_TOGETHER;
  if (motion >= s_active_threshold)                     return STATE_ACTIVE;
  /* Breathing (Quiet) is gated by Pet Mode: small pets can't sustain a
   * human-band breathing peak for 30 s. */
  if (breathing >= s_breathing_threshold) {
    if (s_pet_mode && streak < s_pet_mode_required_win) return STATE_SUBTLE;
    return STATE_QUIET;
  }
  if (motion >= s_motion_threshold) return STATE_SUBTLE;
  return STATE_EMPTY;
}

void on_init(const csi_module_settings_t* s) {
  s_pet_mode             = (uint8_t)csi_module_settings_bool (s, "core.presence.pet_mode",            false) ? 1 : 0;

  /* Two-layer threshold model:
   *
   *   1. Preset (0=sensitive, 1=balanced, 2=quiet) picks a baseline
   *      tuple of (motion, active, breathing) thresholds. Default is
   *      balanced — matches the previous static defaults.
   *   2. Sensitivity slider (0..100, default 50) offsets every threshold
   *      by ±20 points around the preset baseline. Higher slider value
   *      = more sensitive = lower thresholds.
   *
   * The dashboard sends preset + sensitivity (Tier-3 user surface).
   * Direct threshold keys (motion_threshold / active_threshold /
   * breathing_threshold) remain available as per-coefficient overrides
   * for the Tuning Lab (Tier-4); when they're absent the preset+
   * sensitivity baseline wins. */
  const int32_t preset = csi_module_settings_int(s, "core.presence.preset",      1);
  const int32_t sens   = csi_module_settings_int(s, "core.presence.sensitivity", 50);

  int32_t base_motion, base_active, base_breathing;
  switch (preset) {
    case 0: base_motion = 25; base_active = 60; base_breathing = 20; break;  // sensitive
    case 2: base_motion = 50; base_active = 90; base_breathing = 40; break;  // quiet
    default: base_motion = 35; base_active = 75; base_breathing = 30; break; // balanced
  }
  /* sens=0   → +20 (less sensitive); sens=50 → 0 (neutral);
   * sens=100 → -20 (more sensitive). The 40 multiplier is the full
   * ±20 span across the 0..100 slider range — (50-sens)*40/100
   * = -20..+20 as documented. */
  const int32_t offset = ((50 - sens) * 40) / 100;
  auto clamp_threshold = [](int32_t v) -> int32_t {
    if (v < 5) return 5;
    if (v > 120) return 120;
    return v;
  };

  /* Direct keys take precedence if explicitly set (Tuning Lab path);
   * the default we pass is the preset+sensitivity baseline so the
   * common case of "user only touched dashboard preset+slider" lands
   * the right thresholds. */
  s_motion_threshold    = (uint8_t)csi_module_settings_int(
      s, "core.presence.motion_threshold",    clamp_threshold(base_motion    + offset));
  s_active_threshold    = (uint8_t)csi_module_settings_int(
      s, "core.presence.active_threshold",    clamp_threshold(base_active    + offset));
  s_breathing_threshold = (uint8_t)csi_module_settings_int(
      s, "core.presence.breathing_threshold", clamp_threshold(base_breathing + offset));

  s_pet_mode_required_win= (uint16_t)csi_module_settings_int (s, "core.presence.pet_mode_seconds",    30);

  s_shimmer_rssi_swing   = (uint8_t)csi_module_settings_int(s, "core.presence.shimmer_rssi_swing",   8);
  s_shimmer_doppler_floor= (uint8_t)csi_module_settings_int(s, "core.presence.shimmer_doppler_floor",30);
  s_shimmer_enabled      = csi_module_settings_bool(s, "core.presence.shimmer_enabled", true);

  s_current_state        = STATE_EMPTY;
  s_windows_in_state     = 0;
}

void emit_state_change(State new_state, uint8_t motion, uint8_t breathing) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_STATE_NAME
                   | CSI_FIELD_CONFIDENCE
                   | CSI_FIELD_TIME_BUCKET
                   | CSI_FIELD_MOTION_SCORE
                   | CSI_FIELD_BREATHING_SCORE
                   | CSI_FIELD_DOMINANT_SIGNAL;

  strncpy(v.state_name, STATE_NAMES[new_state], sizeof(v.state_name) - 1);
  /* The streak at emit time: 0 on a fresh transition ("tentative"), then
   * the periodic re-emits at 5 / 20 / 60 windows carry 5 / 20 / 60 so the
   * bundler can promote the open row to "observed" / "confirmed". This used
   * to pass a literal 0, which pinned every presence row at "tentative"
   * forever and made the re-emit cadence pointless. */
  const char* conf = confidence_for(s_windows_in_state,
                                    motion >= breathing ? motion : breathing);
  strncpy(v.confidence, conf, sizeof(v.confidence) - 1);
  const char* dom = (motion >= breathing) ? "motion" : "breathing";
  if (new_state == STATE_EMPTY) dom = "none";
  strncpy(v.dominant_signal, dom, sizeof(v.dominant_signal) - 1);

  v.motion_score    = motion;
  v.breathing_score = breathing;
  v.time_bucket     = 0;  /* filled in by chokepoint via coarsen_time_fields */

  (void)csi_event_emit("core.presence", "presence_changed", &v);
}

bool is_shimmer(const csi_features_t* f, uint8_t motion) {
  if (!s_shimmer_enabled) return false;
  const int8_t rssi_max = f->v[IDX_RSSI_MAX];
  const int8_t rssi_min = f->v[IDX_RSSI_MIN];
  const int swing = (int)rssi_max - (int)rssi_min;
  return swing > (int)s_shimmer_rssi_swing && motion < s_shimmer_doppler_floor;
}

void on_tick(const csi_features_t* f) {
  if (!f) return;

  const uint8_t motion    = reduce_magnitude(f->v, IDX_DOPPLER_BASE, IDX_DOPPLER_BASE + 4);
  /* Breathing is a PEAK, not a mean: the extractor puts a clean breath into
   * one Goertzel bin (≈40) and leaves the other seven near zero, so the
   * 8-bin mean of a real breath is ≈5 and could never reach the 30-point
   * "quiet" threshold — only broadband noise could. csi_breathing_peak()
   * is the one reducer every consumer shares (anomaly.baseline, the
   * dashboard stream) so the thresholds mean the same thing everywhere. */
  const uint8_t breathing = csi_breathing_peak(f->v);

  /* Multipath shimmer rejection: large RSSI swing without Doppler is
   * a non-human artifact. Treat as empty so presence doesn't advance. */
  const State target = is_shimmer(f, motion)
      ? STATE_EMPTY
      : derive_target_state(motion, breathing, s_windows_in_state);

  if (target != s_current_state) {
    /* Hysteresis: require 2 consecutive windows of the new target before
     * flipping. This rides on top of the bundler so flicker into a single
     * ribbon row is impossible. */
    static State pending = STATE_EMPTY;
    static uint8_t pending_count = 0;
    if (target != pending) { pending = target; pending_count = 1; }
    else                   { pending_count++; }
    if (pending_count >= 2) {
      s_current_state    = target;
      s_windows_in_state = 0;
      emit_state_change(target, motion, breathing);
      pending_count = 0;
    }
  } else {
    if (s_windows_in_state < 0xffff) s_windows_in_state++;
    /* Periodic re-emit so the bundler updates duration and confidence. */
    if (s_windows_in_state == 5 || s_windows_in_state == 20 || (s_windows_in_state % 60u) == 0) {
      emit_state_change(s_current_state, motion, breathing);
    }
  }
}

void on_dismiss(uint32_t /*event_id*/) {
  /* Nudge thresholds slightly so the same scenario is less likely to fire
   * again. We move by a single point and clamp; the user's calibration
   * baseline is the primary control. */
  if (s_motion_threshold < 120) s_motion_threshold++;
  if (s_breathing_threshold < 120) s_breathing_threshold++;
}

void on_deinit() {
  s_current_state    = STATE_EMPTY;
  s_windows_in_state = 0;
}

const csi_module_t MODULE = {
  /* id */                "core.presence",
  /* default_privacy */   CSI_PRIVACY_P0,
  /* events */            EVENTS,
  /* event_count */       sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */              on_init,
  /* tick */              on_tick,
  /* on_event_dismissed */ on_dismiss,
  /* deinit */            on_deinit,
};

}  /* namespace */

extern "C" {
const csi_module_t* core_presence_module(void) { return &MODULE; }
void core_presence_handle_dismiss(uint32_t event_id) { on_dismiss(event_id); }
}
