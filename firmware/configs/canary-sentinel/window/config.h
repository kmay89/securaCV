/**
 * @file config.h
 * @brief Canary Sentinel — "window" preset (Standard tier).
 *
 * Window / sill guardian. Expressed as DELTAS on the `door` baseline so the
 * differences are the whole story. A window is a smaller, closer zone where the
 * telling signals are a hand or face at the glass and a shadow across the sill,
 * so: range tightened, ambient-light weighted up, and the anomaly threshold
 * lowered so a blinded light sensor (someone covering the glass sensor) trips
 * sooner. No contact/tamper hardware at Standard tier.
 */

#pragma once

#include "../door/config.h"

// Identity
#undef  SENT_DEVICE_ID
#define SENT_DEVICE_ID   "canary_sentinel_win_001"
#undef  SENT_MODEL
#define SENT_MODEL       "Canary Sentinel Standard — Window (XIAO ESP32-C6 + MR60BHA2)"

// Tighter zone: a window is a near field.
#undef  SENT_RANGE_NEAR_CM
#define SENT_RANGE_NEAR_CM 80
#undef  SENT_RANGE_MID_CM
#define SENT_RANGE_MID_CM  200

// Light matters more at glass (shadow on the sill, a hand over the sensor).
#undef  SENT_W_LIGHT
#define SENT_W_LIGHT       40

// Blinding the light sensor at a window is a stronger tell -> quicker anomaly.
#undef  SENT_ANOMALY_SCORE
#define SENT_ANOMALY_SCORE 45

// A window approach is deliberate; give it a hair more debounce than a door.
#undef  SENT_PRESENT_DEBOUNCE_MS
#define SENT_PRESENT_DEBOUNCE_MS 1300
