/*
 * SecuraCV Canary — Thermal Watchdog (passive observer)
 *
 * Always-on die-temperature telemetry. The camera's three-state throttle
 * machine (securacv_camera) only runs while a peek stream is active, so
 * without this module the device is thermally blind when idle, sensor
 * read failures vanish silently, and nothing survives a reboot. The
 * watchdog closes those gaps: it samples the shared thermal provider on
 * a fixed cadence, classifies the condition with the same hysteresis
 * ladder the camera uses (as a *shadow* state — observational only),
 * accumulates lifetime history in NVS, and raises advisories through
 * the health log.
 *
 * IT NEVER ACTUATES. No throttling, no resets, no feature gating —
 * the camera state machine remains the sole thermal actuator. This
 * module observes, records, and advises.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_THERMAL_WATCHDOG_H
#define SECURACV_THERMAL_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

/* ────────────────────────────────────────────────────────────────────────── */
/*  ADVISORY BITMASK                                                         */
/* ────────────────────────────────────────────────────────────────────────── */

#define THERMAL_ADV_SENSOR_FAULT  (1u << 0)  /* consecutive read failures   */
#define THERMAL_ADV_CRITICAL      (1u << 1)  /* die >= 85 °C                */
#define THERMAL_ADV_SATURATION    (1u << 2)  /* shadow-PAUSED >= 10 min     */
#define THERMAL_ADV_ENV_LIMITED   (1u << 3)  /* frequent throttle entries   */
#define THERMAL_ADV_COLD          (1u << 4)  /* die < 5 °C                  */

/* ────────────────────────────────────────────────────────────────────────── */
/*  STATE SNAPSHOT                                                           */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  float    die_temp_c;       /* last good sample (fresh cache for APIs)     */
  uint8_t  shadow_state;     /* 0=normal 1=throttled 2=paused — same 70/80
                                thresholds + 5 °C recovery margin as the
                                camera actuator, but purely observational   */
  bool     sensor_ok;        /* false while in a read-failure episode       */
  uint8_t  advisories;       /* THERMAL_ADV_* currently active              */
  uint32_t last_sample_ms;   /* millis() of last good read (0 = never)      */
  uint32_t consec_failures;  /* current failure streak                      */
} thermal_wd_state_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  LIFETIME HISTORY (NVS-persisted, survives reboot)                        */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  int8_t   alltime_min_c;       /* coldest die temp ever seen              */
  int8_t   alltime_max_c;       /* hottest die temp ever seen              */
  uint32_t total_runtime_min;   /* minutes the watchdog has been observing */
  uint32_t throttled_min;       /* cumulative minutes shadow >= THROTTLED  */
  uint32_t paused_min;          /* cumulative minutes shadow == PAUSED     */
  uint32_t throttle_events;     /* NORMAL -> THROTTLED shadow transitions  */
  uint32_t pause_events;        /* -> PAUSED shadow transitions            */
  uint32_t critical_events;     /* >= 85 °C episodes                       */
  uint32_t sensor_fail_events;  /* completed read-failure episodes         */
  uint32_t cold_events;         /* < 5 °C episodes                         */
  uint32_t max_seen_runtime_min;/* total_runtime_min when max was set      */
} thermal_wd_history_t;

/* ────────────────────────────────────────────────────────────────────────── */
/*  C API                                                                    */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* Load persisted history from NVS. Safe to call once from setup(). */
bool thermal_wd_init(void);

/* Pump from loop(). Rate-limits internally (30 s sample cadence,
 * 10 min dirty-flag NVS persist). Cheap no-op between samples. */
void thermal_wd_process(void);

/* Copy the current state snapshot. Returns false before init. */
bool thermal_wd_get_state(thermal_wd_state_t* out);

/* Copy the lifetime history. Returns false before init. */
bool thermal_wd_get_history(thermal_wd_history_t* out);

/* Force an NVS write if anything is dirty. Call before deep sleep
 * or a deliberate reboot so the last partial minutes aren't lost. */
void thermal_wd_persist(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_THERMAL_WATCHDOG_H */
