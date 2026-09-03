/*
 * SecuraCV Canary WAP — Power Monitor (header-only)
 *
 * Battery voltage monitoring, charge state detection, SoC estimation,
 * and graceful brownout shutdown for the XIAO ESP32-S3 Sense with a
 * LiPo battery connected to the built-in TP4056 charging circuit.
 *
 * Operating modes:
 *
 *   HARDWARE ADC — a 2:1 voltage divider (two 100K resistors) from
 *     VBAT to GPIO 1 (D0/A0) provides calibrated millivolt readings.
 *     The ESP32-S3's eFuse ADC calibration data produces +/-20 mV
 *     accuracy, yielding +/-2% SoC for a typical LiPo discharge curve.
 *
 *   USB ONLY — when no divider is wired, GPIO 1 floats and its readings
 *     carry no battery information, so the module reports USB-only and
 *     stops sampling rather than inferring charge state from noise.
 *     (POWER_MODE_SW_INFERENCE is retained in the enum for wire-format
 *     stability but is no longer entered.)
 *
 * Auto-detection at boot: the module reads the ADC and classifies the
 * result. A reading in the 1.2-2.3 V range (corresponds to 2.4-4.6 V
 * with a 2:1 divider) indicates a divider is present. Near-zero or
 * near-rail readings indicate no divider. A near-rail reading also
 * triggers a wiring warning: VBAT tied directly to GPIO 1 without a
 * divider overstresses the pin.
 *
 * Ported from PIO securacv_power to header-only WAP form.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_POWER_MONITOR_H
#define SECURACV_POWER_MONITOR_H

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <esp_adc_cal.h>
#include <esp_idf_version.h>
#include <esp_sleep.h>
#include <esp_system.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #include <esp_adc/adc_oneshot.h>
  #include <esp_adc/adc_cali.h>
  #include <esp_adc/adc_cali_scheme.h>
#else
  #include <driver/adc.h>
#endif

#include "build_config.h"
#include "log_level.h"
// Pure battery decision logic (SoC curve, charge-state classification,
// health/runtime estimates). Sketch-local copy of the canonical
// firmware/common/power/power_logic.h; CI's check_power_sync.sh guards
// against drift. Host-tested in firmware/common/power/test_power_logic.cpp.
#include "power_logic.h"

// ════════════════════════════════════════════════════════════════════════════
// ENUMS
// ════════════════════════════════════════════════════════════════════════════

enum PowerMonitorMode : uint8_t {
  POWER_MODE_HW_ADC        = 0,
  POWER_MODE_SW_INFERENCE  = 1,
  POWER_MODE_USB_ONLY      = 2,
};

enum PowerSource : uint8_t {
  POWER_SOURCE_UNKNOWN     = 0,
  POWER_SOURCE_USB         = 1,
  POWER_SOURCE_BATTERY     = 2,
  POWER_SOURCE_USB_ONLY    = 3,
};

enum ChargeState : uint8_t {
  CHARGE_STATE_UNKNOWN     = 0,
  CHARGE_STATE_CHARGING    = 1,
  CHARGE_STATE_FULL        = 2,
  CHARGE_STATE_DISCHARGING = 3,
  CHARGE_STATE_LOW         = 4,
  CHARGE_STATE_CRITICAL    = 5,
  CHARGE_STATE_NO_BATTERY  = 6,
};

// ════════════════════════════════════════════════════════════════════════════
// STATE SNAPSHOT
// ════════════════════════════════════════════════════════════════════════════

struct PowerState {
  uint8_t   monitor_mode;
  uint8_t   power_source;
  uint8_t   charge_state;
  uint8_t   soc_pct;
  uint16_t  voltage_mv;
  uint16_t  capacity_mah;
  int16_t   trend_mv_per_min;
  uint32_t  last_sample_ms;
  uint32_t  charge_cycles;
  uint16_t  max_voltage_mv;
  uint16_t  min_voltage_mv;
  uint32_t  samples_taken;
  bool      divider_detected;
  bool      battery_present;
};

// ════════════════════════════════════════════════════════════════════════════
// BATTERY HEALTH HISTORY (NVS-persisted)
// ════════════════════════════════════════════════════════════════════════════

struct PowerHistory {
  uint32_t charge_cycles;
  uint32_t total_runtime_min;
  uint16_t voltage_min_mv;
  uint16_t voltage_max_mv;
  uint8_t  soc_min_pct;
  uint32_t brownout_count;
  uint32_t last_full_charge_ms;
};

// ════════════════════════════════════════════════════════════════════════════
// NAMESPACE — header-only implementation
// ════════════════════════════════════════════════════════════════════════════

namespace power_monitor {

// ────────────────────────────────────────────────────────────────────────────
// CONSTANTS
// ────────────────────────────────────────────────────────────────────────────

#if defined(HARDWARE_XIAO_ESP32C3)
static constexpr uint8_t  ADC_GPIO            = 2;      // GPIO 2 (A0/D0 on XIAO ESP32-C3)
#else
static constexpr uint8_t  ADC_GPIO            = 1;      // GPIO 1 (A0/D0 on XIAO ESP32-S3)
#endif
static constexpr float    DIVIDER_RATIO       = 2.0f;
static constexpr uint16_t CAPACITY_MAH        = 3000;
static constexpr uint32_t SAMPLE_INTERVAL_MS  = 1000;
static constexpr uint8_t  SOC_LOW_PCT         = 15;
static constexpr uint8_t  SOC_CRITICAL_PCT    = 5;
static constexpr uint32_t TREND_WINDOW_MS     = 60000;

static constexpr uint16_t DIVIDER_DETECT_MIN_MV = 1200;
static constexpr uint16_t DIVIDER_DETECT_MAX_MV = 2300;
static constexpr uint16_t LIPO_VALID_MIN_MV     = power_logic::LIPO_VALID_MIN_MV;
static constexpr uint16_t LIPO_VALID_MAX_MV     = 4350;
static constexpr uint8_t  NO_BATTERY_CONFIRM_COUNT = 5;
static constexpr uint8_t  MEDIAN_WINDOW         = 16;

// LiPo discharge curve lives in power_logic.h (power_logic::SOC_TABLE).

static constexpr uint8_t  TREND_SAMPLES = 60;
static constexpr uint16_t CYCLE_LOW_MV  = 3800;
static constexpr uint16_t CYCLE_HIGH_MV = 4100;

// NVS persist interval for voltage extremes (5 min)
static constexpr uint32_t EXTREMES_PERSIST_MS = 300000;

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool         s_initialized = false;
static bool         s_running     = false;
static PowerState   s_state       = {};
static PowerHistory s_history     = {};

// Log callback (set via init, matches sys_monitor pattern)
static void (*s_log_cb)(LogLevel, LogCategory, const char*, const char*) = nullptr;

// ADC
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static adc_oneshot_unit_handle_t s_adc_handle = nullptr;
static adc_cali_handle_t         s_cali_handle = nullptr;
#else
static esp_adc_cal_characteristics_t s_adc_chars;
static bool s_adc_cal_valid = false;
#endif
static bool s_adc_ready = false;

// Sampling
static uint32_t s_last_sample_ms   = 0;
static uint16_t s_median_buf[MEDIAN_WINDOW];
static uint8_t  s_median_idx       = 0;
static uint8_t  s_median_count     = 0;

// Trend tracking
static uint16_t s_trend_ring[TREND_SAMPLES];
static uint8_t  s_trend_idx   = 0;
static uint8_t  s_trend_count = 0;

// Charge cycle detection
static bool s_was_below_cycle_threshold = false;

// No-battery detection
static uint8_t s_no_battery_count = 0;

// Runtime tracking
static uint32_t s_runtime_accum_ms = 0;
static uint32_t s_runtime_last_ms  = 0;

// Voltage extremes persist timer
static uint32_t s_last_extremes_persist_ms = 0;

// Charge state hysteresis
static ChargeState s_pending_charge = CHARGE_STATE_UNKNOWN;
static uint8_t     s_pending_charge_count = 0;

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Logging helper
// ────────────────────────────────────────────────────────────────────────────

inline void log_msg(LogLevel lvl, const char* msg, const char* detail = nullptr) {
  if (s_log_cb) s_log_cb(lvl, SCV_CAT_SENSOR, msg, detail);
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: ADC GPIO -> channel mapping
// ────────────────────────────────────────────────────────────────────────────

inline adc1_channel_t gpio_to_adc1_channel(uint8_t gpio) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3: ADC1 spans GPIO 0-4 → channel 0-4 and the legacy enum STOPS
  // at CHANNEL_4, so the S3 arm below would not even compile on this chip
  // — the gate is the chip target, not the board name.
  switch (gpio) {
    case 0:  return ADC1_CHANNEL_0;
    case 1:  return ADC1_CHANNEL_1;
    case 2:  return ADC1_CHANNEL_2;
    case 3:  return ADC1_CHANNEL_3;
    case 4:  return ADC1_CHANNEL_4;
    default: return ADC1_CHANNEL_0;
  }
#else
  // ESP32-S3: ADC1 spans GPIO 1-10 → channel 0-9.
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

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: ADC open / close / read
// ────────────────────────────────────────────────────────────────────────────

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

inline bool adc_open() {
  adc_oneshot_unit_init_cfg_t unit_cfg = {
    .unit_id = ADC_UNIT_1,
  };
  esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
  if (err != ESP_OK) {
    log_msg(SCV_LOG_WARNING, "Power: ADC unit init failed");
    return false;
  }
  adc_oneshot_chan_cfg_t chan_cfg = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  adc_channel_t ch = (adc_channel_t)gpio_to_adc1_channel(ADC_GPIO);
  err = adc_oneshot_config_channel(s_adc_handle, ch, &chan_cfg);
  if (err != ESP_OK) {
    log_msg(SCV_LOG_WARNING, "Power: ADC channel config failed");
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

inline void adc_close() {
  if (s_cali_handle) {
    adc_cali_delete_scheme_curve_fitting(s_cali_handle);
    s_cali_handle = nullptr;
  }
  if (s_adc_handle) {
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = nullptr;
  }
}

inline uint16_t adc_read_mv() {
  if (!s_adc_handle) return 0;
  int raw = 0;
  adc_channel_t ch = (adc_channel_t)gpio_to_adc1_channel(ADC_GPIO);
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

inline bool adc_open() {
  adc1_channel_t ch = gpio_to_adc1_channel(ADC_GPIO);
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ch, ADC_ATTEN_DB_12);
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
      ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &s_adc_chars);
  s_adc_cal_valid = (val_type != ESP_ADC_CAL_VAL_NOT_SUPPORTED);
  return true;
}

inline void adc_close() {
  s_adc_cal_valid = false;
}

inline uint16_t adc_read_mv() {
  adc1_channel_t ch = gpio_to_adc1_channel(ADC_GPIO);
  int raw = adc1_get_raw(ch);
  if (raw < 0) return 0;
  if (s_adc_cal_valid) {
    return (uint16_t)esp_adc_cal_raw_to_voltage((uint32_t)raw, &s_adc_chars);
  }
  return (uint16_t)((raw * 3300) / 4095);
}

#endif /* ESP_IDF_VERSION */

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Helpers
// ────────────────────────────────────────────────────────────────────────────

inline uint16_t compute_median(const uint16_t* buf, uint8_t count) {
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

inline int16_t compute_trend_mv_per_min() {
  if (s_trend_count < 10) return 0;
  uint8_t oldest_idx = (s_trend_idx + TREND_SAMPLES - s_trend_count) % TREND_SAMPLES;
  int32_t oldest = s_trend_ring[oldest_idx];
  int32_t newest = s_trend_ring[(s_trend_idx + TREND_SAMPLES - 1) % TREND_SAMPLES];
  uint32_t span_ms = (uint32_t)s_trend_count * SAMPLE_INTERVAL_MS;
  if (span_ms == 0) return 0;
  return (int16_t)(((newest - oldest) * 60000) / (int32_t)span_ms);
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: NVS persistence
// ────────────────────────────────────────────────────────────────────────────

inline void load_nvs_state() {
  Preferences prefs;
  if (prefs.begin("securacv", true)) {
    s_state.charge_cycles  = prefs.getUInt("batt_cycles", 0);
    s_state.capacity_mah   = prefs.getUShort("batt_cap", CAPACITY_MAH);
    s_state.max_voltage_mv = prefs.getUShort("batt_max_mv", 0);
    s_state.min_voltage_mv = prefs.getUShort("batt_min_mv", 0xFFFF);

    s_history.charge_cycles     = s_state.charge_cycles;
    s_history.total_runtime_min = prefs.getUInt("batt_rt_min", 0);
    s_history.voltage_min_mv    = s_state.min_voltage_mv;
    s_history.voltage_max_mv    = s_state.max_voltage_mv;
    s_history.soc_min_pct       = prefs.getUChar("batt_soc_min", 100);
    s_history.brownout_count    = prefs.getUInt("batt_bo_cnt", 0);
    s_history.last_full_charge_ms = prefs.getUInt("batt_fc_ms", 0);
    prefs.end();
  }

  // Detect brownout reset and increment count. Persist immediately:
  // a device in a brownout loop may never reach the periodic history
  // save, and the count is the main field-diagnosis signal.
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

inline void save_voltage_extremes() {
  uint32_t now = millis();
  if (now - s_last_extremes_persist_ms < EXTREMES_PERSIST_MS) return;
  s_last_extremes_persist_ms = now;
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putUShort("batt_max_mv", s_state.max_voltage_mv);
    prefs.putUShort("batt_min_mv", s_state.min_voltage_mv);
    prefs.end();
  }
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Battery presence check
// ────────────────────────────────────────────────────────────────────────────

inline void check_battery_presence(uint16_t battery_mv) {
  // Keep running as long as a divider is wired -- monitor_mode may have
  // flipped to USB_ONLY after a battery removal, and this is the only
  // path that can detect the battery being reattached.
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
      log_msg(SCV_LOG_INFO, "Power: no battery detected", "USB-only mode");
    }
  } else {
    s_no_battery_count = 0;
    if (!s_state.battery_present) {
      s_state.battery_present = true;
      s_state.monitor_mode = POWER_MODE_HW_ADC;
      // Clear the latched NO_BATTERY state so the charge state machine
      // re-classifies from fresh trend data instead of staying stuck.
      s_state.charge_state = CHARGE_STATE_UNKNOWN;
      s_state.power_source = POWER_SOURCE_UNKNOWN;
      log_msg(SCV_LOG_INFO, "Power: battery detected", "HW ADC mode");
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Charge state machine
// ────────────────────────────────────────────────────────────────────────────

inline void update_charge_state(uint16_t battery_mv) {
  if (s_state.charge_state == CHARGE_STATE_NO_BATTERY) return;

  ChargeState prev = (ChargeState)s_state.charge_state;
  ChargeState next = prev;
  int16_t trend = s_state.trend_mv_per_min;

  if (s_state.monitor_mode == POWER_MODE_HW_ADC) {
    // Charging detection runs BEFORE the low/critical SoC checks so a
    // depleted battery on USB reports CHARGING (power source USB) --
    // ordering pinned by firmware/common/power/test_power_logic.cpp.
    next = (ChargeState)power_logic::classify_hw_charge_state(
        battery_mv, s_state.soc_pct, trend, SOC_LOW_PCT, SOC_CRITICAL_PCT);
  }

  // Hysteresis: require consecutive readings in the same state
  if (next != prev) {
    if (next == s_pending_charge) {
      s_pending_charge_count++;
    } else {
      s_pending_charge = next;
      s_pending_charge_count = 1;
    }
    uint8_t threshold = (next == CHARGE_STATE_LOW ||
                         next == CHARGE_STATE_CRITICAL) ? 3 : 2;
    if (s_pending_charge_count < threshold) return;
  }
  s_pending_charge = CHARGE_STATE_UNKNOWN;
  s_pending_charge_count = 0;

  if (next == prev) return;
  s_state.charge_state = next;

  // Derive power source from charge state
  if (next == CHARGE_STATE_CHARGING || next == CHARGE_STATE_FULL) {
    s_state.power_source = POWER_SOURCE_USB;
  } else {
    s_state.power_source = POWER_SOURCE_BATTERY;
  }

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
  log_msg(SCV_LOG_INFO, "Power: charge state changed", name);
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Charge cycle tracking
// ────────────────────────────────────────────────────────────────────────────

inline void track_charge_cycle(uint16_t battery_mv) {
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

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

/**
 * Initialize the power monitor.
 * @param log_callback  Logging callback matching sys_monitor pattern
 */
inline void init(void (*log_callback)(LogLevel, LogCategory, const char*, const char*) = nullptr) {
  if (s_initialized) return;
  s_log_cb = log_callback;

  memset(&s_state, 0, sizeof(s_state));
  s_state.min_voltage_mv = 0xFFFF;
  s_state.capacity_mah   = CAPACITY_MAH;

  s_median_idx   = 0;
  s_median_count = 0;
  s_trend_idx    = 0;
  s_trend_count  = 0;
  s_was_below_cycle_threshold = false;
  s_no_battery_count = 0;
  s_pending_charge = CHARGE_STATE_UNKNOWN;
  s_pending_charge_count = 0;

  load_nvs_state();

  // Open ADC
  if (!adc_open()) {
    log_msg(SCV_LOG_WARNING, "Power: ADC open failed");
    s_state.monitor_mode  = POWER_MODE_USB_ONLY;
    s_state.battery_present = false;
    s_state.power_source  = POWER_SOURCE_USB_ONLY;
    s_state.charge_state  = CHARGE_STATE_NO_BATTERY;
    s_initialized = true;
    s_running = true;
    return;
  }
  s_adc_ready = true;

  // Auto-detect divider presence
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
      uint16_t batt_mv = (uint16_t)(avg_mv * DIVIDER_RATIO);
      if (batt_mv >= LIPO_VALID_MIN_MV && batt_mv <= LIPO_VALID_MAX_MV) {
        s_state.monitor_mode     = POWER_MODE_HW_ADC;
        s_state.divider_detected = true;
        s_state.battery_present  = true;
        log_msg(SCV_LOG_INFO, "Power: divider + battery detected", "HW ADC mode");
      } else {
        s_state.monitor_mode     = POWER_MODE_USB_ONLY;
        s_state.divider_detected = true;
        s_state.battery_present  = false;
        s_state.charge_state     = CHARGE_STATE_NO_BATTERY;
        s_state.power_source     = POWER_SOURCE_USB_ONLY;
        log_msg(SCV_LOG_INFO, "Power: divider present, no battery", "USB-only mode");
      }
    } else {
      // No divider: GPIO 1 floats, so its readings carry no battery
      // information. Report USB-only and stop sampling rather than
      // inferring charge state from noise.
      if (avg_mv > DIVIDER_DETECT_MAX_MV) {
        log_msg(SCV_LOG_WARNING, "Power: ADC near rail without divider",
                "check wiring -- VBAT direct to GPIO 1 can damage the pin");
      }
      s_state.monitor_mode     = POWER_MODE_USB_ONLY;
      s_state.divider_detected = false;
      s_state.battery_present  = false;
      s_state.power_source     = POWER_SOURCE_USB_ONLY;
      s_state.charge_state     = CHARGE_STATE_NO_BATTERY;
      adc_close();
      s_adc_ready = false;
      log_msg(SCV_LOG_INFO, "Power: no divider detected", "USB-only (add divider for battery monitoring)");
    }
  } else {
    s_state.monitor_mode     = POWER_MODE_USB_ONLY;
    s_state.divider_detected = false;
    s_state.battery_present  = false;
    s_state.power_source     = POWER_SOURCE_USB_ONLY;
    s_state.charge_state     = CHARGE_STATE_NO_BATTERY;
    log_msg(SCV_LOG_WARNING, "Power: ADC read failed", "USB-only mode");
  }

  s_initialized = true;
  s_running = true;
  s_last_sample_ms = millis() - SAMPLE_INTERVAL_MS;
  log_msg(SCV_LOG_INFO, "Power monitor initialized", "HW ADC with USB-only fallback");
}

/**
 * Process one cycle. Call from loop().
 * Reads ADC at the configured interval, updates SoC/charge state.
 * @return true if a sample was taken this cycle
 */
inline bool process() {
  if (!s_running || !s_adc_ready) return false;
  const uint32_t now = millis();
  if ((now - s_last_sample_ms) < SAMPLE_INTERVAL_MS) return false;
  s_last_sample_ms = now;

  uint16_t adc_mv = adc_read_mv();
  if (adc_mv == 0) return false;

  // Median filter
  s_median_buf[s_median_idx] = adc_mv;
  s_median_idx = (s_median_idx + 1) % MEDIAN_WINDOW;
  if (s_median_count < MEDIAN_WINDOW) s_median_count++;
  uint16_t filtered_mv = compute_median(s_median_buf, s_median_count);

  // Compute battery voltage
  uint16_t battery_mv;
  if (s_state.divider_detected) {
    battery_mv = (uint16_t)(filtered_mv * DIVIDER_RATIO);
  } else {
    battery_mv = filtered_mv;
  }

  s_state.voltage_mv     = battery_mv;
  s_state.last_sample_ms = now;
  s_state.samples_taken++;

  // Check battery presence
  check_battery_presence(battery_mv);

  // Skip SoC/charge tracking when no battery
  if (s_state.charge_state == CHARGE_STATE_NO_BATTERY) return true;

  // Update voltage extremes
  if (battery_mv > s_state.max_voltage_mv)
    s_state.max_voltage_mv = battery_mv;
  if (battery_mv < s_state.min_voltage_mv && battery_mv > 2000)
    s_state.min_voltage_mv = battery_mv;

  // SoC calculation (HW ADC only)
  if (s_state.monitor_mode == POWER_MODE_HW_ADC) {
    s_state.soc_pct = power_logic::voltage_to_soc(battery_mv);
  }

  // Track voltage trend
  s_trend_ring[s_trend_idx] = battery_mv;
  s_trend_idx = (s_trend_idx + 1) % TREND_SAMPLES;
  if (s_trend_count < TREND_SAMPLES) s_trend_count++;
  s_state.trend_mv_per_min = compute_trend_mv_per_min();

  // Update charge state
  update_charge_state(battery_mv);

  // Track charge cycles (HW ADC only)
  if (s_state.monitor_mode == POWER_MODE_HW_ADC) {
    track_charge_cycle(battery_mv);
  }

  // Periodically persist voltage extremes
  save_voltage_extremes();

  // Track battery runtime
  if (s_state.power_source == POWER_SOURCE_BATTERY) {
    if (s_runtime_last_ms != 0) {
      uint32_t delta = now - s_runtime_last_ms;
      s_runtime_accum_ms += delta;
      while (s_runtime_accum_ms >= 60000) {
        s_runtime_accum_ms -= 60000;
        s_history.total_runtime_min++;
      }
    }
  }
  s_runtime_last_ms = now;

  // Track all-time SoC minimum (HW ADC only)
  if (s_state.monitor_mode == POWER_MODE_HW_ADC &&
      s_state.soc_pct < s_history.soc_min_pct) {
    s_history.soc_min_pct = s_state.soc_pct;
  }

  // Keep history in sync with running state
  s_history.charge_cycles  = s_state.charge_cycles;
  s_history.voltage_min_mv = s_state.min_voltage_mv;
  s_history.voltage_max_mv = s_state.max_voltage_mv;

  // Detect full charge for history timestamp
  if (s_state.charge_state == CHARGE_STATE_FULL) {
    s_history.last_full_charge_ms = now;
  }

  return true;
}

/**
 * Get the current power state snapshot.
 */
inline bool get_state(PowerState* out) {
  if (!out || !s_initialized) return false;
  *out = s_state;
  return true;
}

/**
 * Check if battery is below the critical threshold.
 */
inline bool is_critical() {
  return s_state.battery_present &&
         s_state.charge_state == CHARGE_STATE_CRITICAL;
}

/**
 * Check if the battery is currently charging.
 */
inline bool is_charging() {
  return s_state.charge_state == CHARGE_STATE_CHARGING;
}

/**
 * Get the battery health history (NVS-persisted stats).
 */
inline bool get_history(PowerHistory* out) {
  if (!out || !s_initialized) return false;
  *out = s_history;
  return true;
}

/**
 * Persist battery health history to NVS. Call periodically (e.g. every 10 min).
 */
inline void persist_history() {
  if (!s_initialized) return;
  Preferences prefs;
  if (prefs.begin("securacv", false)) {
    prefs.putUInt("batt_rt_min",  s_history.total_runtime_min);
    prefs.putUChar("batt_soc_min", s_history.soc_min_pct);
    prefs.putUInt("batt_bo_cnt",  s_history.brownout_count);
    prefs.putUInt("batt_fc_ms",   s_history.last_full_charge_ms);
    prefs.end();
  }
}

/**
 * Battery health estimate: percent of rated capacity remaining.
 *
 * Cycle-fade model: a consumer LiPo loses roughly 20% of its capacity
 * over ~500 full charge cycles (datasheet-typical). Computed on demand
 * from the persisted cycle count, so it can never go stale or disagree
 * with stored history. Clamped to 60% -- below that the linear model is
 * no longer meaningful and the cell should simply be replaced.
 * Returns 100 when no battery is present (nothing to degrade).
 */
inline uint8_t health_pct() {
  return power_logic::health_pct_from_cycles(s_state.charge_cycles,
                                             s_state.battery_present);
}

/**
 * Estimated minutes of battery runtime remaining, derived from the
 * measured discharge slope: time for the present voltage to reach the
 * 3.3 V low-battery cutoff at the current mV/min trend. This is a
 * coarse estimate (the LiPo curve is not linear in voltage) meant for
 * "hours vs. days" planning, not precision.
 *
 * Returns 0 when unknown rather than guessing: no battery, charging,
 * trend not yet established (needs ~10 samples), or voltage already at
 * the cutoff. Capped at 14 days -- slower slopes are below measurement
 * resolution.
 */
inline uint32_t estimate_runtime_min() {
  return power_logic::estimate_runtime_min(
      s_state.battery_present, s_state.charge_state,
      s_state.trend_mv_per_min, s_state.voltage_mv);
}

/**
 * Initiate graceful shutdown: persist stats, then enter deep sleep.
 * The caller is responsible for flushing witness chain state before
 * calling this, if desired.
 */
inline void graceful_shutdown() {
  if (!s_state.battery_present) return;
  log_msg(SCV_LOG_ALERT, "Power: graceful shutdown -- battery critical");

  // Persist final battery stats
  save_voltage_extremes();
  persist_history();

  // Stop ADC
  if (s_adc_ready) {
    adc_close();
    s_adc_ready = false;
  }
  s_running = false;

  // Enter deep sleep with 30-min timer wake
  esp_sleep_enable_timer_wakeup(30ULL * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
  // Does not return
}

/**
 * Human-readable charge state name.
 */
inline const char* charge_state_name(uint8_t cs) {
  switch (cs) {
    case CHARGE_STATE_UNKNOWN:     return "unknown";
    case CHARGE_STATE_CHARGING:    return "charging";
    case CHARGE_STATE_FULL:        return "full";
    case CHARGE_STATE_DISCHARGING: return "discharging";
    case CHARGE_STATE_LOW:         return "low";
    case CHARGE_STATE_CRITICAL:    return "critical";
    case CHARGE_STATE_NO_BATTERY:  return "no_battery";
    default:                       return "unknown";
  }
}

/**
 * Human-readable monitor mode name.
 */
inline const char* mode_name(uint8_t mode) {
  switch (mode) {
    case POWER_MODE_HW_ADC:       return "hw_adc";
    case POWER_MODE_SW_INFERENCE: return "sw_inference";
    case POWER_MODE_USB_ONLY:     return "usb_only";
    default:                      return "unknown";
  }
}

/**
 * Print battery status to Serial.
 */
inline void print_status() {
  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│         BATTERY STATUS               │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf("│ Mode       : %-22s │\n", mode_name(s_state.monitor_mode));
  Serial.printf("│ Battery    : %-22s │\n", s_state.battery_present ? "YES" : "NO");
  Serial.printf("│ State      : %-22s │\n", charge_state_name(s_state.charge_state));
  Serial.printf("│ Voltage    : %u mV                   │\n", s_state.voltage_mv);
  Serial.printf("│ SoC        : %u%%                    │\n", s_state.soc_pct);
  Serial.printf("│ Trend      : %+d mV/min              │\n", s_state.trend_mv_per_min);
  Serial.printf("│ Cycles     : %u                      │\n", s_state.charge_cycles);
  Serial.printf("│ Samples    : %u                      │\n", s_state.samples_taken);
  Serial.printf("│ Vmin/Vmax  : %u / %u mV             │\n",
                s_state.min_voltage_mv == 0xFFFF ? 0 : s_state.min_voltage_mv,
                s_state.max_voltage_mv);
  Serial.printf("│ Brownouts  : %u                      │\n", s_history.brownout_count);
  Serial.printf("│ Runtime    : %u min                  │\n", s_history.total_runtime_min);
  Serial.printf("│ Health     : %u%%                    │\n", health_pct());
  Serial.printf("│ Est. left  : %u min                  │\n", estimate_runtime_min());
  Serial.println("└─────────────────────────────────────┘");
}

/**
 * Get battery status as JSON.
 */
inline size_t get_json(char* buf, size_t buf_size) {
  int len = snprintf(buf, buf_size,
    "{"
    "\"monitor_mode\":\"%s\","
    "\"battery_present\":%s,"
    "\"charge_state\":\"%s\","
    "\"voltage_mv\":%u,"
    "\"soc_pct\":%u,"
    "\"trend_mv_per_min\":%d,"
    "\"capacity_mah\":%u,"
    "\"charge_cycles\":%u,"
    "\"voltage_min_mv\":%u,"
    "\"voltage_max_mv\":%u,"
    "\"brownout_count\":%u,"
    "\"total_runtime_min\":%u,"
    "\"soc_min_pct\":%u,"
    "\"health_pct\":%u,"
    "\"est_runtime_min\":%u,"
    "\"samples_taken\":%u"
    "}",
    mode_name(s_state.monitor_mode),
    s_state.battery_present ? "true" : "false",
    charge_state_name(s_state.charge_state),
    s_state.voltage_mv,
    s_state.soc_pct,
    (int)s_state.trend_mv_per_min,
    s_state.capacity_mah,
    s_state.charge_cycles,
    s_state.min_voltage_mv == 0xFFFF ? 0 : s_state.min_voltage_mv,
    s_state.max_voltage_mv,
    s_history.brownout_count,
    s_history.total_runtime_min,
    s_history.soc_min_pct,
    health_pct(),
    estimate_runtime_min(),
    s_state.samples_taken
  );
  return (len > 0 && len < (int)buf_size) ? (size_t)len : 0;
}

} // namespace power_monitor

#endif // SECURACV_POWER_MONITOR_H
