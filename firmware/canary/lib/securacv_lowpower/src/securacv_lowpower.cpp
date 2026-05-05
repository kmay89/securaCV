/*
 * SecuraCV Canary — Low-Power HAL implementation
 *
 * The implementation file is the ONE place esp_sleep_*() calls live.
 * Everything else in the firmware goes through securacv_lowpower.h.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_lowpower.h"

#include <Arduino.h>
#include <string.h>

#include "log_level.h"
#include "securacv_witness.h"   /* log_health() */

extern "C" {
  #include <esp_sleep.h>
  #include <esp_err.h>
  #include <driver/gpio.h>
  #include <driver/rtc_io.h>
}

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static uint8_t s_wake_reason_cache = 0;
static int     s_wake_touch_pad_cache = -1;
static bool    s_wake_cache_valid = false;

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

static uint8_t map_wake_cause(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:    return LOWPOWER_WAKE_TIMER;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return LOWPOWER_WAKE_TOUCH;
    case ESP_SLEEP_WAKEUP_EXT0:     return LOWPOWER_WAKE_EXT0;
    case ESP_SLEEP_WAKEUP_EXT1:     return LOWPOWER_WAKE_EXT1;
    case ESP_SLEEP_WAKEUP_ULP:      return LOWPOWER_WAKE_ULP;
    case ESP_SLEEP_WAKEUP_GPIO:     return LOWPOWER_WAKE_GPIO;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return LOWPOWER_WAKE_UNDEFINED;
    default:                        return LOWPOWER_WAKE_OTHER;
  }
}

static void cache_wake_state_once() {
  /* Cache on first read so a later subsystem touching ESP-IDF state
   * can't change the wakeup-cause result mid-boot. */
  if (s_wake_cache_valid) return;
  s_wake_reason_cache = map_wake_cause(esp_sleep_get_wakeup_cause());
  if (s_wake_reason_cache == LOWPOWER_WAKE_TOUCH) {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
    s_wake_touch_pad_cache = (int)esp_sleep_get_touchpad_wakeup_status();
#else
    s_wake_touch_pad_cache = -1;
#endif
  } else {
    s_wake_touch_pad_cache = -1;
  }
  s_wake_cache_valid = true;
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool lowpower_init(void) {
  if (s_initialized) return true;
  cache_wake_state_once();
  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Lowpower HAL initialized",
             lowpower_wake_reason_name(s_wake_reason_cache));
  return true;
}

uint32_t lowpower_get_caps(void) {
  uint32_t caps = LOWPOWER_CAP_TIMER | LOWPOWER_CAP_EXT0 | LOWPOWER_CAP_EXT1;
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  caps |= LOWPOWER_CAP_TOUCH | LOWPOWER_CAP_ULP_RISCV;
#elif defined(CONFIG_IDF_TARGET_ESP32)
  caps |= LOWPOWER_CAP_TOUCH | LOWPOWER_CAP_ULP_FSM;
#endif
  return caps;
}

uint8_t lowpower_get_wake_reason(void) {
  cache_wake_state_once();
  return s_wake_reason_cache;
}

const char* lowpower_wake_reason_name(uint8_t reason) {
  switch (reason) {
    case LOWPOWER_WAKE_UNDEFINED: return "cold_boot";
    case LOWPOWER_WAKE_TIMER:     return "timer";
    case LOWPOWER_WAKE_TOUCH:     return "touch";
    case LOWPOWER_WAKE_EXT0:      return "ext0_gpio";
    case LOWPOWER_WAKE_EXT1:      return "ext1_gpios";
    case LOWPOWER_WAKE_ULP:       return "ulp";
    case LOWPOWER_WAKE_GPIO:      return "gpio";
    case LOWPOWER_WAKE_OTHER:     return "other";
    default:                      return "unknown";
  }
}

int lowpower_get_wake_touch_pad(void) {
  cache_wake_state_once();
  return s_wake_touch_pad_cache;
}

bool lowpower_arm_wake_timer(uint64_t microseconds) {
  esp_err_t err = esp_sleep_enable_timer_wakeup(microseconds);
  if (err != ESP_OK) {
    char d[40]; snprintf(d, sizeof(d), "timer arm err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Lowpower: timer wake arm failed", d);
    return false;
  }
  return true;
}

bool lowpower_arm_wake_touch(void) {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  esp_err_t err = esp_sleep_enable_touchpad_wakeup();
  if (err != ESP_OK) {
    char d[40]; snprintf(d, sizeof(d), "touch arm err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Lowpower: touch wake arm failed", d);
    return false;
  }
  return true;
#else
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Lowpower: touch wake unsupported on this target", nullptr);
  return false;
#endif
}

bool lowpower_arm_wake_ext0(int gpio_num, int level) {
  /* EXT0 requires the GPIO to be RTC-domain. The S3 RTC GPIOs are
   * 0..21; we sanity-check here so the caller gets a clean false
   * instead of an obscure runtime panic. */
  if (gpio_num < 0 || gpio_num > 21) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Lowpower: ext0 gpio not in RTC domain", nullptr);
    return false;
  }
  /* rtc_gpio_init may fail silently on non-RTC pins on some IDF
   * versions; we check return on the wake call. */
  esp_err_t err = esp_sleep_enable_ext0_wakeup((gpio_num_t)gpio_num,
                                               level ? 1 : 0);
  if (err != ESP_OK) {
    char d[40]; snprintf(d, sizeof(d), "ext0 arm err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Lowpower: ext0 wake arm failed", d);
    return false;
  }
  return true;
}

bool lowpower_arm_wake_ext1(uint64_t gpio_mask, int any_or_all_mode) {
  /* any_or_all_mode: 0 = any high, 1 = all low. Maps to
   * ESP_EXT1_WAKEUP_ANY_HIGH / ESP_EXT1_WAKEUP_ALL_LOW. */
  esp_sleep_ext1_wakeup_mode_t m =
      (any_or_all_mode == 0) ? ESP_EXT1_WAKEUP_ANY_HIGH
                             : ESP_EXT1_WAKEUP_ALL_LOW;
  esp_err_t err = esp_sleep_enable_ext1_wakeup(gpio_mask, m);
  if (err != ESP_OK) {
    char d[40]; snprintf(d, sizeof(d), "ext1 arm err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Lowpower: ext1 wake arm failed", d);
    return false;
  }
  return true;
}

bool lowpower_arm_wake_ulp(void) {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  esp_err_t err = esp_sleep_enable_ulp_wakeup();
  if (err != ESP_OK) {
    char d[40]; snprintf(d, sizeof(d), "ulp arm err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Lowpower: ulp wake arm failed", d);
    return false;
  }
  return true;
#else
  return false;
#endif
}

void lowpower_disarm_all(void) {
  /* esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL) is the IDF
   * idiom for "clear everything." */
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

void lowpower_enter_deep_sleep(void) {
  log_health(LOG_LEVEL_NOTICE, LOG_CAT_SENSOR,
             "Lowpower: entering deep sleep", nullptr);
  /* Give the serial buffer a moment to flush so the operator sees the
   * notice on the console before USB-CDC drops. */
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
  /* Not reached. */
}

}  /* extern "C" */
