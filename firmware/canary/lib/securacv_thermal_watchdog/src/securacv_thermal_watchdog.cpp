/*
 * SecuraCV Canary — Thermal Watchdog implementation
 *
 * Passive observer only. See securacv_thermal_watchdog.h for the contract.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_thermal_watchdog.h"

#ifdef CSI_TEST_HOST_BUILD
/* Host test build (g++, no Arduino/IDF): the test binary provides the
 * clock, the die-temp source, the NVS map, and the health-log capture. */
#include "thermal_wd_host_stubs.h"
#define FEATURE_THERMAL_WATCHDOG 1
#else
#include "canary_config.h"
#endif

#if defined(FEATURE_THERMAL_WATCHDOG) && FEATURE_THERMAL_WATCHDOG

#ifndef CSI_TEST_HOST_BUILD
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>

#include "securacv_thermal.h"
#include "securacv_witness.h"
#endif

namespace thermal_wd {

/* ──────────────────────────────────────────────────────────────────────────
 * TUNING
 * ────────────────────────────────────────────────────────────────────────── */

/* Idle cadence. A die-temp read costs microseconds; 30 s is plenty for
 * trend + history. The camera keeps its own 5 s cadence as the actuator. */
static const uint32_t WD_SAMPLE_MS = 30000;

/* Shadow classifier thresholds. Keep in sync with THERMAL_THROTTLE_TEMP_C /
 * THERMAL_PAUSE_TEMP_C / THERMAL_RECOVER_MARGIN_C in securacv_camera.h —
 * literals here so the watchdog never depends on the camera lib (it must
 * observe even in builds without FEATURE_CAMERA_PEEK). */
static const float WD_THROTTLE_C = 70.0f;
static const float WD_PAUSE_C    = 80.0f;
static const float WD_MARGIN_C   = 5.0f;

/* 5 °C above the pause threshold: the actuator's protective pause is not
 * holding — genuinely abnormal. (Espressif Tj max is 105 °C; this fires
 * with headroom left.) Clears 3 °C lower so one episode logs once. */
static const float WD_CRITICAL_C       = 85.0f;
static const float WD_CRITICAL_CLEAR_C = 82.0f;

/* Die runs above ambient, so die < 5 °C implies ambient near/below 0 °C —
 * LiPo charging risk (board ambient spec is −20…65 °C per Seeed). */
static const float WD_COLD_C       = 5.0f;
static const float WD_COLD_CLEAR_C = 7.0f;

/* 4 consecutive failed samples ≈ 2 min: long enough to skip a transient
 * mutex timeout, short enough to surface a dead sensor promptly. */
static const uint32_t WD_FAIL_THRESHOLD = 4;

/* Continuous shadow-PAUSED this long = the environment exceeds the
 * device's cooling capacity; throttling alone won't fix placement. */
static const uint32_t WD_SATURATION_MS = 600000;

/* >= 4 NORMAL->THROTTLED entries inside a rolling 30 min window means the
 * spot runs warm enough that the device adapts often. Guidance, not an
 * error — the 5 °C hysteresis already rules out true flapping. */
static const uint32_t WD_CYCLE_WINDOW_MS = 1800000;
static const uint32_t WD_CYCLE_COUNT     = 4;

/* Dirty-flag persist cadence (NVS wear). Matches the power lib's history
 * pattern. Critical episodes persist immediately (rare, worth the write);
 * new all-time min/max ride the 10 min cadence — a power cut may lose one,
 * and the next occurrence re-records it. */
static const uint32_t WD_PERSIST_MS = 600000;

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool                 s_initialized = false;
static bool                 s_dirty       = false;
static thermal_wd_state_t   s_state;
static thermal_wd_history_t s_history;

static uint32_t s_last_sample_ms   = 0;   /* last sample attempt            */
static uint32_t s_last_persist_ms  = 0;
static uint32_t s_paused_since_ms  = 0;   /* 0 = not currently paused       */
static uint32_t s_runtime_ms_accum   = 0; /* sub-minute accumulators        */
static uint32_t s_throttled_ms_accum = 0;
static uint32_t s_paused_ms_accum    = 0;

/* Ring of the last WD_CYCLE_COUNT NORMAL->THROTTLED entry times. */
static uint32_t s_throttle_entry_ms[WD_CYCLE_COUNT] = {0};
static uint8_t  s_throttle_entry_idx = 0;
static uint8_t  s_throttle_entry_cnt = 0;

enum ShadowState : uint8_t { WD_NORMAL = 0, WD_THROTTLED = 1, WD_PAUSED = 2 };

/* ──────────────────────────────────────────────────────────────────────────
 * NVS PERSISTENCE
 * ────────────────────────────────────────────────────────────────────────── */

static void load_nvs() {
  Preferences prefs;
  if (!prefs.begin("securacv", true)) return;
  s_history.alltime_min_c        = prefs.getChar("th_min_c", 127);
  s_history.alltime_max_c        = prefs.getChar("th_max_c", -128);
  s_history.total_runtime_min    = prefs.getUInt("th_rt_min", 0);
  s_history.throttled_min        = prefs.getUInt("th_thr_min", 0);
  s_history.paused_min           = prefs.getUInt("th_pau_min", 0);
  s_history.throttle_events      = prefs.getUInt("th_thr_cnt", 0);
  s_history.pause_events         = prefs.getUInt("th_pau_cnt", 0);
  s_history.critical_events      = prefs.getUInt("th_crit_cnt", 0);
  s_history.sensor_fail_events   = prefs.getUInt("th_fail_cnt", 0);
  s_history.cold_events          = prefs.getUInt("th_cold_cnt", 0);
  s_history.max_seen_runtime_min = prefs.getUInt("th_max_rt", 0);
  prefs.end();
}

static void save_nvs() {
  Preferences prefs;
  if (!prefs.begin("securacv", false)) return;
  prefs.putChar("th_min_c", s_history.alltime_min_c);
  prefs.putChar("th_max_c", s_history.alltime_max_c);
  prefs.putUInt("th_rt_min", s_history.total_runtime_min);
  prefs.putUInt("th_thr_min", s_history.throttled_min);
  prefs.putUInt("th_pau_min", s_history.paused_min);
  prefs.putUInt("th_thr_cnt", s_history.throttle_events);
  prefs.putUInt("th_pau_cnt", s_history.pause_events);
  prefs.putUInt("th_crit_cnt", s_history.critical_events);
  prefs.putUInt("th_fail_cnt", s_history.sensor_fail_events);
  prefs.putUInt("th_cold_cnt", s_history.cold_events);
  prefs.putUInt("th_max_rt", s_history.max_seen_runtime_min);
  prefs.end();
  s_dirty = false;
}

/* ──────────────────────────────────────────────────────────────────────────
 * SHADOW CLASSIFIER + ADVISORIES
 * ────────────────────────────────────────────────────────────────────────── */

static const char* shadow_name(uint8_t st) {
  switch (st) {
    case WD_THROTTLED: return "throttled";
    case WD_PAUSED:    return "paused";
    default:           return "normal";
  }
}

/* Same three-state hysteresis shape as CameraManager::checkThermal() —
 * entry at 70/80 °C, recovery 5 °C below — so the lifetime history counts
 * the same conditions the actuator responds to, camera running or not. */
static void update_shadow(float t, uint32_t now) {
  uint8_t prev = s_state.shadow_state;

  if (s_state.shadow_state == WD_PAUSED) {
    if (t < WD_PAUSE_C - WD_MARGIN_C) {
      s_state.shadow_state = WD_THROTTLED;
    }
  } else if (s_state.shadow_state == WD_THROTTLED) {
    if (t >= WD_PAUSE_C) {
      s_state.shadow_state = WD_PAUSED;
    } else if (t < WD_THROTTLE_C - WD_MARGIN_C) {
      s_state.shadow_state = WD_NORMAL;
    }
  } else {
    if (t >= WD_PAUSE_C) {
      s_state.shadow_state = WD_PAUSED;
    } else if (t >= WD_THROTTLE_C) {
      s_state.shadow_state = WD_THROTTLED;
    }
  }

  if (prev == s_state.shadow_state) return;

  /* Transitions are counted and printed, but NOT health-logged: while
   * streaming, the camera already health-logs its own (identical)
   * transitions and a second entry per event would be noise. Advisories
   * below are the watchdog's voice. */
  Serial.printf("[THERMAL] Watchdog: %s (die=%.1f°C)\n",
                shadow_name(s_state.shadow_state), t);

  if (prev == WD_NORMAL && s_state.shadow_state >= WD_THROTTLED) {
    s_history.throttle_events++;
    s_dirty = true;
    s_throttle_entry_ms[s_throttle_entry_idx] = now;
    s_throttle_entry_idx = (s_throttle_entry_idx + 1) % WD_CYCLE_COUNT;
    if (s_throttle_entry_cnt < WD_CYCLE_COUNT) s_throttle_entry_cnt++;
  }
  if (s_state.shadow_state == WD_PAUSED) {
    s_history.pause_events++;
    s_dirty = true;
    s_paused_since_ms = now;
  } else if (prev == WD_PAUSED) {
    s_paused_since_ms = 0;
    s_state.advisories &= ~THERMAL_ADV_SATURATION;
  }
}

static void update_advisories(float t, uint32_t now) {
  char detail[24];

  /* CRITICAL — protective pause is not holding. Persist immediately. */
  if (!(s_state.advisories & THERMAL_ADV_CRITICAL)) {
    if (t >= WD_CRITICAL_C) {
      s_state.advisories |= THERMAL_ADV_CRITICAL;
      s_history.critical_events++;
      s_dirty = true;
      snprintf(detail, sizeof(detail), "die=%.0fC", t);
      log_health(LOG_LEVEL_CRITICAL, LOG_CAT_SENSOR,
                 "Die temperature critical — beyond protective pause", detail);
      save_nvs();
    }
  } else if (t < WD_CRITICAL_CLEAR_C) {
    s_state.advisories &= ~THERMAL_ADV_CRITICAL;
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
               "Die temperature back below critical", nullptr);
  }

  /* SATURATION — continuously paused; placement exceeds cooling capacity. */
  if (s_paused_since_ms != 0 &&
      !(s_state.advisories & THERMAL_ADV_SATURATION) &&
      now - s_paused_since_ms >= WD_SATURATION_MS) {
    s_state.advisories |= THERMAL_ADV_SATURATION;
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Thermal saturation — environment exceeds cooling capacity",
               "check placement/heat sink");
  }

  /* ENV_LIMITED — adapting often; the spot runs warm. Guidance only. */
  bool cycling = false;
  if (s_throttle_entry_cnt >= WD_CYCLE_COUNT) {
    cycling = true;
    for (uint8_t i = 0; i < WD_CYCLE_COUNT; i++) {
      if (now - s_throttle_entry_ms[i] > WD_CYCLE_WINDOW_MS) { cycling = false; break; }
    }
  }
  if (cycling && !(s_state.advisories & THERMAL_ADV_ENV_LIMITED)) {
    s_state.advisories |= THERMAL_ADV_ENV_LIMITED;
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
               "Frequent thermal adaptation — placement runs warm",
               "shade/ventilation/heat sink");
  } else if (!cycling) {
    s_state.advisories &= ~THERMAL_ADV_ENV_LIMITED;
  }

  /* COLD — ambient near/below freezing; LiPo must not charge below 0 °C. */
  if (!(s_state.advisories & THERMAL_ADV_COLD)) {
    if (t < WD_COLD_C) {
      s_state.advisories |= THERMAL_ADV_COLD;
      s_history.cold_events++;
      s_dirty = true;
      snprintf(detail, sizeof(detail), "die=%.0fC", t);
      log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
                 "Cold environment — do not charge battery below freezing", detail);
    }
  } else if (t >= WD_COLD_CLEAR_C) {
    s_state.advisories &= ~THERMAL_ADV_COLD;
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
               "Temperature back above cold threshold", nullptr);
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * TIME-IN-STATE ACCOUNTING
 * ────────────────────────────────────────────────────────────────────────── */

/* Accumulate raw milliseconds and only divide when carrying whole minutes
 * into history — truncating to seconds per sample would silently drop up
 * to ~1 s every 30 s cycle and drift the lifetime counters. */
static void accumulate_time(uint32_t elapsed_ms) {
  if (elapsed_ms == 0) return;

  s_runtime_ms_accum += elapsed_ms;
  if (s_state.shadow_state >= WD_THROTTLED) s_throttled_ms_accum += elapsed_ms;
  if (s_state.shadow_state == WD_PAUSED)    s_paused_ms_accum += elapsed_ms;

  if (s_runtime_ms_accum >= 60000) {
    s_history.total_runtime_min += s_runtime_ms_accum / 60000;
    s_runtime_ms_accum %= 60000;
    s_dirty = true;
  }
  if (s_throttled_ms_accum >= 60000) {
    s_history.throttled_min += s_throttled_ms_accum / 60000;
    s_throttled_ms_accum %= 60000;
    s_dirty = true;
  }
  if (s_paused_ms_accum >= 60000) {
    s_history.paused_min += s_paused_ms_accum / 60000;
    s_paused_ms_accum %= 60000;
    s_dirty = true;
  }
}

}  /* namespace thermal_wd */

/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool thermal_wd_init(void) {
  using namespace thermal_wd;
  if (s_initialized) return true;

  memset(&s_state, 0, sizeof(s_state));
  s_state.sensor_ok = true;   /* optimistic until a failure episode */
  memset(&s_history, 0, sizeof(s_history));
  s_history.alltime_min_c = 127;
  s_history.alltime_max_c = -128;

  load_nvs();
  s_initialized = true;
  return true;
}

void thermal_wd_process(void) {
  using namespace thermal_wd;
  if (!s_initialized) return;

  uint32_t now = millis();
  if (s_last_sample_ms != 0 && now - s_last_sample_ms < WD_SAMPLE_MS) return;
  uint32_t elapsed = (s_last_sample_ms == 0) ? 0 : now - s_last_sample_ms;
  s_last_sample_ms = now;

  float t = 0.0f;
  if (!thermal_read_die_c(&t)) {
    s_state.consec_failures++;
    if (s_state.consec_failures == WD_FAIL_THRESHOLD) {
      /* Episode start: surface it — never silently lose the sensor. */
      s_state.sensor_ok = false;
      s_state.advisories |= THERMAL_ADV_SENSOR_FAULT;
      s_history.sensor_fail_events++;
      s_dirty = true;
      log_health(LOG_LEVEL_ERROR, LOG_CAT_SENSOR,
                 "Thermal sensor not responding", "watchdog");
    }
    /* Time still passes while blind — keep runtime honest. */
    accumulate_time(elapsed);
    return;
  }

  if (!s_state.sensor_ok) {
    s_state.sensor_ok = true;
    s_state.advisories &= ~THERMAL_ADV_SENSOR_FAULT;
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
               "Thermal sensor recovered", nullptr);
  }
  s_state.consec_failures = 0;
  s_state.die_temp_c = t;
  s_state.last_sample_ms = now;

  /* All-time extremes (whole degrees; the sensor is ±2 °C absolute). */
  int8_t ti = (int8_t)lroundf(t);
  if (ti > s_history.alltime_max_c) {
    s_history.alltime_max_c = ti;
    s_history.max_seen_runtime_min = s_history.total_runtime_min;
    s_dirty = true;
  }
  if (ti < s_history.alltime_min_c) {
    s_history.alltime_min_c = ti;
    s_dirty = true;
  }

  accumulate_time(elapsed);
  update_shadow(t, now);
  update_advisories(t, now);

  if (s_dirty && now - s_last_persist_ms >= WD_PERSIST_MS) {
    s_last_persist_ms = now;
    save_nvs();
  }
}

bool thermal_wd_get_state(thermal_wd_state_t* out) {
  using namespace thermal_wd;
  if (!s_initialized || !out) return false;
  /* Loop writes, httpd task reads — plain copy of 32-bit-or-smaller
   * fields, same torn-read assumption as the power lib's getters. */
  *out = s_state;
  return true;
}

bool thermal_wd_get_history(thermal_wd_history_t* out) {
  using namespace thermal_wd;
  if (!s_initialized || !out) return false;
  *out = s_history;
  return true;
}

void thermal_wd_persist(void) {
  using namespace thermal_wd;
  if (!s_initialized || !s_dirty) return;
  save_nvs();
}

#ifdef CSI_TEST_HOST_BUILD
/* Host-test-only: wipe all module state so each test starts from a
 * cold boot. Not declared in the public header — the test binary
 * declares it locally. */
void thermal_wd_test_reset(void) {
  using namespace thermal_wd;
  s_initialized = false;
  s_dirty = false;
  memset(&s_state, 0, sizeof(s_state));
  memset(&s_history, 0, sizeof(s_history));
  s_last_sample_ms = 0;
  s_last_persist_ms = 0;
  s_paused_since_ms = 0;
  s_runtime_ms_accum = 0;
  s_throttled_ms_accum = 0;
  s_paused_ms_accum = 0;
  memset(s_throttle_entry_ms, 0, sizeof(s_throttle_entry_ms));
  s_throttle_entry_idx = 0;
  s_throttle_entry_cnt = 0;
}
#endif

}  /* extern "C" */

#endif  /* FEATURE_THERMAL_WATCHDOG */
