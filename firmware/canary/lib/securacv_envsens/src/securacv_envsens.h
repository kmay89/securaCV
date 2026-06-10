/*
 * SecuraCV Canary — Environmental Sensors
 *
 * Reads the ESP32-S3's internal die-temperature sensor at a slow
 * cadence (default once per 60 s, via the shared securacv_thermal
 * provider) and tracks a slow EMA baseline (alpha 1/16 per sample,
 * ~16 min time constant) that absorbs gradual self-heating and HVAC
 * drift. A step ≥ 5 °C from baseline emits a `tamper_temp_drift`
 * event — the kind of signal you'd expect when:
 *
 *   • the case is opened (ambient air rushes in / out — usually a
 *     ~3-8 °C swing within seconds),
 *   • the device is unmounted from a wall and carried to another
 *     room with a different HVAC setpoint,
 *   • a heat gun / hair-dryer is being used to bypass tamper seals.
 *
 * Defense-in-depth, NOT primary tamper detection: the touch-pad lib
 * (Phase 3) catches the more common "case opened" case via its
 * pad-disconnect signature. Temperature drift catches the slower
 * "device removed" case the touch lib can't see.
 *
 * PRIVACY: no raw temperatures cross the module boundary. The event
 * carries only { event_type, time_bucket, confidence } — same shape
 * as every other sensor in the system.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_ENVSENS_H
#define SECURACV_ENVSENS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  ENVSENS_EVENT_NONE          = 0,
  ENVSENS_EVENT_TEMP_DRIFT    = 1,
} envsens_event_type_t;

typedef struct {
  uint8_t event_type;
  uint8_t confidence;
  uint8_t time_bucket;   /* 10-min daily bucket (0..143) */
  uint8_t reserved;
} envsens_event_t;

typedef struct {
  /* How often to sample the die temperature. Default 60 s. */
  uint32_t sample_interval_ms;
  /* Drift threshold in tenths of a degree Celsius. Default 50 = 5.0 °C. */
  uint16_t drift_threshold_tenths_c;
  /* Suppress repeat events for this long after firing. Default 5 min. */
  uint32_t suppress_ms;
  /* Number of samples to average before locking the baseline. */
  uint8_t  baseline_samples;
} envsens_config_t;

#define ENVSENS_CONFIG_DEFAULT { \
    /*.sample_interval_ms*/      60000, \
    /*.drift_threshold_tenths_c*/ 50,   \
    /*.suppress_ms*/             300000, \
    /*.baseline_samples*/        5      \
}

typedef struct {
  uint32_t samples_taken;
  uint32_t drift_events;
  bool     baseline_locked;
  /* Internal-only diagnostics — bucketed to whole degrees so the JSON
   * doesn't reveal a precise temperature. The sensing dashboard only
   * shows the drift-event count, never these. */
  int8_t   baseline_c_rounded;     /* -128..127, whole °C */
  int8_t   last_c_rounded;
} envsens_stats_t;

typedef void (*envsens_event_cb_t)(const envsens_event_t* evt);

#ifdef __cplusplus
extern "C" {
#endif

bool envsens_init(const envsens_config_t* cfg);
void envsens_deinit(void);
bool envsens_start(void);
void envsens_stop(void);
bool envsens_is_running(void);

void envsens_set_event_callback(envsens_event_cb_t cb);

/* Declare a heavy thermal load (camera-peek streaming, sustained radio
 * bursts). While active — and for a 10-min cooldown after it ends — the
 * baseline fast-tracks the die temperature and drift detection is
 * suspended: a 10–20 °C self-heating ramp is indistinguishable from a
 * heat-gun attack by temperature alone, and Seeed documents the XIAO
 * ESP32S3 reaching ~50 °C under plain SoftAP load and ~63 °C streaming
 * camera for an hour. Call from the main loop with the current state;
 * edges are detected internally. */
void envsens_set_high_load(bool active);

/* Pump from the main loop. Performs at most one read per call when
 * the sample interval has elapsed; runs the drift state machine; fires
 * the callback synchronously on a confirmed event. */
bool envsens_process(void);

bool envsens_get_stats(envsens_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_ENVSENS_H */
