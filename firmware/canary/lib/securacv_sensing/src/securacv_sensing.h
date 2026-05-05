/*
 * SecuraCV Canary — Sensing State Aggregator
 *
 * Distills the privacy-safe 32-byte CSI feature vector into a tiny
 * human-readable snapshot for the web UI and HTTP API:
 *
 *   - motion_score    0..100   from amplitude variance + Doppler bands
 *   - breathing_score 0..100   from 0.1–0.5 Hz Goertzel bins
 *   - activity_label  enum     "quiet" / "presence" / "motion" / "active"
 *   - rssi_dbm        int8     window mean
 *   - frames_in_win   uint16   from the last finalized window
 *   - last_window_ms  uint32   millis() at last update
 *
 * The aggregator is the ONLY consumer of the CSI feature callback. It
 * exposes a fixed-size snapshot suitable for embedding in a JSON status
 * response — no raw subcarrier samples, no per-frame timestamps, no
 * device identifiers cross this boundary (those never reach us; the
 * CSI HAL has already scrubbed them).
 *
 * Decay: if no new feature vector arrives for SENSING_TTL_MS (default
 * 5 s), all scores fade linearly toward zero so a stale UI cannot
 * misread "no signal" as "no presence."
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SENSING_H
#define SECURACV_SENSING_H

#include <stdint.h>
#include <stdbool.h>

#include "securacv_csi.h"  /* csi_features_t */

#ifdef __cplusplus
extern "C" {
#endif

#define SENSING_TTL_MS  5000

typedef enum {
  SENSING_LABEL_OFFLINE   = 0,  /* no recent feature vector at all */
  SENSING_LABEL_QUIET     = 1,  /* ambient — empty room or stable furniture */
  SENSING_LABEL_PRESENCE  = 2,  /* steady micro-motion (occupancy / breathing) */
  SENSING_LABEL_MOTION    = 3,  /* clear room-scale motion */
  SENSING_LABEL_ACTIVE    = 4,  /* sustained movement */
} sensing_label_t;

typedef struct {
  uint8_t  motion_score;       /* 0..100, 0 = quiet, 100 = saturated */
  uint8_t  breathing_score;    /* 0..100 */
  uint8_t  activity_label;     /* sensing_label_t */
  int8_t   rssi_dbm;           /* window mean RSSI */
  int8_t   rssi_std;           /* stddev across the window */
  uint16_t frames_in_window;   /* last finalized window's frame count */
  uint16_t dropped_estimate;   /* per-window expected − actual */
  uint8_t  channel;
  uint8_t  bandwidth_code;     /* 0 = HT20, 1 = HT40 */
  uint8_t  time_bucket;        /* 10-min daily bucket (0..143) */
  uint32_t last_window_ms;     /* millis() at last finalize */
  uint32_t windows_seen;       /* total feature windows processed */
  /* Eight-band amplitude variance, 0..127 — useful for a visual bar
   * graph; copied straight from feature vector slots [0..7]. */
  int8_t   amp_bands[8];
  /* Four-band sign-aware Doppler — same scale, useful for direction
   * arrows in the UI; from feature vector slots [8..11]. */
  int8_t   doppler[4];
  /* Eight-bin breathing spectrum (0.1–0.5 Hz Goertzel) from slots [12..19]. */
  int8_t   breathing_bins[8];

  /* ── Acoustic events (Phase 2) ──────────────────────────────────
   * Last cadence-detector event. All values are zero / "none" until
   * sensing_feed_audio() is called; they then persist for ACOUSTIC_TTL_MS
   * (defined in the .cpp) so a UI poll catches a transient T3/T4 event
   * even if it fired between polls. */
  uint8_t  last_audio_event_type;   /* audio_event_type_t (0..2) */
  uint8_t  last_audio_event_conf;   /* 0..100 */
  uint16_t last_audio_event_count;  /* cycles matched in this run */
  uint32_t last_audio_event_ms;     /* millis() at the event */

  /* ── Touch events (Phase 3) ────────────────────────────────────
   * Last touch-pad event (panic / tamper / approach). Zeroed once
   * TOUCH_TTL_MS has elapsed since the event fired. */
  uint8_t  last_touch_event_type;   /* touch_event_type_t (0..3) */
  uint8_t  last_touch_event_conf;   /* 0..100 */
  uint8_t  last_touch_pad_channel;  /* 1..14 on S3 */
  uint32_t last_touch_event_ms;     /* millis() at the event */

  /* ── IR appliance events (Phase 4) ─────────────────────────────
   * Last IR remote-control activity. The hash_bucket is a 4-bit
   * per-session-salted HMAC of the protocol+payload — stable within
   * a session ("button A vs button B"), reset across reboots. */
  uint8_t  last_ir_category;        /* ir_protocol_t (0=unknown..3=sony) */
  uint8_t  last_ir_hash_bucket;     /* 0..15 */
  uint8_t  last_ir_confidence;      /* 0..100 */
  uint32_t last_ir_event_ms;

  /* ── Temperature-drift tamper events (Phase 4) ─────────────────
   * Last tamper_temp_drift event. */
  uint8_t  last_temp_drift_conf;    /* 0..100 */
  uint32_t last_temp_drift_ms;
} sensing_state_t;

/* Initialize the aggregator. Idempotent. */
void sensing_init(void);

/* Feed a finalized CSI window. Safe to call from the CSI callback
 * (synchronous, allocation-free). */
void sensing_feed_csi(const csi_features_t* features);

/* Feed an acoustic event (T3 smoke / T4 CO from securacv_audio).
 * Decoupled from securacv_audio.h to keep the sensing lib a clean
 * leaf module — caller passes the scalar fields directly. The
 * time_bucket should be the audio module's own bucket (0..143) so
 * the snapshot's bucket field stays consistent regardless of which
 * sensor produced the most recent update. */
void sensing_feed_audio_event(uint8_t event_type,
                              uint8_t confidence,
                              uint16_t cycle_count,
                              uint8_t time_bucket);

/* Feed a touch event (panic / tamper / approach from securacv_touch).
 * Same decoupling rationale as sensing_feed_audio_event. */
void sensing_feed_touch_event(uint8_t event_type,
                              uint8_t confidence,
                              uint8_t pad_channel,
                              uint8_t time_bucket);

/* Feed an IR appliance-activity event (NEC / RC5 / Sony from
 * securacv_ir). The hash_bucket is the 4-bit privacy-bucketed
 * HMAC of the IR payload; we don't get to see the raw payload. */
void sensing_feed_ir_event(uint8_t category,
                           uint8_t hash_bucket,
                           uint8_t confidence,
                           uint8_t time_bucket);

/* Feed a temperature-drift tamper event (from securacv_envsens). */
void sensing_feed_temp_drift_event(uint8_t confidence,
                                   uint8_t time_bucket);

/* Apply TTL decay; call from the main loop at 1–10 Hz. */
void sensing_tick(void);

/* Snapshot the current aggregated state into `out`. Always succeeds. */
void sensing_snapshot(sensing_state_t* out);

/* Plain-English label for a sensing_label_t value. */
const char* sensing_label_name(uint8_t label);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_SENSING_H */
