/*
 * SecuraCV Canary — Environmental Sensors implementation
 *
 * Internal die-temperature drift detector. ESP32-S3 has a built-in
 * temperature sensor exposed via driver/temp_sensor.h in IDF 4.4
 * (replaced by driver/temperature_sensor.h in IDF 5.x — different
 * function names; gated below).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_envsens.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#include "log_level.h"
#include "securacv_witness.h"   /* log_health() */

#include <esp_idf_version.h>
#include "securacv_thermal.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \
    defined(CONFIG_IDF_TARGET_ESP32C3)
  #define SECURACV_HAVE_ENVSENS 1
#else
  #define SECURACV_HAVE_ENVSENS 0
#endif

namespace envsens {

static bool s_initialized = false;
static bool s_running     = false;
static envsens_config_t   s_cfg = ENVSENS_CONFIG_DEFAULT;
static envsens_event_cb_t s_cb  = nullptr;

/* Sampling state. */
static uint32_t s_last_sample_ms = 0;
static uint32_t s_last_event_ms  = 0;

/* Baseline accumulator, in tenths of a °C so we don't drag a float
 * through state. A tenth-degree resolution is fine — the drift
 * threshold itself is 5 °C. */
static int32_t  s_baseline_sum  = 0;
static uint8_t  s_baseline_count = 0;
static int16_t  s_baseline_t10  = 0;
static bool     s_baseline_locked = false;

static int16_t  s_last_t10  = 0;
static bool     s_have_last = false;

/* High-load (camera streaming etc.) gate. Heavy radio/camera work heats
 * the die 10–20 °C — by temperature alone that's indistinguishable from
 * the heat-gun tamper signature. While a declared high-load phase is
 * active (plus a cooldown after it ends, so the cool-down ramp doesn't
 * fire either), the baseline fast-tracks the temperature and detection
 * is suspended. */
static bool     s_high_load = false;
static uint32_t s_high_load_until_ms = 0;   /* cooldown deadline after load ends */
static const uint32_t HIGH_LOAD_COOLDOWN_MS = 10UL * 60UL * 1000UL;

static envsens_stats_t s_stats = {0};

/* ──────────────────────────────────────────────────────────────────────────
 * PERIPHERAL
 * ────────────────────────────────────────────────────────────────────────── */

#if SECURACV_HAVE_ENVSENS

/* All reads go through the shared securacv_thermal provider — ESP-IDF 5.x
 * permits only ONE installed temperature-sensor driver instance, and the
 * camera thermal throttle + diagnostics self-test read the same sensor.
 * Installing our own handle here (as this lib originally did) made
 * whichever consumer initialized second fail silently. */

static bool peripheral_open() {
  float c = 0.0f;
  if (!thermal_read_die_c(&c)) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Envsens: temp sensor unavailable", nullptr);
    return false;
  }
  return true;
}

static void peripheral_close() {
  /* Intentionally nothing: the shared driver stays installed for the
   * camera thermal throttle and the diagnostics self-test. */
}

static int16_t peripheral_read_t10() {
  float c = 0.0f;
  if (!thermal_read_die_c(&c)) return 0x7FFF;
  if (c > 3000.0f) c = 3000.0f;
  if (c < -3000.0f) c = -3000.0f;
  return (int16_t)(c * 10.0f);
}

#else  /* !SECURACV_HAVE_ENVSENS */

static bool peripheral_open()  { return false; }
static void peripheral_close() {}
static int16_t peripheral_read_t10() { return 0x7FFF; }

#endif

/* ──────────────────────────────────────────────────────────────────────────
 * STATE MACHINE
 * ────────────────────────────────────────────────────────────────────────── */

static void emit_event(uint8_t conf, uint32_t now_ms) {
  envsens_event_t e = {};
  e.event_type  = ENVSENS_EVENT_TEMP_DRIFT;
  e.confidence  = conf;
  e.time_bucket = (uint8_t)((now_ms / (10UL * 60UL * 1000UL)) % 144);
  if (s_cb) s_cb(&e);
  s_stats.drift_events++;
  s_last_event_ms = now_ms;
  /* Don't outlive the call. */
  memset(&e, 0, sizeof(e));
}

static void on_sample(int16_t t10, uint32_t now_ms) {
  s_last_t10 = t10;
  s_have_last = true;
  s_stats.samples_taken++;
  s_stats.last_c_rounded = (int8_t)((t10 + (t10 >= 0 ? 5 : -5)) / 10);

  if (!s_baseline_locked) {
    s_baseline_sum += t10;
    s_baseline_count++;
    if (s_baseline_count >= s_cfg.baseline_samples) {
      s_baseline_t10 = (int16_t)(s_baseline_sum / s_baseline_count);
      s_baseline_locked = true;
      s_stats.baseline_locked = true;
      s_stats.baseline_c_rounded = (int8_t)
          ((s_baseline_t10 + (s_baseline_t10 >= 0 ? 5 : -5)) / 10);
      log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                 "Envsens: temp baseline locked", nullptr);
    }
    return;
  }

  const int16_t delta = (int16_t)(t10 - s_baseline_t10);

  /* High-load phase (or its cooldown): fast-track the baseline (alpha
   * 1/2 per sample) and suspend detection — the self-heating ramp and
   * the subsequent cool-down are expected, not tamper. */
  if (s_high_load || (int32_t)(now_ms - s_high_load_until_ms) < 0) {
    int16_t adj = (int16_t)(delta / 2);
    if (adj == 0 && delta != 0) adj = (delta > 0) ? 1 : -1;
    s_baseline_t10 = (int16_t)(s_baseline_t10 + adj);
    s_stats.baseline_c_rounded = (int8_t)
        ((s_baseline_t10 + (s_baseline_t10 >= 0 ? 5 : -5)) / 10);
    return;
  }

  /* Drift check. */
  const int16_t mag = (int16_t)(delta < 0 ? -delta : delta);
  if (mag >= (int16_t)s_cfg.drift_threshold_tenths_c) {
    /* Suppress runaway events while the drift is sustained. */
    if ((now_ms - s_last_event_ms) >= s_cfg.suppress_ms) {
      /* Confidence scales linearly with the drift up to 2× threshold. */
      const int32_t denom = (int32_t)s_cfg.drift_threshold_tenths_c;
      int32_t conf = 50 + ((int32_t)mag - denom) * 50 / denom;
      if (conf > 100) conf = 100;
      if (conf < 50)  conf = 50;
      emit_event((uint8_t)conf, now_ms);
    }
  } else {
    /* Slow EMA adaptation (alpha 1/16 per 60 s sample ≈ 16 min time
     * constant): absorbs gradual radio self-heating and HVAC drift so a
     * cold-boot baseline doesn't turn normal workload warm-up into a
     * false tamper event. Sub-threshold drift adapts; a ≥ threshold
     * step fires above instead of adapting — the tamper signature is
     * the fast step, not the slow trend. (This makes the baseline the
     * EMA the header always documented; it was previously frozen at
     * the boot-time mean, which guaranteed a false positive once the
     * die warmed ≥ 5 °C under normal load.) */
    int16_t adj = (int16_t)(delta / 16);
    if (adj == 0 && delta != 0) adj = (delta > 0) ? 1 : -1;
    s_baseline_t10 = (int16_t)(s_baseline_t10 + adj);
    s_stats.baseline_c_rounded = (int8_t)
        ((s_baseline_t10 + (s_baseline_t10 >= 0 ? 5 : -5)) / 10);
  }
}

}  /* namespace envsens */

/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool envsens_init(const envsens_config_t* cfg) {
  using namespace envsens;
  if (s_initialized) return true;
  s_cfg = cfg ? *cfg : (envsens_config_t)ENVSENS_CONFIG_DEFAULT;
  if (s_cfg.sample_interval_ms == 0)
    s_cfg.sample_interval_ms = 60000;
  if (s_cfg.drift_threshold_tenths_c == 0)
    s_cfg.drift_threshold_tenths_c = 50;
  if (s_cfg.suppress_ms == 0)
    s_cfg.suppress_ms = 300000;
  if (s_cfg.baseline_samples == 0)
    s_cfg.baseline_samples = 5;

  s_baseline_sum = 0;
  s_baseline_count = 0;
  s_baseline_locked = false;
  s_have_last = false;
  s_last_sample_ms = 0;
  s_last_event_ms = 0;
  s_high_load = false;
  s_high_load_until_ms = 0;
  memset(&s_stats, 0, sizeof(s_stats));

  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Envsens HAL initialized",
             "ESP32-S3 internal temp sensor");
  return true;
}

void envsens_deinit(void) {
  using namespace envsens;
  if (!s_initialized) return;
  if (s_running) envsens_stop();
  s_cb = nullptr;
  s_initialized = false;
}

bool envsens_start(void) {
  using namespace envsens;
  if (!s_initialized) return false;
  if (s_running) return true;
  if (!peripheral_open()) return false;
  s_running = true;
  s_last_sample_ms = millis() - s_cfg.sample_interval_ms;  /* sample on first tick */
  return true;
}

void envsens_stop(void) {
  using namespace envsens;
  if (!s_running) return;
  peripheral_close();
  s_running = false;
}

bool envsens_is_running(void) { return envsens::s_running; }

void envsens_set_event_callback(envsens_event_cb_t cb) {
  envsens::s_cb = cb;
}

void envsens_set_high_load(bool active) {
  using namespace envsens;
  if (s_high_load && !active) {
    /* Falling edge: hold detection through the cool-down ramp too. */
    s_high_load_until_ms = millis() + HIGH_LOAD_COOLDOWN_MS;
  }
  s_high_load = active;
}

bool envsens_process(void) {
  using namespace envsens;
  if (!s_running) return false;
  const uint32_t now = millis();
  if ((now - s_last_sample_ms) < s_cfg.sample_interval_ms) return false;
  s_last_sample_ms = now;

  const int16_t t10 = peripheral_read_t10();
  if (t10 == 0x7FFF) return false;  /* read error — try again next tick */
  on_sample(t10, now);
  return true;
}

bool envsens_get_stats(envsens_stats_t* out) {
  using namespace envsens;
  if (!out) return false;
  *out = s_stats;
  return true;
}

}  /* extern "C" */
