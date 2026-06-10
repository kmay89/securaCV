/*
 * SecuraCV Canary — Shared die-temperature provider implementation
 *
 * See securacv_thermal.h for the single-instance rationale.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_thermal.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \
    defined(CONFIG_IDF_TARGET_ESP32C3)
  #define SECURACV_HAVE_TSENS 1
  #include <esp_idf_version.h>
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    #include <driver/temperature_sensor.h>
  #else
    extern "C" {
      #include <driver/temp_sensor.h>
    }
  #endif
  #include <esp_err.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
#else
  #define SECURACV_HAVE_TSENS 0
#endif

#if SECURACV_HAVE_TSENS

/* C++ magic static: thread-safe one-time creation, no init-order issues. */
static SemaphoreHandle_t lock_get() {
  static SemaphoreHandle_t l = xSemaphoreCreateMutex();
  return l;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

static temperature_sensor_handle_t s_handle = nullptr;
static bool s_install_failed = false;

static bool read_locked(float* out_c) {
  if (s_install_failed) return false;
  if (!s_handle) {
    /* 20–100 °C is the predefined ±2 °C band that covers normal
     * operation through overheat (camera pauses at 80 °C die). The
     * envsens drift detector compares deltas, so the absolute offset
     * of the band cancels out for it. */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&cfg, &s_handle) != ESP_OK) {
      s_handle = nullptr;
      s_install_failed = true;  /* don't retry every read */
      return false;
    }
    if (temperature_sensor_enable(s_handle) != ESP_OK) {
      temperature_sensor_uninstall(s_handle);
      s_handle = nullptr;
      s_install_failed = true;
      return false;
    }
  }
  float c = 0.0f;
  if (temperature_sensor_get_celsius(s_handle, &c) != ESP_OK) return false;
  *out_c = c;
  return true;
}

#else /* IDF 4.x */

static bool s_started = false;
static bool s_install_failed = false;

static bool read_locked(float* out_c) {
  if (s_install_failed) return false;
  if (!s_started) {
    temp_sensor_config_t cfg = TSENS_CONFIG_DEFAULT();
    cfg.dac_offset = TSENS_DAC_L2;
    if (temp_sensor_set_config(cfg) != ESP_OK || temp_sensor_start() != ESP_OK) {
      s_install_failed = true;
      return false;
    }
    s_started = true;
  }
  float c = 0.0f;
  if (temp_sensor_read_celsius(&c) != ESP_OK) return false;
  *out_c = c;
  return true;
}

#endif /* ESP_IDF_VERSION */

extern "C" bool thermal_read_die_c(float* out_c) {
  if (!out_c) return false;
  SemaphoreHandle_t l = lock_get();
  if (!l || xSemaphoreTake(l, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool ok = read_locked(out_c);
  xSemaphoreGive(l);
  return ok;
}

#else /* !SECURACV_HAVE_TSENS */

extern "C" bool thermal_read_die_c(float* out_c) {
  (void)out_c;
  return false;
}

#endif
