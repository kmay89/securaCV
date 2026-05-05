/*
 * SecuraCV Canary — IR remote-control activity detection
 *
 * Wraps the ESP32-S3's RMT (Remote Control Transceiver) peripheral in
 * RX mode to detect activity from household IR remotes — TV, AC,
 * set-top-box, lights — without ever storing the actual IR code.
 *
 * The reasoning: if the canary in a room sees IR activity, the
 * household is "active" in a coarse sense. That's a useful baseline
 * signal for "abnormal silence" detection in elder-care or vacant-
 * property scenarios. But the IR code itself reveals which channel was
 * tuned, what AC temperature was set, what light was dimmed — that's
 * over the line into surveillance territory.
 *
 * PRIVACY PIPELINE:
 *
 *   raw 38 kHz pulses ──▶ RMT decoder ──▶ {protocol, payload_bits}
 *                                                 │
 *                                                 ▼
 *                  HMAC-SHA256(per-session salt, protocol||payload)
 *                                                 │
 *                                                 ▼  bottom 4 bits
 *                                       appliance_ir_activity event
 *                                       { category, hash_bucket(0..15),
 *                                         time_bucket, confidence }
 *
 *   • Per-session salt rotates on every boot (32 bytes from
 *     esp_random()), so the same TV remote produces a different
 *     bucket across reboots — cross-session correlation is
 *     structurally impossible.
 *   • 4-bit bucket gives 16 distinguishable signatures per session,
 *     enough to say "remote A pressed twice within 5 s" without
 *     leaking which actual key it was.
 *   • The category enum tells the UI whether the leader pattern
 *     matched NEC / RC5 / Sony / Unknown — a household-vendor
 *     signal that's already public information from the protocol
 *     family.
 *
 * The only outputs that cross this module are:
 *   { event_type, category, hash_bucket, time_bucket, confidence }
 *
 * Default RX pin: GPIO 3 (D2 on the XIAO header). Override at
 * compile time with -DIR_RX_PIN_NUM=N.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_IR_H
#define SECURACV_IR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef IR_RX_PIN_NUM
  #define IR_RX_PIN_NUM   3   /* D2 on the XIAO header — free; not SD/cam/GPS/touch */
#endif

typedef enum {
  IR_PROTOCOL_UNKNOWN = 0,
  IR_PROTOCOL_NEC     = 1,   /* 9 ms / 4.5 ms leader (TV, set-top, AC) */
  IR_PROTOCOL_RC5     = 2,   /* 1.78 ms half-bit Manchester (Philips) */
  IR_PROTOCOL_SONY    = 3,   /* 2.4 ms / 0.6 ms leader, 12/15/20 bit */
} ir_protocol_t;

typedef struct {
  uint8_t  event_type;     /* always 1 = appliance_ir_activity */
  uint8_t  category;       /* ir_protocol_t */
  uint8_t  hash_bucket;    /* 0..15, HMAC-bucketed payload */
  uint8_t  confidence;     /* 0..100 — how clean the decode was */
  uint8_t  time_bucket;    /* 10-min daily bucket (0..143) */
} ir_event_t;

typedef struct {
  int gpio_num;            /* default IR_RX_PIN_NUM */
  /* Idle threshold (µs) — silence longer than this ends a frame.
   * Default 9.5 ms covers NEC's 9 ms leader without splitting it. */
  uint16_t idle_threshold_us;
  /* Filter threshold (µs) — pulses shorter than this are dropped.
   * Default 100 µs filters typical sub-µs RX noise without cutting
   * into Sony's 600 µs short pulse. */
  uint16_t filter_threshold_us;
} ir_config_t;

#define IR_CONFIG_DEFAULT { \
    /*.gpio_num*/             IR_RX_PIN_NUM, \
    /*.idle_threshold_us*/    9500,          \
    /*.filter_threshold_us*/  100            \
}

typedef struct {
  uint32_t frames_received;     /* RMT items received (raw) */
  uint32_t frames_decoded;      /* successfully matched a known protocol */
  uint32_t frames_unknown;      /* couldn't identify */
  uint32_t events_emitted;
} ir_stats_t;

typedef void (*ir_event_cb_t)(const ir_event_t* evt);

#ifdef __cplusplus
extern "C" {
#endif

bool ir_init(const ir_config_t* config);
void ir_deinit(void);
bool ir_start(void);
void ir_stop(void);
bool ir_is_running(void);

void ir_set_event_callback(ir_event_cb_t cb);

/* Pump from the main loop. Drains the RMT ring buffer non-blocking,
 * decodes any complete frames, and fires the callback synchronously
 * on a confirmed event. Returns the number of frames processed this
 * call. Storm-capped at 1 event / 250 ms internally. */
int ir_process(void);

bool ir_get_stats(ir_stats_t* out);
const char* ir_protocol_name(uint8_t cat);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_IR_H */
