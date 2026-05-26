/*
 * SecuraCV Canary — Power Management
 *
 * Battery voltage monitoring, charge state detection, SoC estimation,
 * and graceful brownout shutdown for the XIAO ESP32-S3 Sense with a
 * LiPo battery connected to the built-in TP4056 charging circuit.
 *
 * Dual-mode operation:
 *
 *   HARDWARE ADC — a 2:1 voltage divider (two 100K resistors) from
 *     VBAT to GPIO 1 (D0/A0) provides calibrated millivolt readings.
 *     The ESP32-S3's eFuse ADC calibration data produces ±20 mV
 *     accuracy, yielding ±2% SoC for a typical LiPo discharge curve.
 *
 *   SOFTWARE INFERENCE — when no divider is wired, the module watches
 *     USB-powered voltage behavior: a steadily rising ADC suggests
 *     charging (TP4056 is active), a stable high value suggests full,
 *     and any measurable drop from a settled baseline suggests battery
 *     discharge. Less accurate but works with zero hardware modification.
 *
 * Auto-detection at boot: the module reads the ADC and classifies the
 * result. A reading in the 1.2–2.3 V range (corresponds to 2.4–4.6 V
 * with a 2:1 divider) indicates a divider is present. Near-zero or
 * near-rail readings indicate no divider.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_POWER_H
#define SECURACV_POWER_H

#include <stdint.h>
#include <stdbool.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  ENUMS                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

typedef enum {
  POWER_MODE_HW_ADC        = 0,
  POWER_MODE_SW_INFERENCE  = 1,
  POWER_MODE_USB_ONLY      = 2,
} power_monitor_mode_t;

typedef enum {
  POWER_SOURCE_UNKNOWN     = 0,
  POWER_SOURCE_USB         = 1,
  POWER_SOURCE_BATTERY     = 2,
  POWER_SOURCE_USB_ONLY    = 3,
} power_source_t;

typedef enum {
  CHARGE_STATE_UNKNOWN     = 0,
  CHARGE_STATE_CHARGING    = 1,
  CHARGE_STATE_FULL        = 2,
  CHARGE_STATE_DISCHARGING = 3,
  CHARGE_STATE_LOW         = 4,
  CHARGE_STATE_CRITICAL    = 5,
  CHARGE_STATE_NO_BATTERY  = 6,
} charge_state_t;

typedef enum {
  POWER_EVENT_NONE             = 0,
  POWER_EVENT_STATE_CHANGE     = 1,
  POWER_EVENT_LOW_BATTERY      = 2,
  POWER_EVENT_CRITICAL_BATTERY = 3,
  POWER_EVENT_CHARGING_START   = 4,
  POWER_EVENT_CHARGING_DONE    = 5,
  POWER_EVENT_SHUTDOWN         = 6,
} power_event_type_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  CONFIG                                                                   */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t  adc_gpio;
  float    divider_ratio;
  uint16_t capacity_mah;
  uint32_t sample_interval_ms;
  uint8_t  soc_low_pct;
  uint8_t  soc_critical_pct;
  /* Voltage trend window for software inference mode. */
  uint32_t trend_window_ms;
} power_config_t;

#define POWER_CONFIG_DEFAULT { \
    /*.adc_gpio*/            1,     \
    /*.divider_ratio*/       2.0f,  \
    /*.capacity_mah*/        3000,  \
    /*.sample_interval_ms*/  1000,  \
    /*.soc_low_pct*/         15,    \
    /*.soc_critical_pct*/    5,     \
    /*.trend_window_ms*/     60000  \
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  STATE SNAPSHOT                                                           */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t           monitor_mode;
  uint8_t           power_source;
  uint8_t           charge_state;
  uint8_t           soc_pct;
  uint16_t          voltage_mv;
  uint16_t          capacity_mah;
  int16_t           trend_mv_per_min;
  uint32_t          last_sample_ms;
  uint32_t          charge_cycles;
  uint16_t          max_voltage_mv;
  uint16_t          min_voltage_mv;
  uint32_t          samples_taken;
  bool              divider_detected;
  bool              battery_present;
} power_state_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  EVENT CALLBACK                                                           */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t  event_type;
  uint8_t  charge_state;
  uint8_t  soc_pct;
  uint8_t  time_bucket;
} power_event_t;

typedef void (*power_event_cb_t)(const power_event_t* evt);

/* ────────────────────────────────────────────────────────────────────────── */
/*  C API                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

bool power_init(const power_config_t* cfg);
void power_deinit(void);
bool power_start(void);
void power_stop(void);
bool power_is_running(void);

void power_set_event_callback(power_event_cb_t cb);

/* Pump from the main loop. Reads ADC at the configured interval,
 * updates SoC/charge state, fires callbacks on state transitions. */
bool power_process(void);

/* Read the current power state snapshot. Always succeeds if init'd. */
bool power_get_state(power_state_t* out);

/* Set battery capacity at runtime (persisted to NVS). */
void power_set_capacity_mah(uint16_t mah);

/* Initiate graceful shutdown: close witness chain, flush SD, persist
 * battery stats, then enter deep sleep with timer + touch wake. */
void power_graceful_shutdown(void);

/* Check if battery is below the critical threshold. Useful for
 * external code that needs a fast yes/no without a full snapshot. */
bool power_is_critical(void);

bool power_is_charging(void);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_POWER_H */
