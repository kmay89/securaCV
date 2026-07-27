/**
 * @file wifi_channel_activity.cpp
 * @brief Ambient channel-activity burst detector (see header).
 *
 * Algorithm (per 1 Hz tick), structurally the same as anomaly_baseline.cpp:
 *   1. Reduce the current feature window to a single identity-free "activity"
 *      scalar built from the RSSI spread (v[21]) and the frame-rate-health
 *      dropped-frame estimate (v[25]). A channel that a nearby device has just
 *      made busy shows up as more RSSI volatility and more rate-limiter-shed
 *      frames — neither of which is, or comes from, any device identity.
 *   2. Push into a 60-deep ring of recent activity (~60 s of history).
 *   3. After warmup, if current activity clears an absolute floor AND beats the
 *      rolling average by the configured ratio → emit `channel_active` with
 *      category = AMBIENT and motion_score = the burst intensity (0..100).
 *   4. Cooldown so a sustained-busy channel doesn't stream duplicate glows.
 *
 * Why the excess-over-baseline gate: an already-busy home network averaging
 * activity=40 shouldn't glow constantly; the signal is the *step up*, the
 * moment the airwaves get busier than this room's own normal.
 *
 * The emitted intensity is the peak; the live UI is what fades the glow over
 * time — this module never keeps a decaying value, so it stores nothing.
 *
 * Per-coefficient settings (NVS-backed via the Tuning Lab), clamped on read so
 * a corrupted slot or an out-of-range POST can't break the detector:
 *   wifi.channel_activity.spike_ratio  — percent, default 250 (2.5x), 110..1000
 *   wifi.channel_activity.min_activity — 0..100 scalar, default 25, range 1..100
 *   wifi.channel_activity.cooldown_sec — seconds, default 5, range 1..3600
 */

#include "wifi_channel_activity.h"
#include "csi_event.h"

#include <stdint.h>
#include <string.h>

namespace {

/* csi_features_t.v[] indices — the identity-free aggregate slots documented in
 * csi_features.h. We read ONLY these two; never a subcarrier or any header. */
constexpr int IDX_RSSI_STD        = 21;  /* [20..23] = RSSI mean/std/max/min   */
constexpr int IDX_FRAMES_DROPPED  = 25;  /* [24..27] = frames/dropped/chan/bw  */

constexpr size_t   RING_LEN             = 60;   /* ~60 s @ 1 Hz */
constexpr uint16_t BASELINE_PRIME_TICKS = 60;   /* warmup before first emit */

constexpr uint16_t DEFAULT_SPIKE_RATIO = 250;   /* percent (2.5x) */
constexpr uint16_t MIN_SPIKE_RATIO     = 110;   /* below 1.1x the gate is meaningless */
constexpr uint16_t MAX_SPIKE_RATIO     = 1000;  /* above 10x nothing ever fires */

constexpr uint8_t  DEFAULT_MIN_ACTIVITY = 25;
constexpr uint8_t  MIN_FLOOR            = 1;     /* 0 would disable the absolute floor */
constexpr uint8_t  MAX_FLOOR            = 100;   /* the scalar is 0..100 */

constexpr uint16_t DEFAULT_COOLDOWN_SEC = 5;    /* ambient: stay responsive */
constexpr uint16_t MIN_COOLDOWN_SEC     = 1;
constexpr uint16_t MAX_COOLDOWN_SEC     = 3600;

uint8_t  s_activity_ring[RING_LEN];
size_t   s_ring_head    = 0;
uint16_t s_warmup_left  = BASELINE_PRIME_TICKS;
uint16_t s_cooldown     = 0;     /* ticks remaining in cooldown */

uint16_t s_spike_ratio    = DEFAULT_SPIKE_RATIO;
uint8_t  s_min_activity   = DEFAULT_MIN_ACTIVITY;
uint16_t s_cooldown_ticks = DEFAULT_COOLDOWN_SEC;

template <typename T>
T clamp_range(int32_t v, T lo, T hi) {
  if (v < (int32_t)lo) return lo;
  if (v > (int32_t)hi) return hi;
  return (T)v;
}

int32_t abs8(int8_t b) { return (b < 0) ? -(int32_t)b : (int32_t)b; }

/* Fold the two identity-free aggregate slots into one 0..100 activity scalar.
 * The RSSI spread is halved so a single very loud frame can't dominate; the
 * dropped-frame estimate carries the "more traffic than we could sample" cue. */
uint8_t activity_of(const int8_t* v) {
  int32_t a = abs8(v[IDX_FRAMES_DROPPED]) + (abs8(v[IDX_RSSI_STD]) >> 1);
  if (a > 100) a = 100;
  return (uint8_t)a;
}

uint8_t ring_average(const uint8_t* ring) {
  uint32_t sum = 0;
  for (size_t i = 0; i < RING_LEN; ++i) sum += ring[i];
  return (uint8_t)(sum / RING_LEN);
}

/* AMBIENT is never persisted — the chokepoint routes it to the live UI only
 * (csi_event.cpp gates the witness commit on category != AMBIENT). */
constexpr uint8_t AMBIENT_CEILING_PER_HOUR = 0;  /* 0 = no cap; bundler still applies */

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "channel_active",
    /* allowed_fields */           CSI_FIELD_STATE_NAME
                                 | CSI_FIELD_TIME_BUCKET
                                 | CSI_FIELD_MOTION_SCORE,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ AMBIENT_CEILING_PER_HOUR,
  },
};

void on_init(const csi_module_settings_t* s) {
  s_spike_ratio    = clamp_range<uint16_t>(
      csi_module_settings_int(s, "wifi.channel_activity.spike_ratio",  DEFAULT_SPIKE_RATIO),
      MIN_SPIKE_RATIO, MAX_SPIKE_RATIO);
  s_min_activity   = clamp_range<uint8_t>(
      csi_module_settings_int(s, "wifi.channel_activity.min_activity", DEFAULT_MIN_ACTIVITY),
      MIN_FLOOR, MAX_FLOOR);
  s_cooldown_ticks = clamp_range<uint16_t>(
      csi_module_settings_int(s, "wifi.channel_activity.cooldown_sec", DEFAULT_COOLDOWN_SEC),
      MIN_COOLDOWN_SEC, MAX_COOLDOWN_SEC);

  memset(s_activity_ring, 0, sizeof(s_activity_ring));
  s_ring_head   = 0;
  s_warmup_left = BASELINE_PRIME_TICKS;
  s_cooldown    = 0;
}

void emit_channel_active(uint8_t intensity) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_AMBIENT;
  v.present_fields = CSI_FIELD_STATE_NAME
                   | CSI_FIELD_TIME_BUCKET
                   | CSI_FIELD_MOTION_SCORE;
  strncpy(v.state_name, "channel_active", sizeof(v.state_name) - 1);
  v.motion_score = intensity;
  (void)csi_event_emit("wifi.channel_activity", "channel_active", &v);
}

void on_tick(const csi_features_t* f) {
  if (!f) return;

  const uint8_t activity = activity_of(f->v);

  if (s_cooldown > 0) s_cooldown--;

  /* Warmup: fill the baseline ring without emitting. */
  if (s_warmup_left > 0) {
    s_activity_ring[s_ring_head] = activity;
    s_ring_head = (s_ring_head + 1) % RING_LEN;
    s_warmup_left--;
    return;
  }

  const uint8_t avg = ring_average(s_activity_ring);

  if (s_cooldown == 0
      && activity >= s_min_activity
      && (uint32_t)activity * 100u >= (uint32_t)avg * s_spike_ratio) {
    emit_channel_active(activity);
    s_cooldown = s_cooldown_ticks;
  }

  /* Append after the comparison so the current sample doesn't inflate the
   * baseline it was just compared against. */
  s_activity_ring[s_ring_head] = activity;
  s_ring_head = (s_ring_head + 1) % RING_LEN;
}

void on_deinit() {
  s_warmup_left = BASELINE_PRIME_TICKS;
  s_cooldown    = 0;
}

const csi_module_t MODULE = {
  /* id */                 "wifi.channel_activity",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             on_deinit,
};

}  /* namespace */

extern "C" const csi_module_t* wifi_channel_activity_module(void) { return &MODULE; }
