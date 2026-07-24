/**
 * @file config.h
 * @brief Canary Sentinel — "hallway" preset (Standard tier).
 *
 * Interior corridor / room occupancy. DELTAS on `door`. This is a presence
 * preset, not an alarm preset: gentler debounce, a wider radar range, a longer
 * loiter window (people dwell indoors legitimately), and a higher anomaly
 * threshold so ordinary indoor life doesn't read as suspicious.
 */

#pragma once

#include "../door/config.h"

#undef  SENT_DEVICE_ID
#define SENT_DEVICE_ID   "canary_sentinel_hall_001"
#undef  SENT_MODEL
#define SENT_MODEL       "Canary Sentinel Standard — Hallway (XIAO ESP32-C6 + MR60BHA2)"

// Cover the length of a corridor.
#undef  SENT_RANGE_NEAR_CM
#define SENT_RANGE_NEAR_CM 250
#undef  SENT_RANGE_MID_CM
#define SENT_RANGE_MID_CM  500

// Occupancy, not alarm: relax the reflexes.
#undef  SENT_PRESENT_DEBOUNCE_MS
#define SENT_PRESENT_DEBOUNCE_MS 1500
#undef  SENT_LOITER_DWELL_MS
#define SENT_LOITER_DWELL_MS     120000   // 2 min before "loiter" indoors
#undef  SENT_ANOMALY_SCORE
#define SENT_ANOMALY_SCORE 65

// Indoors, a still device-free body is normal (someone reading on a couch);
// don't treat an uncorroborated dwelling body as an anomaly here.
#undef  SENT_SILENT_BODY_ANOMALY
#define SENT_SILENT_BODY_ANOMALY 0
