/*
 * SecuraCV Canary — Acoustic Event Detection
 *
 * Brings up the on-board PDM microphone (XIAO ESP32-S3 Sense:
 * MSM261D3526H1CPM, GPIO 41 data / GPIO 42 clock) and detects two
 * open-standard alarm cadences that every code-compliant household
 * smoke / CO detector emits:
 *
 *   • T3  — NFPA 72 / ISO 8201 smoke-alarm cadence
 *           three 0.5 s beeps with 0.5 s gaps, then 1.5 s silence
 *           (4.0 s period; common to every UL 217 smoke alarm sold
 *           in North America since 1996)
 *
 *   • T4  — UL 2034 carbon-monoxide cadence
 *           four 100 ms beeps with 100 ms gaps, then 5 s silence
 *           (5.7 s period; common to every UL 2034 CO alarm)
 *
 * PRIVACY BARRIER (enforced by the implementation, asserted at compile
 * time by the C-API contract — see audio_event_t below):
 *
 *   1. The PDM driver hands us 16 kHz int16 mono samples. We compute
 *      a single 32-bit RMS scalar per 20 ms window, then ZERO the
 *      sample buffer immediately. The raw samples never outlive the
 *      callback that produces them.
 *   2. The RMS envelope is bucketed into a binary on/off signal with
 *      hysteresis. Only on/off transitions and their durations are
 *      retained — no envelope shape, no spectral content, no speech-
 *      band data.
 *   3. The only thing that crosses the module boundary is a fixed-size
 *      audio_event_t containing {event_type, confidence, time_bucket,
 *      cycle_count}. No timestamps below the 10-minute daily bucket
 *      cross the barrier.
 *   4. Cadence detection is purely temporal. There is no MFCC, no
 *      classifier, no audio fingerprint. Speech is structurally
 *      impossible to recover from a binary on/off envelope.
 *
 * This is the "no-ML, no-IP-risk" first slice of the broader Phase 2
 * acoustic roadmap. Glass-break / doorbell / knock detection (which
 * does require a small classifier) is a separate, optional follow-up
 * and is gated behind its own feature flag.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_AUDIO_H
#define SECURACV_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

#define AUDIO_SAMPLE_RATE_HZ      16000   /* PDM mic native; well-supported */
#define AUDIO_FRAME_MS            20      /* 50 Hz envelope rate */
#define AUDIO_FRAME_SAMPLES       (AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_MS / 1000)

/* ──────────────────────────────────────────────────────────────────────────
 * TYPES
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
  AUDIO_EVENT_NONE            = 0,
  AUDIO_EVENT_T3_SMOKE_ALARM  = 1,   /* NFPA 72 / ISO 8201 */
  AUDIO_EVENT_T4_CO_ALARM     = 2,   /* UL 2034 */
} audio_event_type_t;

/* The ONLY structure that crosses the privacy barrier. Fixed size, no
 * pointers, no per-frame timestamp. */
typedef struct {
  uint8_t  event_type;     /* audio_event_type_t */
  uint8_t  confidence;     /* 0..100 — driven by timing-error fit */
  uint8_t  time_bucket;    /* 10-min daily bucket (0..143), matches CSI */
  uint8_t  reserved;
  uint16_t cycle_count;    /* number of consecutive cadence cycles matched */
} audio_event_t;

typedef struct {
  uint8_t  channel;            /* I2S channel index (currently unused; 0) */
  uint16_t sample_rate_hz;     /* default AUDIO_SAMPLE_RATE_HZ */
  uint16_t frame_ms;           /* default AUDIO_FRAME_MS */
  /* On/off thresholds for the RMS envelope. Default OFF=400 / ON=800
   * (ratio 2:1 hysteresis) — works at typical room SPL with a UL alarm
   * 1–3 m from the device. Tune up if the room is noisy. */
  uint16_t rms_on_threshold;
  uint16_t rms_off_threshold;
} audio_config_t;

#define AUDIO_CONFIG_DEFAULT { \
    /*.channel*/           0, \
    /*.sample_rate_hz*/    AUDIO_SAMPLE_RATE_HZ, \
    /*.frame_ms*/          AUDIO_FRAME_MS, \
    /*.rms_on_threshold*/  800, \
    /*.rms_off_threshold*/ 400  \
}

typedef struct {
  uint32_t frames_processed;     /* 20 ms RMS windows computed */
  uint32_t envelope_samples;     /* same as frames_processed; alias */
  uint32_t on_transitions;       /* envelope crossed below→above ON */
  uint32_t off_transitions;      /* envelope crossed above→below OFF */
  uint32_t t3_detected;          /* T3 cycles confirmed since boot */
  uint32_t t4_detected;          /* T4 cycles confirmed since boot */
  uint32_t i2s_read_errors;      /* underflow / DMA error counter */
} audio_stats_t;

/* A single recent on/off transition, exposed for the UI's "show me the
 * cadence" diagnostic. Same fields the internal matcher uses. */
typedef struct {
  uint8_t  is_on;        /* 1 = entered ON state, 0 = entered OFF */
  uint8_t  reserved[3];
  uint32_t age_ms;       /* how long ago (ms) the transition happened */
  uint32_t dur_ms;       /* duration of the PREVIOUS state, in ms */
} audio_transition_t;

typedef void (*audio_event_cb_t)(const audio_event_t* evt);

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

/* Initialize the I2S-PDM driver and the cadence detector. Idempotent. */
bool audio_init(const audio_config_t* config);

/* Tear down the I2S channel and scrub all per-frame state. */
void audio_deinit(void);

/* Enable I2S RX. After this call, audio_process() begins draining samples. */
bool audio_start(void);

/* Disable I2S RX. Scrubs the envelope ring. */
void audio_stop(void);

/* True if the PDM driver is currently running. */
bool audio_is_running(void);

/* ──────────────────────────────────────────────────────────────────────────
 * RUNTIME MUTE (user-controlled, persisted by the caller)
 *
 * audio_mute(true) physically uninstalls the I2S driver and releases the
 * mic pins, so this is a hard mute the user can verify (the GPIOs are
 * tri-stated until unmute). audio_mute(false) re-runs audio_start().
 * audio_is_muted() reports the last-requested state regardless of
 * whether the I2S driver could be re-opened.
 *
 * `source` is recorded so we can sign an audit-trail event into the
 * witness chain — a tamperer flipping the mic off from HA leaves a
 * different trail than the user clicking the dashboard button.
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
  AUDIO_MUTE_SOURCE_BOOT = 0,   /* applied at startup from persisted NVS */
  AUDIO_MUTE_SOURCE_HTTP = 1,   /* user clicked the dashboard mute toggle */
  AUDIO_MUTE_SOURCE_MQTT = 2,   /* command from Home Assistant via MQTT */
} audio_mute_source_t;

bool audio_mute(bool muted, uint8_t source);
bool audio_is_muted(void);

/* Synchronous mute used by the boot path BEFORE the HTTP server starts,
 * when there is provably only one task in play. Do NOT call this from
 * any other context — use audio_mute() instead, which defers the I2S
 * teardown to the main loop to avoid racing audio_process(). */
bool audio_mute_sync_at_boot(bool muted);

/* Fired synchronously from audio_process() when a deferred mute toggle
 * is actually applied. Always called from the main task, so the callback
 * is free to call into other modules (sensing, witness, MQTT publish).
 * Pass nullptr to unregister. */
typedef void (*audio_mute_cb_t)(bool muted, uint8_t source);
void audio_set_mute_callback(audio_mute_cb_t cb);

/* Persist the user's mute intent to NVS. Used by every control path
 * (HTTP, MQTT, future ones) so the NVS namespace / key stay in one
 * place. Honored at the next boot by main.cpp's audio_init block. */
bool audio_save_mute_intent(bool muted);

/* Info about the most recent applied mute toggle. `source` is one of
 * audio_mute_source_t; `age_ms` is millis() since the toggle was applied,
 * or UINT32_MAX if no toggle has been applied this boot. Lets the
 * dashboard surface "Muted by Home Assistant · 2 min ago" so the user
 * can tell who flipped the mic. */
typedef struct {
  uint8_t  source;     /* audio_mute_source_t */
  uint8_t  reserved[3];
  uint32_t age_ms;     /* since last applied toggle, UINT32_MAX if never */
} audio_mute_info_t;

void audio_get_mute_info(audio_mute_info_t* out);

/* ──────────────────────────────────────────────────────────────────────────
 * SELF-TEST MODE
 *
 * When active, the existing T3/T4 matchers run with relaxed timing
 * tolerance and a lower confidence floor so a user holding their alarm's
 * TEST button at ~3 m has the best chance of being heard. Crucially, the
 * normal event callback is NOT fired while self-test is active — we don't
 * want a test press to flow into Home Assistant smoke automations.
 * Auto-expires after `duration_ms` (max 60_000).
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t  active;          /* 1 = self-test running */
  uint8_t  matched_type;    /* audio_event_type_t once matched, else 0 */
  uint8_t  matched_conf;    /* 0..100 once matched */
  uint8_t  reserved;
  uint32_t remaining_ms;    /* time until auto-expiry */
  uint32_t transitions_seen;/* on/off transitions observed during the test */
} audio_selftest_status_t;

bool audio_selftest_start(uint32_t duration_ms);
void audio_selftest_stop(void);
bool audio_selftest_status(audio_selftest_status_t* out);

/* ──────────────────────────────────────────────────────────────────────────
 * DATA FLOW
 * ────────────────────────────────────────────────────────────────────────── */

/* Register the event callback. Fires synchronously from audio_process()
 * each time a complete cadence cycle (T3 or T4) is matched, capped at
 * one event per 250 ms to avoid storming downstream consumers. Pass
 * nullptr to unregister. */
void audio_set_event_callback(audio_event_cb_t cb);

/* Pump the audio pipeline. Call from the main loop at >= 50 Hz so the
 * I2S DMA ring doesn't back up. Each call drains up to 4 frames (80 ms
 * of audio) per call, computes RMS, runs the envelope hysteresis +
 * transition tracker, and matches against T3/T4 cadence templates.
 * Returns the number of frames processed this call. */
int audio_process(void);

/* ──────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ────────────────────────────────────────────────────────────────────────── */

bool audio_get_stats(audio_stats_t* out);

/* Snapshot of the active runtime configuration. Safe to call from any
 * task — s_cfg is written only by audio_init() at boot. */
bool audio_get_config(audio_config_t* out);

/* Most recent 20 ms RMS scalar (0..65535) the pipeline computed. This is
 * the SAME number the on/off hysteresis uses — exposing it lets the UI
 * draw a level meter without adding a second audio path. Returns 0 when
 * muted or never run. `age_ms_out` reports how long ago the value was
 * produced; the UI can grey out the meter if it's stale (> ~200 ms). */
bool audio_get_live_level(uint16_t* rms_out, uint32_t* age_ms_out);

/* Copy up to `max` most-recent transitions (newest first) into `out`,
 * with their age relative to `now_ms_or_zero` (pass 0 to use millis()).
 * Returns the number of transitions actually written. */
size_t audio_get_recent_transitions(audio_transition_t* out, size_t max,
                                    uint32_t now_ms_or_zero);

/* Plain-English label for an audio_event_type_t. */
const char* audio_event_name(uint8_t event_type);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_AUDIO_H */
