/**
 * @file core_activity_ribbon.cpp
 * @brief 96-bucket × 15-minute activity intensity ring.
 *
 * Each tick (~1 Hz) we update the *current* bucket's intensity using a
 * leaky-max so a brief peak survives long enough to land on the dashboard
 * but a quiet hour eventually decays. Bucket boundaries are anchored to
 * monotonic millis(); the host can correct to wall clock for display.
 *
 * Privacy: the ribbon is intensity-only (no state names, no rates, no
 * counts). The published event is `ribbon_bucket_advanced` which only
 * carries the new bucket index — no leak.
 */

#include "core_activity_ribbon.h"
#include "csi_event.h"

#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  static inline uint32_t ribbon_now_ms() { return millis(); }
#else
  #include <time.h>
  static inline uint32_t ribbon_now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
  }
#endif

namespace {

constexpr uint32_t BUCKET_MS = 15u * 60u * 1000u;   /* 15 minutes */

uint8_t  s_intensity[CSI_RIBBON_BUCKETS] = {};
uint16_t s_version = 0;
uint8_t  s_current_bucket = 0;
uint32_t s_bucket_started_ms = 0;

constexpr int IDX_DOPPLER_BASE      = 8;
constexpr int IDX_BREATHING_FFT_BASE = 12;

uint8_t reduce(const int8_t* v, int from, int to) {
  int32_t s = 0;
  for (int i = from; i < to; ++i) {
    int8_t b = v[i];
    s += (b < 0) ? -(int32_t)b : (int32_t)b;
  }
  const int n = to - from;
  if (n <= 0) return 0;
  int32_t avg = s / n;
  if (avg > 255) avg = 255;
  return (uint8_t)avg;
}

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "ribbon_bucket_advanced",
    /* allowed_fields */           CSI_FIELD_TIME_BUCKET,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 4,    /* one per 15 minutes */
  },
};

void emit_advance(uint8_t bucket) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  v.time_bucket    = bucket;
  (void)csi_event_emit("core.activity_ribbon", "ribbon_bucket_advanced", &v);
}

void on_init(const csi_module_settings_t* /*s*/) {
  /* Intensity buffer preserved across init() calls so a host that calls
   * deinit/init at runtime doesn't blank the ribbon. */
  s_bucket_started_ms = ribbon_now_ms();
}

void on_tick(const csi_features_t* f) {
  if (!f) return;
  const uint32_t now = ribbon_now_ms();
  const uint32_t elapsed = now - s_bucket_started_ms;
  if (elapsed >= BUCKET_MS) {
    /* Advance to the next bucket. Apply a gentle global decay so the ribbon
     * fades without writes being needed every tick. */
    for (size_t i = 0; i < CSI_RIBBON_BUCKETS; ++i) {
      uint8_t v = s_intensity[i];
      v = (uint8_t)((uint32_t)v * 250u / 256u);   /* ~2 % per bucket decay */
      s_intensity[i] = v;
    }
    s_current_bucket = (uint8_t)((s_current_bucket + 1) % CSI_RIBBON_BUCKETS);
    s_intensity[s_current_bucket] = 0;
    s_bucket_started_ms = now;
    s_version++;
    emit_advance(s_current_bucket);
  }

  const uint8_t motion    = reduce(f->v, IDX_DOPPLER_BASE,      IDX_DOPPLER_BASE + 4);
  const uint8_t breathing = reduce(f->v, IDX_BREATHING_FFT_BASE, IDX_BREATHING_FFT_BASE + 8);
  uint16_t combined = (uint16_t)motion + (breathing >> 1);  /* breathing weighted half */
  if (combined > 255) combined = 255;

  if (combined > s_intensity[s_current_bucket]) {
    s_intensity[s_current_bucket] = (uint8_t)combined;
    s_version++;
  }
}

void on_deinit() { /* keep ribbon contents */ }

const csi_module_t MODULE = {
  /* id */                 "core.activity_ribbon",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             on_deinit,
};

}  /* namespace */

extern "C" {

const csi_module_t* core_activity_ribbon_module(void) { return &MODULE; }

void core_activity_ribbon_snapshot(csi_ribbon_snapshot_t* out) {
  if (!out) return;
  memcpy(out->intensity, s_intensity, sizeof(s_intensity));
  out->version = s_version;
}

void core_activity_ribbon_load(const csi_ribbon_snapshot_t* in) {
  if (!in) return;
  memcpy(s_intensity, in->intensity, sizeof(s_intensity));
  s_version = in->version;
}

uint8_t core_activity_ribbon_current_bucket(uint32_t now_minutes_of_day) {
  return (uint8_t)((now_minutes_of_day / 15u) % CSI_RIBBON_BUCKETS);
}

}  /* extern "C" */
