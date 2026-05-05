/*
 * SecuraCV Canary — Touch Sensor (ESP32-S3 native touch_pad_*)
 *
 * Wraps the ESP32-S3's capacitive-touch peripheral for three privacy-safe
 * usage modes:
 *
 *   • PANIC     — long-press (default ≥ 1.5 s) on a configured pad emits
 *                 a `silent_panic` event. The pad can be a sticker
 *                 electrode hidden under a desk or bedside table; the
 *                 device is silent on press (no LED flash, no audible
 *                 chirp), so a person under duress can trigger an alert
 *                 without a third party noticing.
 *
 *   • TAMPER    — a sustained release-from-baseline (e.g. case opened,
 *                 device unmounted) emits an `enclosure_tamper` event.
 *                 The baseline self-calibrates over the first
 *                 TOUCH_BASELINE_MS after init.
 *
 *   • APPROACH  — proximity threshold (the touch peripheral senses a
 *                 hand 1–3 cm from the pad without contact); emits
 *                 `approach_detected`. Useful for "wake the dashboard"
 *                 or a courtesy LED.
 *
 * PRIVACY: the only outputs that cross this module are
 *   {event_type, time_bucket, confidence}.
 * No raw counts, no per-frame timestamps, no pad identity beyond the
 * channel index used for diagnostics.
 *
 * The peripheral is polled from the main loop via touch_process(); no
 * interrupts are registered (keeps the privacy surface small).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_TOUCH_H
#define SECURACV_TOUCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Default pad: GPIO 4 (D3 on the XIAO breakout) → Touch4. Avoids the
 * SD-SPI pins (D8/D9/D10), the TAMPER_GPIO=2 pin, and the BOOT button
 * (GPIO 0). Override at compile time with -DTOUCH_PIN_NUM=N. */
#ifndef TOUCH_PIN_NUM
  #define TOUCH_PIN_NUM   4
#endif

#define TOUCH_PANIC_HOLD_MS_DEFAULT      1500
#define TOUCH_TAMPER_HOLD_MS_DEFAULT     5000
#define TOUCH_BASELINE_MS_DEFAULT        2000   /* self-calibrate window */
#define TOUCH_RELATIVE_THRESHOLD_PCT     20     /* press = baseline * 80% */
#define TOUCH_APPROACH_THRESHOLD_PCT     8      /* approach = baseline * 92% */

typedef enum {
  TOUCH_EVENT_NONE             = 0,
  TOUCH_EVENT_SILENT_PANIC     = 1,
  TOUCH_EVENT_ENCLOSURE_TAMPER = 2,
  TOUCH_EVENT_APPROACH         = 3,
} touch_event_type_t;

typedef struct {
  uint8_t  event_type;     /* touch_event_type_t */
  uint8_t  confidence;     /* 0..100 */
  uint8_t  time_bucket;    /* 10-min daily bucket (0..143), matches CSI/audio */
  uint8_t  pad_channel;    /* the touch channel that triggered (1..14 on S3) */
} touch_event_t;

typedef struct {
  /* Touch channel (TOUCH_PAD_NUMx). Defaults to TOUCH_PIN_NUM. */
  int      channel;

  /* How long the pad must register a press before silent panic fires. */
  uint16_t panic_hold_ms;

  /* How long the pad must read above-baseline (case removed) before
   * enclosure-tamper fires. */
  uint16_t tamper_hold_ms;

  /* Baseline auto-calibration window after init. */
  uint16_t baseline_ms;

  /* Enable each detector independently. */
  bool     panic_enabled;
  bool     tamper_enabled;
  bool     approach_enabled;
} touch_config_t;

#define TOUCH_CONFIG_DEFAULT { \
    /*.channel*/          TOUCH_PIN_NUM, \
    /*.panic_hold_ms*/    TOUCH_PANIC_HOLD_MS_DEFAULT,    \
    /*.tamper_hold_ms*/   TOUCH_TAMPER_HOLD_MS_DEFAULT,   \
    /*.baseline_ms*/      TOUCH_BASELINE_MS_DEFAULT,      \
    /*.panic_enabled*/    true,                           \
    /*.tamper_enabled*/   true,                           \
    /*.approach_enabled*/ false                           \
}

typedef struct {
  uint32_t reads_total;
  uint32_t panic_events;
  uint32_t tamper_events;
  uint32_t approach_events;
  uint16_t baseline_value;       /* learned baseline reading */
  uint16_t last_value;           /* most recent filtered reading */
  bool     baseline_locked;      /* true once baseline_ms has elapsed */
} touch_stats_t;

typedef void (*touch_event_cb_t)(const touch_event_t* evt);

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the ESP32-S3 touch peripheral and configure the pad.
 * Returns false if the chip doesn't support touch (e.g. running on a
 * non-S3 target) or if peripheral init fails. */
bool touch_init(const touch_config_t* config);

/* Tear down the peripheral and reset baseline. */
void touch_deinit(void);

/* Begin polling. After this call, touch_process() drives the state
 * machines and may fire callbacks. */
bool touch_start(void);

/* Stop polling. */
void touch_stop(void);

bool touch_is_running(void);

/* Register the event callback (one slot — replacing). */
void touch_set_event_callback(touch_event_cb_t cb);

/* Pump from the main loop. Reads the filtered touch value at most once
 * per ~50 ms; runs panic/tamper/approach state machines; fires the
 * callback synchronously on a confirmed event. Returns true if a read
 * happened on this call. */
bool touch_process(void);

/* Read aggregate diagnostics. */
bool touch_get_stats(touch_stats_t* out);

const char* touch_event_name(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_TOUCH_H */
