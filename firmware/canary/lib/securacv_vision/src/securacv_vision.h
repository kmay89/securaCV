/*
 * SecuraCV Canary — 3-Layer Cascaded Vision Detection
 *
 * Layer 1: JPEG size delta (near-zero cost)
 * Layer 2: Block luminance motion detection (~10-30ms)
 * Layer 3: TFLite person classifier (~500ms, opt-in)
 *
 * PRIVACY: No raw frames leave this module. Output is semantic events
 * only: {event_type, confidence, zone, time_bucket}. No bounding boxes,
 * no identity, no stored frames.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_VISION_H
#define SECURACV_VISION_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  VISION_EVENT_NONE       = 0,
  VISION_EVENT_MOTION     = 1,
  VISION_EVENT_MOTION_END = 2,
  VISION_EVENT_PERSON     = 3,
} vision_event_type_t;

typedef struct {
  uint8_t event_type;
  uint8_t confidence;
  uint8_t zone;
  uint8_t time_bucket;
} vision_event_t;

typedef struct {
  uint8_t  jpeg_delta_pct;
  uint8_t  block_change_pct;
  uint8_t  person_confidence_min;
  uint8_t  luminance_threshold;
  uint16_t process_interval_ms;
  uint16_t motion_hold_ms;
  uint32_t layer3_cooldown_ms;
  uint16_t sustained_backoff_ms;
  uint8_t  sustained_threshold;
  uint16_t thermal_rest_ms;
  uint8_t  duty_cycle_pct;
} vision_config_t;

#define VISION_CONFIG_DEFAULT { \
  /*.jpeg_delta_pct*/         15,    \
  /*.block_change_pct*/       20,    \
  /*.person_confidence_min*/  60,    \
  /*.luminance_threshold*/    20,    \
  /*.process_interval_ms*/    200,   \
  /*.motion_hold_ms*/         3000,  \
  /*.layer3_cooldown_ms*/     5000,  \
  /*.sustained_backoff_ms*/   1000,  \
  /*.sustained_threshold*/    10,    \
  /*.thermal_rest_ms*/        5000,  \
  /*.duty_cycle_pct*/         50     \
}

#define VISION_GRID_COLS  10
#define VISION_GRID_ROWS   8
#define VISION_GRID_TOTAL (VISION_GRID_COLS * VISION_GRID_ROWS)

typedef struct {
  uint32_t frames_analyzed;
  uint32_t layer1_passes;
  uint32_t layer2_passes;
  uint32_t layer3_passes;
  uint32_t motion_events;
  uint32_t person_events;
  bool     motion_active;
  uint8_t  last_zone;
  uint8_t  last_confidence;
} vision_stats_t;

typedef void (*vision_event_cb_t)(const vision_event_t* evt);

#ifdef __cplusplus
extern "C" {
#endif

bool vision_init(const vision_config_t* cfg);
void vision_deinit(void);
bool vision_start(void);
void vision_stop(void);
bool vision_is_running(void);

void vision_set_event_callback(vision_event_cb_t cb);

bool vision_process(void);

bool vision_get_stats(vision_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_VISION_H */
