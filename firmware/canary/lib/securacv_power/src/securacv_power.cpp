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
#include "securacv_lowpower.h"

namespace power {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

static constexpr uint16_t DIVIDER_DETECT_MIN_MV = 1200;
static constexpr uint16_t DIVIDER_DETECT_MAX_MV = 2300;

/* Valid LiPo voltage range. Readings outside this with a divider
 * present indicate no battery is connected (USB-only power, or
 * VBUS bleeding through the charger IC to the battery pads). */
static constexpr uint16_t LIPO_VALID_MIN_MV = 2800;
static constexpr uint16_t LIPO_VALID_MAX_MV = 4350;

/* If the divider reads above this, the battery pads are likely seeing
 * VBUS (~5V) through the charger rather than an actual cell. */
static constexpr uint16_t VBUS_BLEED_THRESHOLD_MV = 4400;

/* Number of consecutive out-of-range readings before declaring
 * no battery present (avoids transient glitches on hot-plug). */
static constexpr uint8_t  NO_BATTERY_CONFIRM_COUNT = 5;

static constexpr uint8_t  MEDIAN_WINDOW = 16;

/* LiPo discharge curve: voltage (mV) → SoC (%). Piecewise linear
 * interpolation between these points. Values are for a typical
 * single-cell LiPo at ~0.2C discharge rate, 25 °C. */
struct SocPoint { uint16_t mv; uint8_t pct; };
static constexpr SocPoint SOC_TABLE[] = {
  {4200, 100},
  {4150,  95},
  {4110,  90},
  {4080,  85},
  {4020,  80},
  {3980,  70},
  {3920,  60},
  {3870,  50},
  {3830,  40},
  {3790,  30},
  {3750,  20},
  {3700,  15},
  {3630,  10},
  {3500,   5},
  {3300,   2},
  {3000,   0},
};
static constexpr size_t SOC_TABLE_LEN = sizeof(SOC_TABLE) / sizeof(SOC_TABLE[0]);

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
static constexpr uint16_t CYCLE_LOW_MV  = 3800;
static constexpr uint16_t CYCLE_HIGH_MV = 4100;

/* No-battery detection */
static uint8_t s_no_battery_count = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * ADC PERIPHERAL
 * ────────────────────────────────────────────────────────────────────────── */

static adc1_channel_t gpio_to_adc1_channel(uint8_t gpio) {
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

static uint8_t voltage_to_soc(uint16_t mv) {
  if (mv >= SOC_TABLE[0].mv) return 100;
  if (mv <= SOC_TABLE[SOC_TABLE_LEN - 1].mv) return 0;
  for (size_t i = 0; i < SOC_TABLE_LEN - 1; i++) {
    if (mv >= SOC_TABLE[i + 1].mv) {
      uint16_t v_range = SOC_TABLE[i].mv - SOC_TABLE[i + 1].mv;
      uint8_t  p_range = SOC_TABLE[i].pct - SOC_TABLE[i + 1].pct;
      if (v_range == 0) return SOC_TABLE[i].pct;
      uint16_t v_offset = mv - SOC_TABLE[i + 1].mv;
      return SOC_TABLE[i + 1].pct +
             (uint8_t)((uint32_t)v_offset * p_range / v_range);
    }
  }
  return 0;
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
  if (s_state.monitor_mode == POWER_MODE_USB_ONLY) return;
  if (!s_state.divider_detected) return;

  bool out_of_range = (battery_mv < LIPO_VALID_MIN_MV ||
                       battery_mv > VBUS_BLEED_THRESHOLD_MV);
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
    if (s_state.soc_pct <= s_cfg.soc_critical_pct) {
      next = CHARGE_STATE_CRITICAL;
    } else if (s_state.soc_pct <= s_cfg.soc_low_pct) {
      next = CHARGE_STATE_LOW;
    } else if (battery_mv >= 4150 && trend >= -1 && trend <= 1) {
      next = CHARGE_STATE_FULL;
    } else if (trend > 2) {
      next = CHARGE_STATE_CHARGING;
    } else {
      next = CHARGE_STATE_DISCHARGING;
    }
  } else if (s_state.monitor_mode == POWER_MODE_SW_INFERENCE) {
    if (trend > 3) {
      next = CHARGE_STATE_CHARGING;
    } else if (trend < -3) {
      next = CHARGE_STATE_DISCHARGING;
    } else if (prev == CHARGE_STATE_CHARGING) {
      next = CHARGE_STATE_FULL;
    }
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
    prefs.end();
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
             "Power HAL initialized", "dual-mode ADC + inference");
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
      /* No divider — fall back to software inference. We can't tell
       * if a battery is present without hardware. Assume USB power. */
      s_state.monitor_mode = POWER_MODE_SW_INFERENCE;
      s_state.divider_detected = false;
      s_state.battery_present = false;
      s_state.power_source = POWER_SOURCE_USB_ONLY;
      s_state.charge_state = CHARGE_STATE_NO_BATTERY;
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
    s_state.soc_pct = voltage_to_soc(battery_mv);
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

  /* Stop all peripherals. */
  power_stop();

  /* Arm deep-sleep wake sources: timer (30 min) + touch (silent panic). */
  lowpower_arm_wake_timer(30ULL * 60ULL * 1000000ULL);
  lowpower_arm_wake_touch();

  /* Enter deep sleep — does not return. */
  lowpower_enter_deep_sleep();
}

bool power_is_critical(void) {
  return power::s_state.battery_present &&
         power::s_state.charge_state == CHARGE_STATE_CRITICAL;
}

bool power_is_charging(void) {
  return power::s_state.charge_state == CHARGE_STATE_CHARGING;
}

}  /* extern "C" */
