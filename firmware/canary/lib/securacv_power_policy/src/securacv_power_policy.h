/*
 * SecuraCV Canary — Power Policy Engine
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
 *
 * The engine reads from securacv_power (battery state) and writes
 * feature-enable signals that main.cpp checks each loop iteration.
 * It does NOT directly call into sensor libraries — that coupling
 * stays in main.cpp so the policy engine remains a pure decision layer.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_POWER_POLICY_H
#define SECURACV_POWER_POLICY_H

#include <stdint.h>
#include <stdbool.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  POWER MODES                                                              */
/* ────────────────────────────────────────────────────────────────────────── */

typedef enum {
  PMODE_PLUGGED_IN      = 0,
  PMODE_BATTERY_NORMAL  = 1,
  PMODE_BATTERY_SAVER   = 2,
  PMODE_LOW_POWER       = 3,
  PMODE_SHUTDOWN        = 4,
  PMODE_USB_ONLY        = 5,
} power_mode_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  FEATURE ENABLE/DISABLE SIGNALS                                           */
/*                                                                           */
/*  Each field is a runtime override. true = feature should run if compiled   */
/*  in; false = feature should be stopped/skipped this loop. main.cpp ANDs   */
/*  these with the compile-time FEATURE_* flags.                             */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
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
  uint8_t  wifi_ps_mode;
} policy_features_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  POLICY CONFIG                                                            */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t  normal_threshold_pct;
  uint8_t  saver_threshold_pct;
  uint8_t  low_power_threshold_pct;
  uint8_t  shutdown_threshold_pct;
  uint32_t low_power_wake_sec;
  uint32_t low_power_sleep_sec;
  uint32_t shutdown_wake_interval_sec;
  bool     auto_mode;
} policy_config_t;

#define POLICY_CONFIG_DEFAULT { \
    /*.normal_threshold_pct*/       50,   \
    /*.saver_threshold_pct*/        15,   \
    /*.low_power_threshold_pct*/    5,    \
    /*.shutdown_threshold_pct*/     3,    \
    /*.low_power_wake_sec*/         5,    \
    /*.low_power_sleep_sec*/        55,   \
    /*.shutdown_wake_interval_sec*/ 1800, \
    /*.auto_mode*/                  true  \
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  POLICY STATE SNAPSHOT                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t           mode;
  uint8_t           prev_mode;
  uint32_t          mode_entered_ms;
  uint32_t          transitions;
  bool              auto_mode;
  bool              deep_sleep_pending;
  policy_features_t features;
} policy_state_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  EVENT CALLBACK                                                           */
/* ────────────────────────────────────────────────────────────────────────── */

typedef void (*policy_mode_change_cb_t)(power_mode_t prev, power_mode_t next);

/* ────────────────────────────────────────────────────────────────────────── */
/*  C API                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

bool policy_init(const policy_config_t* cfg);
void policy_deinit(void);

void policy_set_mode_change_callback(policy_mode_change_cb_t cb);

/* Evaluate policy based on current power state. Call once per main loop
 * iteration. Returns true if the mode changed. */
bool policy_process(void);

/* Get the current feature-enable signals. main.cpp reads this each
 * loop to decide which sensor process() calls to make. */
const policy_features_t* policy_get_features(void);

/* Get full policy state snapshot. */
bool policy_get_state(policy_state_t* out);

/* Get current mode. */
power_mode_t policy_get_mode(void);

/* Human-readable mode name. */
const char* policy_mode_name(power_mode_t mode);

/* Override the mode manually. Disables auto-mode until the next
 * call to policy_set_auto(true) or reboot.
 *
 * SECURITY: LOW_POWER and SHUTDOWN modes are rejected — they disable
 * most sensors and create unwitnessed windows. An attacker with API
 * access must not be able to blind the device. Only the automatic
 * battery-driven evaluation path can trigger these modes. */
void policy_set_mode(power_mode_t mode);

/* Enable/disable automatic mode transitions. */
void policy_set_auto(bool enable);

/* Check if deep sleep should be entered this cycle. The caller
 * (main.cpp) is responsible for flushing state and calling
 * lowpower_enter_deep_sleep(). */
bool policy_should_deep_sleep(void);

/* Acknowledge that the deep sleep cycle completed (called after
 * wake from deep sleep to reset the pending flag). */
void policy_ack_deep_sleep(void);

/* Get the configured deep sleep duration in seconds for low-power
 * mode cycling. main.cpp uses this to arm the wake timer. */
uint32_t policy_get_sleep_duration_sec(void);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_POWER_POLICY_H */
