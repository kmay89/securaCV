/**
 * @file config.h
 * @brief Canary Sentinel — "mailbox-lite" preset (LITE tier).
 *
 * DELTAS on `door`, but this one changes the TIER: it turns OFF the two
 * body-present modalities (radar, CSI) because the Lite board is a single
 * XIAO ESP32-C3/C6 with no radar module. What remains is PIR + WiFi-RF + BLE +
 * ambient light — three modality classes (Thermal, CarriedRadio, Optical).
 *
 * Honesty is the feature: with no radar and no CSI, a slow, device-free,
 * still intruder CAN evade this tier. It is right for a mailbox, a shed, a
 * porch, an interior hallway — places where the threat is casual, not a
 * determined adversary. The confirmed threshold is lowered because the highest-
 * weight channels are gone; without them, two of the remaining three agreeing
 * is the best corroboration available.
 */

#pragma once

#include "../door/config.h"

#undef  SENT_DEVICE_ID
#define SENT_DEVICE_ID   "canary_sentinel_lite_001"
#undef  SENT_MODEL
#define SENT_MODEL       "Canary Sentinel Lite (XIAO ESP32-C3)"
#undef  SENT_TIER
#define SENT_TIER        "lite"

// Lite has no radar module and no CSI-capable pipeline wired.
#undef  FEATURE_MMWAVE_RADAR
#define FEATURE_MMWAVE_RADAR 0
#undef  FEATURE_WIFI_CSI
#define FEATURE_WIFI_CSI     0

// Lean harder on the channels that remain.
#undef  SENT_W_PIR
#define SENT_W_PIR         65
#undef  SENT_W_RF
#define SENT_W_RF          45
#undef  SENT_W_BLE
#define SENT_W_BLE         45
#undef  SENT_W_LIGHT
#define SENT_W_LIGHT       35

// With the heavy-hitters gone, lower the confirm bar to what 2 of 3 remaining
// independent classes can actually reach.
#undef  SENT_CONFIRMED_SCORE
#define SENT_CONFIRMED_SCORE 60

// A single blinded sensor on a 3-channel unit is a bigger fraction of the
// system — keep the anomaly bar where a body-plus-blind still trips it, but
// don't over-alarm a lone fault.
#undef  SENT_ANOMALY_SCORE
#define SENT_ANOMALY_SCORE 55
