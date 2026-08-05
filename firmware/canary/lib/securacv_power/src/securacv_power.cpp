/*
 * SecuraCV Canary — Power Management implementation
 *
 * Dual-mode battery monitoring: hardware ADC with voltage divider on
 * GPIO 1 (primary) or software-based trend inference (fallback).
 *
 * The ESP32-S3's ADC1 is 12-bit (0–4095) with a 0–3.3 V range.
 * With a 2:1 divider the full LiPo voltage window (3.0–4.2 V) maps
 * to 1.5–2.1 V — comfortably within the ADC's linear region.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_power.h"

#include <Arduino.h>
#include <string.h>
#include <Preferences.h>

#include "log_level.h"
#include "securacv_witness.h"
#include "securacv_crypto.h"

#include <esp_adc_cal.h>
#include <esp_idf_version.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #include <esp_adc/adc_oneshot.h>
  #include <esp_adc/adc_cali.h>
  #include <esp_adc/adc_cali_scheme.h>
#else
  #include <driver/adc.h>
#endif

#include <esp_sleep.h>
#include <esp_system.h>
#include "securacv_lowpower.h"

/* Pure battery decision logic (SoC curve, charge-state classification,
 * health/runtime estimates). Canonical at firmware/common/power/, on the
 * include path via platformio.ini's -I${PROJECT_DIR}/../common. Host-
 * tested in firmware/common/power/test_power_logic.cpp (CI). */
#include "power/power_logic.h"

namespace power {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

static constexpr uint16_t DIVIDER_DETECT_MIN_MV = 1200;
static constexpr uint16_t DIVIDER_DETECT_MAX_MV = 2300;

/* Valid LiPo voltage range. Readings outside this with a divider
 * present indicate no battery is connected (USB-only power, or
 * VBUS bleeding through the charger IC to the battery pads).
 * The presence window itself lives in power_logic.h. */
static constexpr uint16_t LIPO_VALID_MIN_MV = power_logic::LIPO_VALID_MIN_MV;
static constexpr uint16_t LIPO_VALID_MAX_MV = 4350;

/* Number of consecutive out-of-range readings before declaring
 * no battery present (avoids transient glitches on hot-plug). */
static constexpr uint8_t  NO_BATTERY_CONFIRM_COUNT = 5;

static constexpr uint8_t  MEDIAN_WINDOW = 16;

/* LiPo discharge curve lives in power_logic.h (power_logic::SOC_TABLE):
 * typical single-cell LiPo at ~0.2C discharge rate, 25 °C. */

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static bool s_running      = false;
static power_config_t   s_cfg = POWER_CONFIG_DEFAULT;
static power_event_cb_t s_cb  = nullptr;
static power_state_t    s_state = {};

/* ADC */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static adc_oneshot_unit_handle_t s_adc_handle = nullptr;
static adc_cali_handle_t         s_cali_handle = nullptr;
#else
static esp_adc_cal_characteristics_t s_adc_chars;
static bool s_adc_cal_valid = false;
#endif
static bool s_adc_ready = false;

/* Sampling */
static uint32_t s_last_sample_ms = 0;
static uint16_t s_median_buf[MEDIAN_WINDOW];
static uint8_t  s_median_idx = 0;
static uint8_t  s_median_count = 0;

/* Trend tracking (for software inference) */
static constexpr uint8_t TREND_SAMPLES = 60;
static uint16_t s_trend_ring[TREND_SAMPLES];
static uint8_t  s_trend_idx = 0;
static uint8_t  s_trend_count = 0;

/* Charge cycle detection */
static bool s_was_below_cycle_threshold = false;
#if !FEATURE_DEEP_SLEEP
/* One-shot latch for power_graceful_shutdown() under the never-sleep contract
 * (FEATURE_DEEP_SLEEP=0). Cleared on battery recovery in update_charge_state(). */
static bool s_shutdown_latched = false;
#endif
static constexpr uint16_t CYCLE_LOW_MV  = 3800;
static constexpr uint16_t CYCLE_HIGH_MV = 4100;

/* No-battery detection */
static uint8_t s_no_battery_count = 0;

/* Battery health history (NVS-persisted) */
static power_history_t s_history = {};
static uint32_t s_runtime_accum_ms = 0;    /* sub-minute accumulator */
static uint32_t s_runtime_last_ms  = 0;    /* last millis() for runtime tracking */

/* ──────────────────────────────────────────────────────────────────────────
 * ADC PERIPHERAL
 * ────────────────────────────────────────────────────────────────────────── */

/* GPIO -> ADC1 channel. The mapping is per-silicon, not universal: ADC1 has
 * ten channels on S3/S2 (GPIO1-10), five on C3 (GPIO0-4), and eight on the
 * classic ESP32 — where they sit on GPIO32-39, in an order that is NOT
 * ascending. Compiling the S3 table on another target is a hard error
 * (ADC1_CHANNEL_8/9 simply don't exist there), which is how the classic
 * ESP32 ports found this.
 *
 * `default:` returns channel 0 on every target. That is the "board declared
 * no usable VBAT pin" path, and it is safe by construction: adc_read_mv()
 * only trusts a reading inside the divider-detect window, so an unwired or
 * input-only pin reads as "no divider present" and the library reports
 * USB-only mode rather than inventing a battery. */
static adc1_channel_t gpio_to_adc1_channel(uint8_t gpio) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  /* Classic ESP32: ADC1 is on the input-only + RTC block, GPIO32-39. */
  switch (gpio) {
    case 36: return ADC1_CHANNEL_0;
    case 37: return ADC1_CHANNEL_1;
    case 38: return ADC1_CHANNEL_2;
    case 39: return ADC1_CHANNEL_3;
    case 32: return ADC1_CHANNEL_4;
    case 33: return ADC1_CHANNEL_5;
    case 34: return ADC1_CHANNEL_6;
    case 35: return ADC1_CHANNEL_7;
    default: return ADC1_CHANNEL_0;
  }
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  /* C3: ADC1 channels 0-4 on GPIO0-4 (ADC2 exists but is Wi-Fi-hostile). */
  switch (gpio) {
    case 0:  return ADC1_CHANNEL_0;
    case 1:  return ADC1_CHANNEL_1;
    case 2:  return ADC1_CHANNEL_2;
    case 3:  return ADC1_CHANNEL_3;
    case 4:  return ADC1_CHANNEL_4;
    default: return ADC1_CHANNEL_0;
  }
#else
  /* S3 / S2: ADC1 channels 0-9 on GPIO1-10. The XIAO's VBAT is GPIO1. */
  switch (gpio) {
    case 1:  return ADC1_CHANNEL_0;
    case 2:  return ADC1_CHANNEL_1;
    case 3:  return ADC1_CHANNEL_2;
    case 4:  return ADC1_CHANNEL_3;
    case 5:  return ADC1_CHANNEL_4;
    case 6:  return ADC1_CHANNEL_5;
    case 7:  return ADC1_CHANNEL_6;
    case 8:  return ADC1_CHANNEL_7;
    case 9:  return ADC1_CHANNEL_8;
    case 10: return ADC1_CHANNEL_9;
    default: return ADC1_CHANNEL_0;
  }
#endif
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

static bool adc_open() {
  adc_oneshot_unit_init_cfg_t unit_cfg = {
    .unit_id = ADC_UNIT_1,
  };
  esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
  if (err != ESP_OK) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Power: ADC unit init failed", nullptr);
    return false;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  adc_channel_t ch = (adc_channel_t)gpio_to_adc1_channel(s_cfg.adc_gpio);
  err = adc_oneshot_config_channel(s_adc_handle, ch, &chan_cfg);
  if (err != ESP_OK) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Power: ADC channel config failed", nullptr);
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = nullptr;
    return false;
  }

  adc_cali_curve_fitting_config_t cali_cfg = {
    .unit_id = ADC_UNIT_1,
    .chan = ch,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
  if (err != ESP_OK) {
    s_cali_handle = nullptr;
  }

  return true;
}

static void adc_close() {
  if (s_cali_handle) {
    adc_cali_delete_scheme_curve_fitting(s_cali_handle);
    s_cali_handle = nullptr;
  }
  if (s_adc_handle) {
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = nullptr;
  }
}

static uint16_t adc_read_mv() {
  if (!s_adc_handle) return 0;
  int raw = 0;
  adc_channel_t ch = (adc_channel_t)gpio_to_adc1_channel(s_cfg.adc_gpio);
  esp_err_t err = adc_oneshot_read(s_adc_handle, ch, &raw);
  if (err != ESP_OK) return 0;

  if (s_cali_handle) {
    int mv = 0;
    err = adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
    if (err == ESP_OK) return (uint16_t)mv;
  }
  return (uint16_t)((raw * 3300) / 4095);
}

#else /* IDF 4.x */

static bool adc_open() {
  adc1_channel_t ch = gpio_to_adc1_channel(s_cfg.adc_gpio);
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ch, ADC_ATTEN_DB_12);
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
      ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &s_adc_chars);
  s_adc_cal_valid = (val_type != ESP_ADC_CAL_VAL_NOT_SUPPORTED);
  return true;
}

static void adc_close() {
  s_adc_cal_valid = false;
}

static uint16_t adc_read_mv() {
  adc1_channel_t ch = gpio_to_adc1_channel(s_cfg.adc_gpio);
  int raw = adc1_get_raw(ch);
  if (raw < 0) return 0;
  if (s_adc_cal_valid) {
    return (uint16_t)esp_adc_cal_raw_to_voltage((uint32_t)raw, &s_adc_chars);
  }
  return (uint16_t)((raw * 3300) / 4095);
}

#endif /* ESP_IDF_VERSION */

/* ──────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

static uint16_t compute_median(const uint16_t* buf, uint8_t count) {
  if (count == 0) return 0;
  uint16_t sorted[MEDIAN_WINDOW];
  memcpy(sorted, buf, count * sizeof(uint16_t));
  for (uint8_t i = 0; i < count - 1; i++) {
    for (uint8_t j = i + 1; j < count; j++) {
      if (sorted[j] < sorted[i]) {
        uint16_t tmp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = tmp;
      }
    }
  }
  return sorted[count / 2];
}

static int16_t compute_trend_mv_per_min() {
  if (s_trend_count < 10) return 0;
  uint8_t oldest_idx = (s_trend_idx + TREND_SAMPLES - s_trend_count) % TREND_SAMPLES;
  int32_t oldest = s_trend_ring[oldest_idx];
  int32_t newest = s_trend_ring[(s_trend_idx + TREND_SAMPLES - 1) % TREND_SAMPLES];
  uint32_t span_ms = (uint32_t)s_trend_count * s_cfg.sample_interval_ms;
  if (span_ms == 0) return 0;
  return (int16_t)(((newest - oldest) * 60000) / (int32_t)span_ms);
}

static uint8_t time_bucket_now() {
  return (uint8_t)((millis() / (10UL * 60UL * 1000UL)) % 144);
}

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT EMISSION
 * ────────────────────────────────────────────────────────────────────────── */

static void emit_event(power_event_type_t type) {
  if (!s_cb) return;
  power_event_t e = {};
  e.event_type   = (uint8_t)type;
  e.charge_state = s_state.charge_state;
  e.soc_pct      = s_state.soc_pct;
  e.time_bucket  = time_bucket_now();
  s_cb(&e);
}

/* ──────────────────────────────────────────────────────────────────────────
 * CHARGE STATE MACHINE
 * ────────────────────────────────────────────────────────────────────────── */

static void check_battery_presence(uint16_t battery_mv) {
  /* Keep running as long as a divider is wired -- monitor_mode may have
   * flipped to USB_ONLY after a battery removal, and this is the only
   * path that can detect the battery being reattached. */
  if (!s_state.divider_detected) return;

  bool out_of_range = !power_logic::battery_voltage_in_range(battery_mv);
  if (out_of_range) {
    if (s_no_battery_count < NO_BATTERY_CONFIRM_COUNT) {
      s_no_battery_count++;
    }
    if (s_no_battery_count >= NO_BATTERY_CONFIRM_COUNT && s_state.battery_present) {
      s_state.battery_present = false;
      s_state.charge_state = CHARGE_STATE_NO_BATTERY;
      s_state.power_source = POWER_SOURCE_USB_ONLY;
      s_state.soc_pct = 0;
      s_state.monitor_mode = POWER_MODE_USB_ONLY;
      log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                 "Power: no battery detected", "USB-only mode");
      emit_event(POWER_EVENT_STATE_CHANGE);
    }
  } else {
    s_no_battery_count = 0;
    if (!s_state.battery_present) {
      s_state.battery_present = true;
      s_state.monitor_mode = POWER_MODE_HW_ADC;
      /* Clear the latched NO_BATTERY state so the charge state machine
       * re-classifies from fresh trend data instead of staying stuck. */
      s_state.charge_state = CHARGE_STATE_UNKNOWN;
      s_state.power_source = POWER_SOURCE_UNKNOWN;
      log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                 "Power: battery detected", "HW ADC mode");
    }
  }
}

static void update_charge_state(uint16_t battery_mv) {
  if (s_state.charge_state == CHARGE_STATE_NO_BATTERY) return;

  charge_state_t prev = (charge_state_t)s_state.charge_state;
  charge_state_t next = prev;
  int16_t trend = s_state.trend_mv_per_min;

  if (s_state.monitor_mode == POWER_MODE_HW_ADC) {
    /* Charging detection runs BEFORE the low/critical SoC checks so a
     * depleted battery on USB reports CHARGING (power source USB) --
     * ordering pinned by firmware/common/power/test_power_logic.cpp. */
    next = (charge_state_t)power_logic::classify_hw_charge_state(
        battery_mv, s_state.soc_pct, trend,
        s_cfg.soc_low_pct, s_cfg.soc_critical_pct);
  }

  /* Hysteresis: require two consecutive readings in the same state
   * for LOW/CRITICAL transitions to avoid flapping. */
  static charge_state_t s_pending = CHARGE_STATE_UNKNOWN;
  static uint8_t s_pending_count = 0;
  if (next != prev) {
    if (next == s_pending) {
      s_pending_count++;
    } else {
      s_pending = next;
      s_pending_count = 1;
    }
    uint8_t threshold = (next == CHARGE_STATE_LOW ||
                         next == CHARGE_STATE_CRITICAL) ? 3 : 2;
    if (s_pending_count < threshold) return;
  }
  s_pending = CHARGE_STATE_UNKNOWN;
  s_pending_count = 0;

  if (next == prev) return;
  s_state.charge_state = next;

#if !FEATURE_DEEP_SLEEP
  /* Recovery clears the graceful-shutdown latch so a later critical episode
   * (after a charge) re-emits the shutdown attestation exactly once again. */
  if (next != CHARGE_STATE_CRITICAL) s_shutdown_latched = false;
#endif

  /* Derive power source from charge state. */
  if (next == CHARGE_STATE_CHARGING || next == CHARGE_STATE_FULL) {
    s_state.power_source = POWER_SOURCE_USB;
  } else {
    s_state.power_source = POWER_SOURCE_BATTERY;
  }

  /* Emit events. */
  emit_event(POWER_EVENT_STATE_CHANGE);
  if (next == CHARGE_STATE_LOW)
    emit_event(POWER_EVENT_LOW_BATTERY);
  if (next == CHARGE_STATE_CRITICAL)
    emit_event(POWER_EVENT_CRITICAL_BATTERY);
  if (next == CHARGE_STATE_CHARGING && prev != CHARGE_STATE_CHARGING)
    emit_event(POWER_EVENT_CHARGING_START);
  if (next == CHARGE_STATE_FULL && prev == CHARGE_STATE_CHARGING)
    emit_event(POWER_EVENT_CHARGING_DONE);

  const char* name = "unknown";
  switch (next) {
    case CHARGE_STATE_CHARGING:    name = "charging"; break;
    case CHARGE_STATE_FULL:        name = "full"; break;
    case CHARGE_STATE_DISCHARGING: name = "discharging"; break;
    case CHARGE_STATE_LOW:         name = "low"; break;
    case CHARGE_STATE_CRITICAL:    name = "critical"; break;
    case CHARGE_STATE_NO_BATTERY:  name = "no_battery"; break;
    default: break;
  }
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Power: charge state changed", name);
}

/* ──────────────────────────────────────────────────────────────────────────
 * CHARGE CYCLE TRACKING
 * ────────────────────────────────────────────────────────────────────────── */

static void track_charge_cycle(uint16_t battery_mv) {
  if (battery_mv < CYCLE_LOW_MV) {
    s_was_below_cycle_threshold = true;
  } else if (battery_mv > CYCLE_HIGH_MV && s_was_below_cycle_threshold) {
    s_was_below_cycle_threshold = false;
    s_state.charge_cycles++;
    Preferences prefs;
    if (prefs.begin("securacv", false)) {
      prefs.putUInt("batt_cycles", s_state.charge_cycles);
      prefs.end();
    }
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * NVS PERSISTENCE
 * ────────────────────────────────────────────────────────────────────────── */

static void load_nvs_state() {
  Preferences prefs;
  if (prefs.begin("securacv", true)) {
    s_state.charge_cycles = prefs.getUInt("batt_cycles", 0);
    s_state.capacity_mah = prefs.getUShort("batt_cap", s_cfg.capacity_mah);
    s_state.max_voltage_mv = prefs.getUShort("batt_max_mv", 0);
    s_state.min_voltage_mv = prefs.getUShort("batt_min_mv", 0xFFFF);

    /* Battery health history fields */
    s_history.charge_cycles     = s_state.charge_cycles;
    s_history.total_runtime_min = prefs.getUInt("batt_rt_min", 0);
    s_history.voltage_min_mv    = s_state.min_voltage_mv;
    s_history.voltage_max_mv    = s_state.max_voltage_mv;
    s_history.soc_min_pct       = prefs.getUChar("batt_soc_min", 100);
    s_history.brownout_count    = prefs.getUInt("batt_bo_cnt", 0);
    s_history.last_full_charge_ms = prefs.getUInt("batt_fc_ms", 0);

    prefs.end();
  }

  /* Detect brownout reset and increment count. Persist immediately:
   * a device in a brownout loop may never reach the periodic history
   * save, and the count is the main field-diagnosis signal. */
  {
    esp_reset_reason_t rst = esp_reset_reason();
    if (rst == ESP_RST_BROWNOUT) {
      s_history.brownout_count++;
      Preferences bo;
      if (bo.begin("securacv", false)) {
        bo.putUInt("batt_bo_cnt", s_history.brownout_count);
        bo.end();
      }
    }
  }
}

static void save_voltage_extremes() {
  static uint32_t s_last_persist_ms = 0;
  uint32_t now = millis();
  if (now - s_last_persist_ms < 300000) return;
  s_last_persist_ms = now;
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putUShort("batt_max_mv", s_state.max_voltage_mv);
    prefs.putUShort("batt_min_mv", s_state.min_voltage_mv);
    prefs.end();
  }
}

}  /* namespace power */


/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool power_init(const power_config_t* cfg) {
  using namespace power;
  if (s_initialized) return true;
  s_cfg = cfg ? *cfg : (power_config_t)POWER_CONFIG_DEFAULT;

  memset(&s_state, 0, sizeof(s_state));
  s_state.min_voltage_mv = 0xFFFF;
  s_state.capacity_mah = s_cfg.capacity_mah;

  s_median_idx = 0;
  s_median_count = 0;
  s_trend_idx = 0;
  s_trend_count = 0;
  s_was_below_cycle_threshold = false;
  s_no_battery_count = 0;

  load_nvs_state();

  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Power HAL initialized", "HW ADC with USB-only fallback");
  return true;
}

void power_deinit(void) {
  using namespace power;
  if (!s_initialized) return;
  if (s_running) power_stop();
  s_cb = nullptr;
  s_initialized = false;
}

bool power_start(void) {
  using namespace power;
  if (!s_initialized) return false;
  if (s_running) return true;

  if (!adc_open()) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Power: ADC open failed", nullptr);
    return false;
  }
  s_adc_ready = true;

  /* Auto-detect divider presence: take a few readings and check range. */
  uint32_t sum = 0;
  uint8_t valid = 0;
  for (int i = 0; i < 8; i++) {
    uint16_t mv = adc_read_mv();
    if (mv > 0) { sum += mv; valid++; }
    delayMicroseconds(500);
  }

  if (valid > 0) {
    uint16_t avg_mv = (uint16_t)(sum / valid);
    if (avg_mv >= DIVIDER_DETECT_MIN_MV && avg_mv <= DIVIDER_DETECT_MAX_MV) {
      /* Divider present. Check if the battery voltage makes sense. */
      uint16_t batt_mv = (uint16_t)(avg_mv * s_cfg.divider_ratio);
      if (batt_mv >= LIPO_VALID_MIN_MV && batt_mv <= LIPO_VALID_MAX_MV) {
        s_state.monitor_mode = POWER_MODE_HW_ADC;
        s_state.divider_detected = true;
        s_state.battery_present = true;
        log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                   "Power: voltage divider + battery detected", "HW ADC mode");
      } else {
        /* Divider present but voltage out of LiPo range — USB-only. */
        s_state.monitor_mode = POWER_MODE_USB_ONLY;
        s_state.divider_detected = true;
        s_state.battery_present = false;
        s_state.charge_state = CHARGE_STATE_NO_BATTERY;
        s_state.power_source = POWER_SOURCE_USB_ONLY;
        log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                   "Power: divider present, no battery", "USB-only mode");
      }
    } else {
      /* No divider: the ADC pin floats, so its readings carry no
       * battery information. Report USB-only and stop sampling rather
       * than inferring charge state from noise. A near-rail reading is
       * suspicious -- VBAT wired directly to the pin overstresses it. */
      if (avg_mv > DIVIDER_DETECT_MAX_MV) {
        log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
                   "Power: ADC near rail without divider",
                   "check wiring -- VBAT direct to ADC pin can damage it");
      }
      s_state.monitor_mode = POWER_MODE_USB_ONLY;
      s_state.divider_detected = false;
      s_state.battery_present = false;
      s_state.power_source = POWER_SOURCE_USB_ONLY;
      s_state.charge_state = CHARGE_STATE_NO_BATTERY;
      adc_close();
      s_adc_ready = false;
      log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                 "Power: no divider detected", "USB-only (add divider for battery monitoring)");
    }
  } else {
    s_state.monitor_mode = POWER_MODE_USB_ONLY;
    s_state.divider_detected = false;
    s_state.battery_present = false;
    s_state.power_source = POWER_SOURCE_USB_ONLY;
    s_state.charge_state = CHARGE_STATE_NO_BATTERY;
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Power: ADC read failed", "USB-only mode");
  }

  s_running = true;
  s_last_sample_ms = millis() - s_cfg.sample_interval_ms;
  return true;
}

void power_stop(void) {
  using namespace power;
  if (!s_running) return;
  save_voltage_extremes();
  adc_close();
  s_adc_ready = false;
  s_running = false;
}

bool power_is_running(void) { return power::s_running; }

void power_set_event_callback(power_event_cb_t cb) {
  power::s_cb = cb;
}

bool power_process(void) {
  using namespace power;
  if (!s_running || !s_adc_ready) return false;
  const uint32_t now = millis();
  if ((now - s_last_sample_ms) < s_cfg.sample_interval_ms) return false;
  s_last_sample_ms = now;

  /* Read raw ADC in millivolts. */
  uint16_t adc_mv = adc_read_mv();
  if (adc_mv == 0) return false;

  /* Insert into median filter. */
  s_median_buf[s_median_idx] = adc_mv;
  s_median_idx = (s_median_idx + 1) % MEDIAN_WINDOW;
  if (s_median_count < MEDIAN_WINDOW) s_median_count++;
  uint16_t filtered_mv = compute_median(s_median_buf, s_median_count);

  /* Compute battery voltage from filtered ADC reading. */
  uint16_t battery_mv;
  if (s_state.divider_detected) {
    battery_mv = (uint16_t)(filtered_mv * s_cfg.divider_ratio);
  } else {
    battery_mv = filtered_mv;
  }

  s_state.voltage_mv = battery_mv;
  s_state.last_sample_ms = now;
  s_state.samples_taken++;

  /* Check if a battery is actually connected. Catches USB-only setups
   * where the divider reads VBUS bleed-through rather than a LiPo cell,
   * and setups where no battery is soldered to the pads at all. */
  check_battery_presence(battery_mv);

  /* If no battery, skip SoC/charge/cycle tracking — the voltage
   * reading is meaningless and would produce false critical alerts. */
  if (s_state.charge_state == CHARGE_STATE_NO_BATTERY) return true;

  /* Update voltage extremes. */
  if (battery_mv > s_state.max_voltage_mv)
    s_state.max_voltage_mv = battery_mv;
  if (battery_mv < s_state.min_voltage_mv && battery_mv > 2000)
    s_state.min_voltage_mv = battery_mv;

  /* SoC calculation (only meaningful in HW ADC mode). */
  if (s_state.monitor_mode == POWER_MODE_HW_ADC) {
    s_state.soc_pct = power_logic::voltage_to_soc(battery_mv);
  }

  /* Track voltage trend. */
  s_trend_ring[s_trend_idx] = battery_mv;
  s_trend_idx = (s_trend_idx + 1) % TREND_SAMPLES;
  if (s_trend_count < TREND_SAMPLES) s_trend_count++;
  s_state.trend_mv_per_min = compute_trend_mv_per_min();

  /* Update charge state. */
  update_charge_state(battery_mv);

  /* Track charge cycles. */
  if (s_state.monitor_mode == POWER_MODE_HW_ADC) {
    track_charge_cycle(battery_mv);
  }

  /* Periodically persist voltage extremes. */
  save_voltage_extremes();

  /* Track battery runtime — only accumulate when on battery. */
  if (s_state.power_source == POWER_SOURCE_BATTERY) {
    if (s_runtime_last_ms != 0) {
      uint32_t delta = now - s_runtime_last_ms;
      s_runtime_accum_ms += delta;
      /* Roll over full minutes into the history counter. */
      while (s_runtime_accum_ms >= 60000) {
        s_runtime_accum_ms -= 60000;
        s_history.total_runtime_min++;
      }
    }
  }
  s_runtime_last_ms = now;

  /* Track all-time SoC minimum (only meaningful in HW ADC mode). */
  if (s_state.monitor_mode == POWER_MODE_HW_ADC &&
      s_state.soc_pct < s_history.soc_min_pct) {
    s_history.soc_min_pct = s_state.soc_pct;
  }

  /* Keep history in sync with running state. */
  s_history.charge_cycles  = s_state.charge_cycles;
  s_history.voltage_min_mv = s_state.min_voltage_mv;
  s_history.voltage_max_mv = s_state.max_voltage_mv;

  /* Detect full charge for history timestamp. */
  if (s_state.charge_state == CHARGE_STATE_FULL) {
    s_history.last_full_charge_ms = now;
  }

  return true;
}

bool power_get_state(power_state_t* out) {
  if (!out || !power::s_initialized) return false;
  *out = power::s_state;
  return true;
}

void power_set_capacity_mah(uint16_t mah) {
  power::s_state.capacity_mah = mah;
  power::s_cfg.capacity_mah = mah;
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putUShort("batt_cap", mah);
    prefs.end();
  }
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Power: capacity updated", nullptr);
}

void power_graceful_shutdown(void) {
  using namespace power;
  if (!s_state.battery_present) return;

#if !FEATURE_DEEP_SLEEP
  /* Never-sleep contract (FEATURE_DEEP_SLEEP=0): this function returns instead
   * of deep-sleeping, but policy_process() re-invokes it every loop while the
   * battery stays in PMODE_SHUTDOWN. Do the chain-close attestation + persist
   * EXACTLY ONCE — without this latch each loop would re-emit
   * RECORD_POWER_SHUTDOWN and re-flush NVS/SD until brownout, spamming the
   * witness log and accelerating the final drain. The latch clears on battery
   * recovery in update_charge_state(). */
  if (s_shutdown_latched) return;
#endif

  log_health(LOG_LEVEL_ALERT, LOG_CAT_SYSTEM,
             "Power: graceful shutdown — battery critical", nullptr);

  emit_event(POWER_EVENT_SHUTDOWN);

  /* Close the witness chain with a shutdown attestation record. */
  uint8_t payload[64];
  CborWriter cbor(payload, sizeof(payload));
  cbor.write_map(4);
  cbor.write_text("type"); cbor.write_text("power_shutdown");
  cbor.write_text("soc");  cbor.write_uint(s_state.soc_pct);
  cbor.write_text("mv");   cbor.write_uint(s_state.voltage_mv);
  cbor.write_text("cyc");  cbor.write_uint(s_state.charge_cycles);

  WitnessRecord rec;
  witness_create_record(payload, cbor.size(), RECORD_POWER_SHUTDOWN, &rec);

  /* Flush chain state to NVS and SD. */
  witness_persist_chain_state();

  /* Persist final battery stats. */
  save_voltage_extremes();

  /* Deep-sleep entry is gated on FEATURE_DEEP_SLEEP so a build documented as
   * "compiled in but never sleeps" (FEATURE_DEEP_SLEEP=0) does not silently
   * deep-sleep on a critical-battery event. When the flag is undefined (envs
   * that don't pass -D and don't include canary_config.h here), the C
   * preprocessor evaluates it as 0 — the safe never-sleep default. */
#if FEATURE_DEEP_SLEEP
  /* Stop all peripherals. */
  power_stop();

  /* Arm deep-sleep wake sources: timer (30 min) + touch (silent panic). */
  lowpower_arm_wake_timer(30ULL * 60ULL * 1000000ULL);
  lowpower_arm_wake_touch();

  /* Enter deep sleep — does not return. */
  lowpower_enter_deep_sleep();
#else
  /* Never-sleep contract: the chain-close record and battery stats are already
   * persisted above. Do not stop peripherals or deep-sleep — keep witnessing
   * until brownout. Latch so the record/persist runs once per critical episode
   * (see the guard at the top of this function; cleared on recovery in
   * update_charge_state). Deep-sleep battery protection needs FEATURE_DEEP_SLEEP=1. */
  s_shutdown_latched = true;
  log_health(LOG_LEVEL_WARNING, LOG_CAT_SYSTEM,
             "Power: critical battery — deep sleep disabled (FEATURE_DEEP_SLEEP=0)",
             nullptr);
#endif
}

bool power_is_critical(void) {
  return power::s_state.battery_present &&
         power::s_state.charge_state == CHARGE_STATE_CRITICAL;
}

bool power_is_charging(void) {
  return power::s_state.charge_state == CHARGE_STATE_CHARGING;
}

bool power_get_history(power_history_t* out) {
  if (!out || !power::s_initialized) return false;
  *out = power::s_history;
  return true;
}

void power_persist_history(void) {
  using namespace power;
  if (!s_initialized) return;
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putUInt("batt_rt_min",  s_history.total_runtime_min);
    prefs.putUChar("batt_soc_min", s_history.soc_min_pct);
    prefs.putUInt("batt_bo_cnt",  s_history.brownout_count);
    prefs.putUInt("batt_fc_ms",   s_history.last_full_charge_ms);
    /* charge_cycles and voltage extremes are already persisted by
     * track_charge_cycle() and save_voltage_extremes(). */
    prefs.end();
  }
}

uint8_t power_health_pct(void) {
  using namespace power;
  /* Cycle-fade model in power_logic.h: ~20% fade per 500 full cycles,
   * computed on demand from the persisted cycle count so it can never
   * go stale; clamped to 60%. */
  return power_logic::health_pct_from_cycles(s_state.charge_cycles,
                                             s_state.battery_present);
}

uint32_t power_estimate_runtime_min(void) {
  using namespace power;
  return power_logic::estimate_runtime_min(
      s_state.battery_present, s_state.charge_state,
      s_state.trend_mv_per_min, s_state.voltage_mv);
}

const char* power_charge_state_name(uint8_t charge_state) {
  switch (charge_state) {
    case CHARGE_STATE_CHARGING:    return "charging";
    case CHARGE_STATE_FULL:        return "full";
    case CHARGE_STATE_DISCHARGING: return "discharging";
    case CHARGE_STATE_LOW:         return "low";
    case CHARGE_STATE_CRITICAL:    return "critical";
    case CHARGE_STATE_NO_BATTERY:  return "no_battery";
    default:                       return "unknown";
  }
}

}  /* extern "C" */
