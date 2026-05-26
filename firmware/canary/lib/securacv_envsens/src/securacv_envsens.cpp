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

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \
    defined(CONFIG_IDF_TARGET_ESP32C3)
  #define SECURACV_HAVE_ENVSENS 1
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    #include <driver/temperature_sensor.h>
  #else
    extern "C" {
      #include <driver/temp_sensor.h>
    }
  #endif
  #include <esp_err.h>
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

static envsens_stats_t s_stats = {0};

/* ──────────────────────────────────────────────────────────────────────────
 * PERIPHERAL
 * ────────────────────────────────────────────────────────────────────────── */

#if SECURACV_HAVE_ENVSENS

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

static temperature_sensor_handle_t s_tsens_handle = nullptr;

static bool peripheral_open() {
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
  esp_err_t err = temperature_sensor_install(&cfg, &s_tsens_handle);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "install err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Envsens: temp install failed", d);
    return false;
  }
  err = temperature_sensor_enable(s_tsens_handle);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "enable err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Envsens: temp enable failed", d);
    temperature_sensor_uninstall(s_tsens_handle);
    s_tsens_handle = nullptr;
    return false;
  }
  return true;
}

static void peripheral_close() {
  if (s_tsens_handle) {
    temperature_sensor_disable(s_tsens_handle);
    temperature_sensor_uninstall(s_tsens_handle);
    s_tsens_handle = nullptr;
  }
}

static int16_t peripheral_read_t10() {
  if (!s_tsens_handle) return 0x7FFF;
  float c = 0.0f;
  esp_err_t err = temperature_sensor_get_celsius(s_tsens_handle, &c);
  if (err != ESP_OK) return 0x7FFF;
  if (c > 3000.0f) c = 3000.0f;
  if (c < -3000.0f) c = -3000.0f;
  return (int16_t)(c * 10.0f);
}

#else /* IDF 4.x */

static bool peripheral_open() {
  temp_sensor_config_t cfg = TSENS_CONFIG_DEFAULT();
  cfg.dac_offset = TSENS_DAC_L2;
  cfg.clk_div    = 6;
  esp_err_t err = temp_sensor_set_config(cfg);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "set_config err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Envsens: temp config failed", d);
    return false;
  }
  err = temp_sensor_start();
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "start err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Envsens: temp_sensor_start failed", d);
    return false;
  }
  return true;
}

static void peripheral_close() {
  temp_sensor_stop();
}

static int16_t peripheral_read_t10() {
  float c = 0.0f;
  esp_err_t err = temp_sensor_read_celsius(&c);
  if (err != ESP_OK) return 0x7FFF;
  if (c > 3000.0f) c = 3000.0f;
  if (c < -3000.0f) c = -3000.0f;
  return (int16_t)(c * 10.0f);
}

#endif /* ESP_IDF_VERSION */

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

  /* Drift check. */
  const int16_t delta = (int16_t)(t10 - s_baseline_t10);
  const int16_t mag   = (int16_t)(delta < 0 ? -delta : delta);
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
