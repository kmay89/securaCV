/*
 * SecuraCV Canary WAP — Power Policy Engine (header-only)
 *
 * Runtime state machine that adapts device behavior to the current
 * power source and battery level. Unlike feature flags (compile-time),
 * the policy engine makes runtime decisions about what to enable,
 * disable, or throttle:
 *
 *   PLUGGED_IN      USB power, all features at full rate.
 *   BATTERY_NORMAL  Battery > 50%, camera off, reduced poll rates.
 *   BATTERY_SAVER   Battery 15-50%, WiFi power save, CSI off.
 *   LOW_POWER       Battery 5-15%, deep sleep cycles, minimal sensing.
 *   SHUTDOWN        Battery < 5%, graceful chain closure + deep sleep.
 *   USB_ONLY        No battery detected, same as PLUGGED_IN.
 *
 * The engine reads from power_monitor (battery state) and writes
 * feature-enable signals that the main loop checks each iteration.
 * It does NOT directly call into sensor libraries — that coupling
 * stays in the .ino so the policy engine remains a pure decision layer.
 *
 * Ported from PIO securacv_power_policy to header-only WAP form.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_POWER_POLICY_H
#define SECURACV_POWER_POLICY_H

#include <Arduino.h>
#include <string.h>
#include <esp_wifi.h>

#include "build_config.h"
#include "log_level.h"
#include "power_monitor.h"

// ════════════════════════════════════════════════════════════════════════════
// POWER MODES
// ════════════════════════════════════════════════════════════════════════════

enum PowerPolicyMode : uint8_t {
  PMODE_PLUGGED_IN      = 0,
  PMODE_BATTERY_NORMAL  = 1,
  PMODE_BATTERY_SAVER   = 2,
  PMODE_LOW_POWER       = 3,
  PMODE_SHUTDOWN        = 4,
  PMODE_USB_ONLY        = 5,
};

// ════════════════════════════════════════════════════════════════════════════
// FEATURE ENABLE/DISABLE SIGNALS
//
// Each field is a runtime override. true = feature should run if compiled
// in; false = feature should be stopped/skipped this loop. The main loop
// ANDs these with the compile-time FEATURE_* flags.
// ════════════════════════════════════════════════════════════════════════════

struct PolicyFeatures {
  bool     wifi_ap;
  bool     wifi_sta;
  bool     http_server;
  bool     camera_peek;
  bool     csi;
  bool     acoustic;
  bool     touch;
  bool     ir_rmt;
  bool     temp_tamper;
  bool     vision;
  bool     gnss;
  bool     mqtt;
  bool     mesh;
  uint32_t record_interval_ms;
  uint16_t cpu_freq_mhz;
  uint8_t  wifi_ps_mode;       // 0=NONE, 1=MIN_MODEM, 2=MAX_MODEM
};

// ════════════════════════════════════════════════════════════════════════════
// NAMESPACE — header-only implementation
// ════════════════════════════════════════════════════════════════════════════

namespace power_policy {

// ────────────────────────────────────────────────────────────────────────────
// CONFIG DEFAULTS
// ────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t  NORMAL_THRESHOLD_PCT     = 50;
static constexpr uint8_t  SAVER_THRESHOLD_PCT      = 15;
static constexpr uint8_t  LOW_POWER_THRESHOLD_PCT  = 5;
static constexpr uint8_t  SHUTDOWN_THRESHOLD_PCT   = 3;
static constexpr uint32_t LOW_POWER_WAKE_SEC       = 5;
static constexpr uint32_t LOW_POWER_SLEEP_SEC      = 55;
static constexpr uint32_t SHUTDOWN_WAKE_SEC        = 1800;

// Default record interval (matches WAP's RECORD_INTERVAL_MS)
static constexpr uint32_t DEFAULT_RECORD_INTERVAL_MS = 1000;

// Hysteresis timing
static constexpr uint32_t DOWNGRADE_HYSTERESIS_MS = 2000;
static constexpr uint32_t UPGRADE_HYSTERESIS_MS   = 5000;

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool           s_initialized      = false;
static bool           s_auto_mode        = true;
static PowerPolicyMode s_mode            = PMODE_PLUGGED_IN;
static PowerPolicyMode s_prev_mode       = PMODE_PLUGGED_IN;
static uint32_t       s_mode_entered_ms  = 0;
static uint32_t       s_transitions      = 0;
static bool           s_deep_sleep_pending = false;
static uint32_t       s_last_deep_sleep_ms = 0;

// Hysteresis tracking
static PowerPolicyMode s_pending_mode    = PMODE_PLUGGED_IN;
static uint32_t       s_pending_since    = 0;

// Feature signals
static PolicyFeatures s_features         = {};

// Log callback
static void (*s_log_cb)(LogLevel, LogCategory, const char*, const char*) = nullptr;

// CPU freq tracking
static uint16_t s_current_freq_mhz      = 240;
static uint8_t  s_current_wifi_ps        = 0;

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Logging helper
// ────────────────────────────────────────────────────────────────────────────

inline void log_msg(LogLevel lvl, const char* msg, const char* detail = nullptr) {
  if (s_log_cb) s_log_cb(lvl, SCV_CAT_SYSTEM, msg, detail);
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Feature profiles per mode
// ────────────────────────────────────────────────────────────────────────────

inline void apply_plugged_in() {
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
  s_features.record_interval_ms = DEFAULT_RECORD_INTERVAL_MS;
  s_features.cpu_freq_mhz  = 240;
  s_features.wifi_ps_mode  = 0;   // WIFI_PS_NONE
}

inline void apply_battery_normal() {
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
  s_features.wifi_ps_mode  = 1;   // WIFI_PS_MIN_MODEM
}

inline void apply_battery_saver() {
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
  s_features.wifi_ps_mode  = 2;   // WIFI_PS_MAX_MODEM
}

inline void apply_low_power() {
  s_features.wifi_ap       = false;
  // WiFi STA stays on so panic events can still reach HA via MQTT.
  s_features.wifi_sta      = true;
  s_features.http_server   = false;
  s_features.camera_peek   = false;
  s_features.csi           = false;
  // Acoustic stays on: T3 smoke / T4 CO are life-safety events.
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
  s_features.wifi_ps_mode  = 2;   // WIFI_PS_MAX_MODEM
}

inline void apply_mode(PowerPolicyMode mode) {
  switch (mode) {
    case PMODE_PLUGGED_IN:     apply_plugged_in(); break;
    case PMODE_BATTERY_NORMAL: apply_battery_normal(); break;
    case PMODE_BATTERY_SAVER:  apply_battery_saver(); break;
    case PMODE_LOW_POWER:      apply_low_power(); break;
    case PMODE_SHUTDOWN:       apply_low_power(); break;
    case PMODE_USB_ONLY:       apply_plugged_in(); break;
    default:                   apply_plugged_in(); break;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: CPU frequency scaling
// ────────────────────────────────────────────────────────────────────────────

inline void set_cpu_freq(uint16_t mhz) {
  if (mhz == s_current_freq_mhz) return;
  if (mhz != 80 && mhz != 160 && mhz != 240) return;
  setCpuFrequencyMhz(mhz);
  s_current_freq_mhz = mhz;
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: WiFi power save
// ────────────────────────────────────────────────────────────────────────────

inline void set_wifi_ps(uint8_t mode) {
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

// ────────────────────────────────────────────────────────────────────────────
// Human-readable mode name (defined early so transition_to can use it)
// ────────────────────────────────────────────────────────────────────────────

inline const char* mode_name(PowerPolicyMode mode) {
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

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Mode evaluation
// ────────────────────────────────────────────────────────────────────────────

inline PowerPolicyMode evaluate(const PowerState* pwr) {
  if (!pwr->battery_present) return PMODE_USB_ONLY;

  if (pwr->power_source == POWER_SOURCE_USB ||
      pwr->charge_state == CHARGE_STATE_CHARGING ||
      pwr->charge_state == CHARGE_STATE_FULL) {
    return PMODE_PLUGGED_IN;
  }

  uint8_t soc = pwr->soc_pct;
  if (soc <= SHUTDOWN_THRESHOLD_PCT)   return PMODE_SHUTDOWN;
  if (soc <= LOW_POWER_THRESHOLD_PCT)  return PMODE_LOW_POWER;
  if (soc <= SAVER_THRESHOLD_PCT)      return PMODE_BATTERY_SAVER;
  return PMODE_BATTERY_NORMAL;
}

// ────────────────────────────────────────────────────────────────────────────
// INTERNAL: Mode transition
// ────────────────────────────────────────────────────────────────────────────

inline void transition_to(PowerPolicyMode next) {
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

  log_msg(SCV_LOG_INFO, "Power policy: mode changed", mode_name(next));
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

/**
 * Initialize the power policy engine.
 * @param log_callback  Logging callback matching sys_monitor pattern
 */
inline void init(void (*log_callback)(LogLevel, LogCategory, const char*, const char*) = nullptr) {
  if (s_initialized) return;
  s_log_cb = log_callback;

  // Evaluate actual power state at boot so we don't start in
  // PMODE_PLUGGED_IN for 2+ seconds on a low-battery wake
  PowerState pwr;
  PowerPolicyMode initial_mode = PMODE_PLUGGED_IN;
  if (power_monitor::get_state(&pwr)) {
    initial_mode = evaluate(&pwr);
  }

  s_mode            = initial_mode;
  s_prev_mode       = initial_mode;
  s_mode_entered_ms = millis();
  s_transitions     = 0;
  s_deep_sleep_pending = false;
  s_last_deep_sleep_ms = 0;
  s_pending_mode    = initial_mode;
  s_pending_since   = 0;
  s_auto_mode       = true;

  apply_mode(initial_mode);
  set_cpu_freq(s_features.cpu_freq_mhz);
  if (s_features.wifi_ap || s_features.wifi_sta) {
    set_wifi_ps(s_features.wifi_ps_mode);
  }

  s_initialized = true;
  log_msg(SCV_LOG_INFO, "Power policy engine initialized", mode_name(initial_mode));
}

/**
 * Evaluate policy based on current power state.
 * Call once per main loop iteration.
 * @return true if the mode changed
 */
inline bool process() {
  if (!s_initialized) return false;

  PowerState pwr;
  if (!power_monitor::get_state(&pwr)) return false;
  if (!s_auto_mode) return false;

  PowerPolicyMode target = evaluate(&pwr);

  // Hysteresis: require the target mode to be stable before
  // transitioning. Downgrades need 2s; upgrades need 5s.
  if (target != s_mode) {
    bool is_downgrade = (target > s_mode);
    if (target == s_pending_mode) {
      uint32_t required_ms = is_downgrade ? DOWNGRADE_HYSTERESIS_MS : UPGRADE_HYSTERESIS_MS;
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

  // Deep sleep scheduling for LOW_POWER mode
  if (s_mode == PMODE_LOW_POWER && !power_monitor::is_charging()) {
    uint32_t awake_ms = millis() - s_mode_entered_ms;
    uint32_t since_last = millis() - s_last_deep_sleep_ms;
    if (awake_ms > (LOW_POWER_WAKE_SEC * 1000UL) &&
        since_last > (LOW_POWER_WAKE_SEC * 1000UL)) {
      s_deep_sleep_pending = true;
    }
  } else {
    s_deep_sleep_pending = false;
  }

  // Shutdown mode triggers graceful shutdown
  if (s_mode == PMODE_SHUTDOWN && pwr.battery_present) {
    power_monitor::graceful_shutdown();
  }

  return false;
}

/**
 * Get the current power mode.
 */
inline PowerPolicyMode get_mode() {
  return s_mode;
}

/**
 * Get the current feature-enable signals.
 */
inline const PolicyFeatures* get_features() {
  return &s_features;
}

/**
 * Check if deep sleep should be entered this cycle.
 */
inline bool should_deep_sleep() {
  return s_deep_sleep_pending;
}

/**
 * Acknowledge that the deep sleep cycle completed.
 */
inline void ack_deep_sleep() {
  s_deep_sleep_pending = false;
  s_last_deep_sleep_ms = millis();
}

/**
 * Get the configured deep sleep duration in seconds.
 */
inline uint32_t get_sleep_duration_sec() {
  return LOW_POWER_SLEEP_SEC;
}

/**
 * Override the mode manually. Disables auto-mode.
 *
 * SECURITY: LOW_POWER and SHUTDOWN modes are rejected -- they disable
 * most sensors, creating unwitnessed windows. Only the battery state
 * machine (auto mode) can trigger these modes.
 */
inline void set_mode(PowerPolicyMode mode) {
  if (mode == PMODE_LOW_POWER || mode == PMODE_SHUTDOWN) {
    log_msg(SCV_LOG_WARNING, "Power policy: manual LOW_POWER/SHUTDOWN rejected",
            "only auto-mode can trigger these");
    return;
  }
  s_auto_mode = false;
  transition_to(mode);
}

/**
 * Enable/disable automatic mode transitions.
 */
inline void set_auto(bool enable) {
  s_auto_mode = enable;
  if (enable) {
    log_msg(SCV_LOG_INFO, "Power policy: auto mode enabled");
  }
}

/**
 * Print policy status to Serial.
 */
inline void print_status() {
  Serial.println();
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│         POWER POLICY STATUS          │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf("│ Mode       : %-22s │\n", mode_name(s_mode));
  Serial.printf("│ Prev Mode  : %-22s │\n", mode_name(s_prev_mode));
  Serial.printf("│ Auto       : %-22s │\n", s_auto_mode ? "YES" : "NO");
  Serial.printf("│ Transitions: %-22u │\n", s_transitions);
  Serial.printf("│ CPU MHz    : %-22u │\n", s_features.cpu_freq_mhz);
  Serial.printf("│ WiFi PS    : %-22u │\n", s_features.wifi_ps_mode);
  Serial.printf("│ Record Int : %-17u ms │\n", s_features.record_interval_ms);
  Serial.printf("│ Deep Sleep : %-22s │\n", s_deep_sleep_pending ? "PENDING" : "no");
  Serial.println("├─────────────────────────────────────┤");
  Serial.println("│ Feature Gates:                      │");
  Serial.printf("│  WiFi AP    : %-21s │\n", s_features.wifi_ap ? "ON" : "OFF");
  Serial.printf("│  HTTP       : %-21s │\n", s_features.http_server ? "ON" : "OFF");
  Serial.printf("│  Camera     : %-21s │\n", s_features.camera_peek ? "ON" : "OFF");
  Serial.printf("│  CSI        : %-21s │\n", s_features.csi ? "ON" : "OFF");
  Serial.printf("│  Acoustic   : %-21s │\n", s_features.acoustic ? "ON" : "OFF");
  Serial.printf("│  Touch      : %-21s │\n", s_features.touch ? "ON" : "OFF");
  Serial.printf("│  GNSS       : %-21s │\n", s_features.gnss ? "ON" : "OFF");
  Serial.printf("│  MQTT       : %-21s │\n", s_features.mqtt ? "ON" : "OFF");
  Serial.printf("│  Mesh       : %-21s │\n", s_features.mesh ? "ON" : "OFF");
  Serial.println("└─────────────────────────────────────┘");
}

/**
 * Get policy status as JSON.
 */
inline size_t get_json(char* buf, size_t buf_size) {
  int len = snprintf(buf, buf_size,
    "{"
    "\"mode\":\"%s\","
    "\"prev_mode\":\"%s\","
    "\"auto\":%s,"
    "\"transitions\":%u,"
    "\"deep_sleep_pending\":%s,"
    "\"features\":{"
      "\"wifi_ap\":%s,"
      "\"http_server\":%s,"
      "\"camera_peek\":%s,"
      "\"csi\":%s,"
      "\"acoustic\":%s,"
      "\"touch\":%s,"
      "\"gnss\":%s,"
      "\"mqtt\":%s,"
      "\"mesh\":%s,"
      "\"record_interval_ms\":%u,"
      "\"cpu_freq_mhz\":%u,"
      "\"wifi_ps_mode\":%u"
    "}"
    "}",
    mode_name(s_mode),
    mode_name(s_prev_mode),
    s_auto_mode ? "true" : "false",
    s_transitions,
    s_deep_sleep_pending ? "true" : "false",
    s_features.wifi_ap ? "true" : "false",
    s_features.http_server ? "true" : "false",
    s_features.camera_peek ? "true" : "false",
    s_features.csi ? "true" : "false",
    s_features.acoustic ? "true" : "false",
    s_features.touch ? "true" : "false",
    s_features.gnss ? "true" : "false",
    s_features.mqtt ? "true" : "false",
    s_features.mesh ? "true" : "false",
    s_features.record_interval_ms,
    s_features.cpu_freq_mhz,
    s_features.wifi_ps_mode
  );
  return (len > 0 && len < (int)buf_size) ? (size_t)len : 0;
}

} // namespace power_policy

#endif // SECURACV_POWER_POLICY_H
