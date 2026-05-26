/*
 * SecuraCV Canary — Power Policy Engine implementation
 *
 * Pure decision layer: reads securacv_power state, emits feature-enable
 * signals and sleep requests. Does NOT call into sensor libraries.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_power_policy.h"

#include <Arduino.h>
#include <string.h>

#include "canary_config.h"
#include "log_level.h"
#include "securacv_witness.h"
#include "securacv_power.h"

#include <esp_pm.h>
#include <esp_wifi.h>

namespace policy {

static bool s_initialized = false;
static policy_config_t s_cfg = POLICY_CONFIG_DEFAULT;
static policy_mode_change_cb_t s_cb = nullptr;
static power_mode_t s_mode = PMODE_PLUGGED_IN;
static power_mode_t s_prev_mode = PMODE_PLUGGED_IN;
static uint32_t s_mode_entered_ms = 0;
static uint32_t s_transitions = 0;
static bool s_deep_sleep_pending = false;
static uint32_t s_last_deep_sleep_ms = 0;

static power_mode_t s_pending_mode = PMODE_PLUGGED_IN;
static uint32_t s_pending_since = 0;

static policy_features_t s_features = {};

/* ──────────────────────────────────────────────────────────────────────────
 * FEATURE PROFILES
 *
 * Each mode defines which features run and at what rate. The profiles
 * are applied atomically on mode transition.
 * ────────────────────────────────────────────────────────────────────────── */

static void apply_plugged_in() {
  s_features.wifi_ap       = true;
  s_features.wifi_sta      = true;
  s_features.http_server   = true;
  s_features.camera_peek   = true;
  s_features.csi           = true;
  s_features.acoustic      = true;
  s_features.touch         = true;
  s_features.ir_rmt        = true;
  s_features.temp_tamper   = true;
  s_features.vision        = true;
  s_features.gnss          = true;
  s_features.mqtt          = true;
  s_features.mesh          = true;
  s_features.record_interval_ms = RECORD_INTERVAL_MS;
  s_features.cpu_freq_mhz  = 240;
  s_features.wifi_ps_mode   = 0;  /* WIFI_PS_NONE */
}

static void apply_battery_normal() {
  s_features.wifi_ap       = true;
  s_features.wifi_sta      = true;
  s_features.http_server   = true;
  s_features.camera_peek   = false;
  s_features.csi           = true;
  s_features.acoustic      = true;
  s_features.touch         = true;
  s_features.ir_rmt        = true;
  s_features.temp_tamper   = true;
  s_features.vision        = false;
  s_features.gnss          = true;
  s_features.mqtt          = true;
  s_features.mesh          = true;
  s_features.record_interval_ms = 5000;
  s_features.cpu_freq_mhz  = 160;
  s_features.wifi_ps_mode   = 1;  /* WIFI_PS_MIN_MODEM */
}

static void apply_battery_saver() {
  s_features.wifi_ap       = true;
  s_features.wifi_sta      = true;
  s_features.http_server   = true;
  s_features.camera_peek   = false;
  s_features.csi           = false;
  s_features.acoustic      = true;
  s_features.touch         = true;
  s_features.ir_rmt        = false;
  s_features.temp_tamper   = true;
  s_features.vision        = false;
  s_features.gnss          = false;
  s_features.mqtt          = true;
  s_features.mesh          = false;
  s_features.record_interval_ms = 30000;
  s_features.cpu_freq_mhz  = 80;
  s_features.wifi_ps_mode   = 2;  /* WIFI_PS_MAX_MODEM */
}

static void apply_low_power() {
  s_features.wifi_ap       = false;
  /* WiFi STA stays on so panic events can still reach HA via MQTT.
   * Without this, a silent-panic touch event gets witnessed locally
   * but never leaves the device — defeating the safety purpose. */
  s_features.wifi_sta      = true;
  s_features.http_server   = false;
  s_features.camera_peek   = false;
  s_features.csi           = false;
  /* Acoustic stays on: T3 smoke / T4 CO are life-safety events that
   * must not be suppressed by battery state. The PDM mic draws ~1 mA
   * — acceptable even in low-power mode. */
  s_features.acoustic      = true;
  s_features.touch         = true;
  s_features.ir_rmt        = false;
  s_features.temp_tamper   = true;
  s_features.vision        = false;
  s_features.gnss          = false;
  s_features.mqtt          = true;
  s_features.mesh          = false;
  s_features.record_interval_ms = 60000;
  s_features.cpu_freq_mhz  = 80;
  s_features.wifi_ps_mode   = 2;  /* WIFI_PS_MAX_MODEM — aggressive sleep */
}

static void apply_usb_only() {
  apply_plugged_in();
}

static void apply_mode(power_mode_t mode) {
  switch (mode) {
    case PMODE_PLUGGED_IN:     apply_plugged_in(); break;
    case PMODE_BATTERY_NORMAL: apply_battery_normal(); break;
    case PMODE_BATTERY_SAVER:  apply_battery_saver(); break;
    case PMODE_LOW_POWER:      apply_low_power(); break;
    case PMODE_SHUTDOWN:       apply_low_power(); break;
    case PMODE_USB_ONLY:       apply_usb_only(); break;
    default:                   apply_plugged_in(); break;
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * CPU FREQUENCY SCALING
 * ────────────────────────────────────────────────────────────────────────── */

static uint16_t s_current_freq_mhz = 240;

static void set_cpu_freq(uint16_t mhz) {
  if (mhz == s_current_freq_mhz) return;
  if (mhz != 80 && mhz != 160 && mhz != 240) return;

#if CONFIG_PM_ENABLE
  esp_pm_config_esp32s3_t pm_cfg = {
    .max_freq_mhz = (int)mhz,
    .min_freq_mhz = (int)(mhz > 80 ? 80 : mhz),
    .light_sleep_enable = (mhz < 240),
  };
  if (esp_pm_configure(&pm_cfg) == ESP_OK) {
    s_current_freq_mhz = mhz;
  }
#else
  setCpuFrequencyMhz(mhz);
  s_current_freq_mhz = mhz;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * WIFI POWER SAVE
 * ────────────────────────────────────────────────────────────────────────── */

static uint8_t s_current_wifi_ps = 0;

static void set_wifi_ps(uint8_t mode) {
  if (mode == s_current_wifi_ps) return;
  wifi_ps_type_t ps;
  switch (mode) {
    case 1: ps = WIFI_PS_MIN_MODEM; break;
    case 2: ps = WIFI_PS_MAX_MODEM; break;
    default: ps = WIFI_PS_NONE; break;
  }
  if (esp_wifi_set_ps(ps) == ESP_OK) {
    s_current_wifi_ps = mode;
  }
}

/* ──────────────────────────────────────────────────────────────────────────
 * MODE TRANSITION
 * ────────────────────────────────────────────────────────────────────────── */

static void transition_to(power_mode_t next) {
  if (next == s_mode) return;
  s_prev_mode = s_mode;
  s_mode = next;
  s_mode_entered_ms = millis();
  s_transitions++;

  apply_mode(next);
  set_cpu_freq(s_features.cpu_freq_mhz);
  if (s_features.wifi_ap || s_features.wifi_sta) {
    set_wifi_ps(s_features.wifi_ps_mode);
  }

  if (s_cb) s_cb(s_prev_mode, next);

  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
             "Power policy: mode changed",
             policy_mode_name(next));
}

/* ──────────────────────────────────────────────────────────────────────────
 * MODE EVALUATION
 * ────────────────────────────────────────────────────────────────────────── */

static power_mode_t evaluate(const power_state_t* pwr) {
  if (!pwr->battery_present) return PMODE_USB_ONLY;

  if (pwr->power_source == POWER_SOURCE_USB ||
      pwr->charge_state == CHARGE_STATE_CHARGING ||
      pwr->charge_state == CHARGE_STATE_FULL) {
    return PMODE_PLUGGED_IN;
  }

  uint8_t soc = pwr->soc_pct;
  if (soc <= s_cfg.shutdown_threshold_pct) return PMODE_SHUTDOWN;
  if (soc <= s_cfg.low_power_threshold_pct) return PMODE_LOW_POWER;
  if (soc <= s_cfg.saver_threshold_pct) return PMODE_BATTERY_SAVER;
  return PMODE_BATTERY_NORMAL;
}

}  /* namespace policy */


/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool policy_init(const policy_config_t* cfg) {
  using namespace policy;
  if (s_initialized) return true;
  s_cfg = cfg ? *cfg : (policy_config_t)POLICY_CONFIG_DEFAULT;

  /* Evaluate the actual power state at boot so we don't start in
   * PMODE_PLUGGED_IN for 2+ seconds on a low-battery wake — that
   * wastes energy and can brownout marginal cells. */
  power_state_t pwr;
  power_mode_t initial_mode = PMODE_PLUGGED_IN;
  if (power_get_state(&pwr)) {
    initial_mode = evaluate(&pwr);
  }

  s_mode = initial_mode;
  s_prev_mode = initial_mode;
  s_mode_entered_ms = millis();
  s_transitions = 0;
  s_deep_sleep_pending = false;
  s_last_deep_sleep_ms = 0;
  s_pending_mode = initial_mode;
  s_pending_since = 0;

  apply_mode(initial_mode);
  set_cpu_freq(s_features.cpu_freq_mhz);
  if (s_features.wifi_ap || s_features.wifi_sta) {
    set_wifi_ps(s_features.wifi_ps_mode);
  }

  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
             "Power policy engine initialized",
             policy_mode_name(initial_mode));
  return true;
}

void policy_deinit(void) {
  using namespace policy;
  if (!s_initialized) return;
  s_cb = nullptr;
  s_initialized = false;
}

void policy_set_mode_change_callback(policy_mode_change_cb_t cb) {
  policy::s_cb = cb;
}

bool policy_process(void) {
  using namespace policy;
  if (!s_initialized) return false;

  power_state_t pwr;
  if (!power_get_state(&pwr)) return false;

  if (!s_cfg.auto_mode) return false;

  power_mode_t target = evaluate(&pwr);

  /* Hysteresis: require the target mode to be stable before
   * transitioning. Downgrades (more urgent) need 2s; upgrades
   * (less urgent, avoid flapping) need 5s. */
  if (target != s_mode) {
    bool is_downgrade = (target > s_mode);
    if (target == s_pending_mode) {
      uint32_t required_ms = is_downgrade ? 2000 : 5000;
      if ((millis() - s_pending_since) >= required_ms) {
        transition_to(target);
        s_pending_mode = target;
        return true;
      }
    } else {
      s_pending_mode = target;
      s_pending_since = millis();
    }
  } else {
    s_pending_mode = s_mode;
  }

  /* Deep sleep scheduling for LOW_POWER mode. Re-check power state
   * right before arming sleep — if USB was just plugged in, the
   * evaluate() call above will have set target to PLUGGED_IN and
   * the hysteresis timer is already ticking. But for extra safety,
   * never request sleep while charging. */
  if (s_mode == PMODE_LOW_POWER && !power_is_charging()) {
    uint32_t awake_ms = millis() - s_mode_entered_ms;
    uint32_t since_last = millis() - s_last_deep_sleep_ms;
    if (awake_ms > (s_cfg.low_power_wake_sec * 1000UL) &&
        since_last > (s_cfg.low_power_wake_sec * 1000UL)) {
      s_deep_sleep_pending = true;
    }
  } else {
    s_deep_sleep_pending = false;
  }

  /* Shutdown mode triggers graceful shutdown. */
  if (s_mode == PMODE_SHUTDOWN && pwr.battery_present) {
    power_graceful_shutdown();
  }

  return false;
}

const policy_features_t* policy_get_features(void) {
  return &policy::s_features;
}

bool policy_get_state(policy_state_t* out) {
  using namespace policy;
  if (!out || !s_initialized) return false;
  out->mode = s_mode;
  out->prev_mode = s_prev_mode;
  out->mode_entered_ms = s_mode_entered_ms;
  out->transitions = s_transitions;
  out->auto_mode = s_cfg.auto_mode;
  out->deep_sleep_pending = s_deep_sleep_pending;
  out->features = s_features;
  return true;
}

power_mode_t policy_get_mode(void) {
  return policy::s_mode;
}

const char* policy_mode_name(power_mode_t mode) {
  switch (mode) {
    case PMODE_PLUGGED_IN:     return "plugged_in";
    case PMODE_BATTERY_NORMAL: return "battery_normal";
    case PMODE_BATTERY_SAVER:  return "battery_saver";
    case PMODE_LOW_POWER:      return "low_power";
    case PMODE_SHUTDOWN:       return "shutdown";
    case PMODE_USB_ONLY:       return "usb_only";
    default:                   return "unknown";
  }
}

void policy_set_mode(power_mode_t mode) {
  using namespace policy;
  /* SECURITY: reject manual transitions to LOW_POWER/SHUTDOWN.
   * These modes disable most sensors, creating unwitnessed windows.
   * An attacker with API access could force the device blind by
   * setting LOW_POWER mode. Only the battery state machine (auto
   * mode) can trigger these — not REST API, not serial commands. */
  if (mode == PMODE_LOW_POWER || mode == PMODE_SHUTDOWN) {
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SYSTEM,
               "Power policy: manual LOW_POWER/SHUTDOWN rejected",
               "only auto-mode can trigger these");
    return;
  }
  s_cfg.auto_mode = false;
  transition_to(mode);
}

void policy_set_auto(bool enable) {
  policy::s_cfg.auto_mode = enable;
  if (enable) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM,
               "Power policy: auto mode enabled", nullptr);
  }
}

bool policy_should_deep_sleep(void) {
  return policy::s_deep_sleep_pending;
}

void policy_ack_deep_sleep(void) {
  policy::s_deep_sleep_pending = false;
  policy::s_last_deep_sleep_ms = millis();
}

uint32_t policy_get_sleep_duration_sec(void) {
  return policy::s_cfg.low_power_sleep_sec;
}

}  /* extern "C" */
