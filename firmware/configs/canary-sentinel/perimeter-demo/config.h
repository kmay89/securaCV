/**
 * @file config.h
 * @brief Canary Sentinel — "perimeter-demo" preset (HEAVY tier, the rigged demo).
 *
 * DELTAS on `door` that turn EVERYTHING on: the Standard five modalities plus
 * door-contact, enclosure tamper, and the optical vision vote from the Heavy
 * hub — all six physical modality classes live at once. Sensitivities are
 * pushed and the reflexes are fast. This is the "near impossible to evade"
 * configuration we demo: to cross this threshold unseen you must simultaneously
 * emit no heat, reflect no radar, not perturb the WiFi, carry no powered radio,
 * cast no optical change, and touch nothing — and any attempt to blind a
 * channel is itself an alarm.
 *
 * Heavy is dual-board (see the project README): the sensor head runs the local
 * channels; the vision vote arrives from a second XIAO ESP32-S3 hub over
 * ESP-NOW. FEATURE_VISION here means "accept a vision vote from the hub".
 */

#pragma once

#include "../door/config.h"

#undef  SENT_DEVICE_ID
#define SENT_DEVICE_ID   "canary_sentinel_demo_001"
#undef  SENT_MODEL
#define SENT_MODEL       "Canary Sentinel Heavy — Perimeter Demo (C6 head + S3 vision hub)"
#undef  SENT_TIER
#define SENT_TIER        "heavy"

// Light up the Heavy-only channels.
#undef  FEATURE_CONTACT
#define FEATURE_CONTACT  1
#undef  FEATURE_VISION
#define FEATURE_VISION   1
#undef  FEATURE_TAMPER
#define FEATURE_TAMPER   1
#undef  FEATURE_MESH_NETWORK
#define FEATURE_MESH_NETWORK 1   // ESP-NOW head<->hub link for the vision vote

// Push sensitivity: commit fast, confirm on the strong independent stack.
#undef  SENT_PRESENT_DEBOUNCE_MS
#define SENT_PRESENT_DEBOUNCE_MS 700
#undef  SENT_PRESENT_SCORE
#define SENT_PRESENT_SCORE 40
#undef  SENT_CONFIRMED_SCORE
#define SENT_CONFIRMED_SCORE 65

// With six independent classes available, a blinded channel is a very strong
// tell — lower the anomaly bar so tamper/blinding surfaces immediately in the
// demo.
#undef  SENT_ANOMALY_SCORE
#define SENT_ANOMALY_SCORE 40
#undef  SENT_LOITER_DWELL_MS
#define SENT_LOITER_DWELL_MS 15000
