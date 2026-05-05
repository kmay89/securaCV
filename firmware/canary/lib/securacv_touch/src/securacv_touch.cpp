/*
 * SecuraCV Canary — Touch Sensor implementation
 *
 * Polled wrapper around the ESP32-S3 touch_pad_* peripheral (driver/
 * touch_sensor.h, ESP-IDF 4.4 baseline). Self-calibrating baseline,
 * three independent state machines (panic / tamper / approach), all
 * running off the main loop without interrupts.
 *
 * On non-S3 targets the wrapper compiles to a runtime no-op so the
 * lib is safe to include unconditionally; CONFIG_IDF_TARGET_ESP32S3
 * gates the actual peripheral calls.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_touch.h"

#include <Arduino.h>
#include <string.h>

#include "log_level.h"
#include "securacv_witness.h"   /* log_health() */

/* ESP32-S3 and S2 share the same touch_pad V2-ish API in IDF 4.4 (the
 * one we use here: touch_pad_config(pad), touch_pad_filter_start(period),
 * touch_pad_read_filtered(pad, uint32_t*)). The original ESP32 has a
 * different signature shape (touch_pad_config takes a threshold,
 * touch_pad_read_filtered uses uint16_t*) so we don't compile this lib
 * for plain ESP32 — the active canary tree only targets S3 anyway. */
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)
  #define SECURACV_HAVE_TOUCH 1
  extern "C" {
    #include <driver/touch_sensor.h>
    #include <esp_err.h>
  }
#else
  #define SECURACV_HAVE_TOUCH 0
#endif

namespace touch {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool s_initialized = false;
static bool s_running     = false;
static touch_config_t   s_cfg = TOUCH_CONFIG_DEFAULT;
static touch_event_cb_t s_cb  = nullptr;

/* Baseline auto-calibration. Records the running mean of the first
 * baseline_ms readings; once locked, downstream state machines start. */
static uint32_t s_baseline_sum     = 0;
static uint16_t s_baseline_samples = 0;
static uint16_t s_baseline_value   = 0;
static uint32_t s_baseline_started_ms = 0;
static bool     s_baseline_locked  = false;

/* State-machine bookkeeping. The pad is "pressed" when filtered <
 * baseline * (1 - REL_THRESHOLD_PCT/100). */
static bool     s_pressed_now     = false;
static uint32_t s_press_started_ms = 0;
static bool     s_panic_fired     = false;

/* Tamper: pad reads consistently *above* baseline (case opened, pad
 * disconnected from ground). When `released_above` persists longer
 * than tamper_hold_ms we fire `enclosure_tamper`. */
static bool     s_released_above_now = false;
static uint32_t s_release_started_ms = 0;
static bool     s_tamper_fired       = false;

/* Approach: short-lived, debounced (don't re-fire within 1.5 s). */
static uint32_t s_last_approach_ms   = 0;

/* Stats. */
static uint32_t s_reads_total      = 0;
static uint32_t s_panic_events     = 0;
static uint32_t s_tamper_events    = 0;
static uint32_t s_approach_events  = 0;
static uint16_t s_last_value       = 0;

/* Polling cadence. */
static uint32_t s_last_read_ms     = 0;
static constexpr uint32_t READ_INTERVAL_MS = 50;  /* 20 Hz */

/* ──────────────────────────────────────────────────────────────────────────
 * EVENT EMITTER
 * ────────────────────────────────────────────────────────────────────────── */

static void emit_event(uint8_t type, uint8_t confidence, uint32_t now_ms) {
  if (!s_cb) return;
  touch_event_t evt = {};
  evt.event_type  = type;
  evt.confidence  = confidence;
  evt.pad_channel = (uint8_t)s_cfg.channel;
  evt.time_bucket = (uint8_t)((now_ms / (10UL * 60UL * 1000UL)) % 144);
  s_cb(&evt);
  /* Don't outlive the call. */
  memset(&evt, 0, sizeof(evt));
}

/* ──────────────────────────────────────────────────────────────────────────
 * PERIPHERAL BRING-UP / TEAR-DOWN  (gated by SECURACV_HAVE_TOUCH)
 * ────────────────────────────────────────────────────────────────────────── */

#if SECURACV_HAVE_TOUCH

static bool peripheral_open() {
  esp_err_t err = touch_pad_init();
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "init err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Touch: peripheral init failed", d);
    return false;
  }

  /* The S3 ROM defaults are typically fine: 16-cycle measurements with
   * a 1024-cycle inter-measurement interval. We just configure the pad
   * we want and let the peripheral free-run. */
  err = touch_pad_config((touch_pad_t)s_cfg.channel);
  if (err != ESP_OK) {
    char d[32]; snprintf(d, sizeof(d), "config err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Touch: pad config failed", d);
    touch_pad_deinit();
    return false;
  }

  /* Start the on-chip 10-tap IIR software filter so single-sample
   * noise can't trigger the state machines. */
  err = touch_pad_filter_start(10);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    /* INVALID_STATE just means the filter was already running — fine. */
    char d[32]; snprintf(d, sizeof(d), "filter err=0x%x", (unsigned)err);
    log_health(LOG_LEVEL_WARNING, LOG_CAT_SENSOR,
               "Touch: filter start failed", d);
  }

  return true;
}

static void peripheral_close() {
  touch_pad_filter_stop();
  touch_pad_deinit();
}

/* Read the filtered touch value. On ESP32-S3 in IDF 4.4 the API is
 * `touch_pad_read_filtered(pad, uint32_t*)`. Returns 0 on read error,
 * which the state machine treats as never-pressed. */
static uint16_t peripheral_read_filtered() {
  uint32_t v = 0;
  esp_err_t err = touch_pad_read_filtered((touch_pad_t)s_cfg.channel, &v);
  if (err != ESP_OK) return 0;
  if (v > 0xFFFFu) v = 0xFFFFu;
  return (uint16_t)v;
}

#else  /* !SECURACV_HAVE_TOUCH */

static bool peripheral_open()  { return false; }
static void peripheral_close() {}
static uint16_t peripheral_read_filtered() { return 0; }

#endif  /* SECURACV_HAVE_TOUCH */

/* ──────────────────────────────────────────────────────────────────────────
 * STATE MACHINES
 * ────────────────────────────────────────────────────────────────────────── */

static void reset_state(uint32_t now_ms) {
  s_baseline_sum     = 0;
  s_baseline_samples = 0;
  s_baseline_value   = 0;
  s_baseline_started_ms = now_ms;
  s_baseline_locked  = false;
  s_pressed_now      = false;
  s_press_started_ms = 0;
  s_panic_fired      = false;
  s_released_above_now = false;
  s_release_started_ms = 0;
  s_tamper_fired     = false;
  s_last_approach_ms = 0;
}

static void update_state_machines(uint16_t value, uint32_t now_ms) {
  s_last_value = value;
  s_reads_total++;

  /* Stage 1 — baseline auto-calibration. */
  if (!s_baseline_locked) {
    if (value > 0) {
      s_baseline_sum     += value;
      s_baseline_samples += 1;
    }
    if ((now_ms - s_baseline_started_ms) >= s_cfg.baseline_ms &&
        s_baseline_samples > 0) {
      s_baseline_value  = (uint16_t)(s_baseline_sum / s_baseline_samples);
      s_baseline_locked = true;
      log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
                 "Touch: baseline locked", nullptr);
    }
    return;  /* don't run detectors until baseline is locked */
  }

  if (s_baseline_value == 0) return;  /* sensor never produced a real reading */

  /* Stage 2 — derive thresholds from baseline. */
  const uint32_t base = s_baseline_value;
  const uint32_t press_thresh    = base * (100 - TOUCH_RELATIVE_THRESHOLD_PCT) / 100;
  const uint32_t approach_thresh = base * (100 - TOUCH_APPROACH_THRESHOLD_PCT) / 100;
  /* "Above baseline" = pad disconnected. We allow ≥ 5% above. */
  const uint32_t above_thresh    = base * 105 / 100;

  /* Stage 3 — panic state machine (long-press). */
  if (s_cfg.panic_enabled) {
    const bool now_pressed = (value > 0 && value <= press_thresh);
    if (now_pressed && !s_pressed_now) {
      s_pressed_now      = true;
      s_press_started_ms = now_ms;
      s_panic_fired      = false;
    } else if (!now_pressed && s_pressed_now) {
      s_pressed_now = false;
    } else if (now_pressed && !s_panic_fired &&
               (now_ms - s_press_started_ms) >= s_cfg.panic_hold_ms) {
      s_panic_fired = true;
      s_panic_events++;
      emit_event(TOUCH_EVENT_SILENT_PANIC, 100, now_ms);
    }
  }

  /* Stage 4 — tamper state machine (sustained above-baseline). */
  if (s_cfg.tamper_enabled) {
    const bool now_above = (value >= above_thresh);
    if (now_above && !s_released_above_now) {
      s_released_above_now = true;
      s_release_started_ms = now_ms;
      s_tamper_fired       = false;
    } else if (!now_above && s_released_above_now) {
      s_released_above_now = false;
    } else if (now_above && !s_tamper_fired &&
               (now_ms - s_release_started_ms) >= s_cfg.tamper_hold_ms) {
      s_tamper_fired = true;
      s_tamper_events++;
      emit_event(TOUCH_EVENT_ENCLOSURE_TAMPER, 100, now_ms);
    }
  }

  /* Stage 5 — approach (proximity threshold without contact). Debounced
   * to one fire per 1.5 s. */
  if (s_cfg.approach_enabled) {
    const bool near_press = (value > 0 &&
                              value <= approach_thresh && value > press_thresh);
    if (near_press && (now_ms - s_last_approach_ms) >= 1500) {
      s_last_approach_ms = now_ms;
      s_approach_events++;
      emit_event(TOUCH_EVENT_APPROACH, 80, now_ms);
    }
  }
}

}  /* namespace touch */

/* ──────────────────────────────────────────────────────────────────────────
 * C API
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" {

bool touch_init(const touch_config_t* config) {
  using namespace touch;
  if (s_initialized) return true;

  s_cfg = config ? *config : (touch_config_t)TOUCH_CONFIG_DEFAULT;
  if (s_cfg.panic_hold_ms  == 0) s_cfg.panic_hold_ms  = TOUCH_PANIC_HOLD_MS_DEFAULT;
  if (s_cfg.tamper_hold_ms == 0) s_cfg.tamper_hold_ms = TOUCH_TAMPER_HOLD_MS_DEFAULT;
  if (s_cfg.baseline_ms    == 0) s_cfg.baseline_ms    = TOUCH_BASELINE_MS_DEFAULT;

  reset_state(millis());

  s_initialized = true;
  log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
             "Touch HAL initialized", "ESP32-S3 native touch_pad");
  return true;
}

void touch_deinit(void) {
  using namespace touch;
  if (!s_initialized) return;
  if (s_running) touch_stop();
  s_cb = nullptr;
  s_initialized = false;
}

bool touch_start(void) {
  using namespace touch;
  if (!s_initialized) return false;
  if (s_running) return true;
  if (!peripheral_open()) {
#if !SECURACV_HAVE_TOUCH
    log_health(LOG_LEVEL_INFO, LOG_CAT_SENSOR,
               "Touch peripheral not available on this target", nullptr);
#endif
    return false;
  }
  reset_state(millis());
  s_running = true;
  return true;
}

void touch_stop(void) {
  using namespace touch;
  if (!s_running) return;
  peripheral_close();
  s_running = false;
}

bool touch_is_running(void) { return touch::s_running; }

void touch_set_event_callback(touch_event_cb_t cb) { touch::s_cb = cb; }

bool touch_process(void) {
  using namespace touch;
  if (!s_running) return false;
  const uint32_t now = millis();
  if ((now - s_last_read_ms) < READ_INTERVAL_MS) return false;
  s_last_read_ms = now;

  const uint16_t v = peripheral_read_filtered();
  update_state_machines(v, now);
  return true;
}

bool touch_get_stats(touch_stats_t* out) {
  using namespace touch;
  if (!out) return false;
  out->reads_total      = s_reads_total;
  out->panic_events     = s_panic_events;
  out->tamper_events    = s_tamper_events;
  out->approach_events  = s_approach_events;
  out->baseline_value   = s_baseline_value;
  out->last_value       = s_last_value;
  out->baseline_locked  = s_baseline_locked;
  return true;
}

const char* touch_event_name(uint8_t type) {
  switch (type) {
    case TOUCH_EVENT_NONE:             return "none";
    case TOUCH_EVENT_SILENT_PANIC:     return "silent_panic";
    case TOUCH_EVENT_ENCLOSURE_TAMPER: return "enclosure_tamper";
    case TOUCH_EVENT_APPROACH:         return "approach";
    default:                           return "unknown";
  }
}

}  /* extern "C" */
