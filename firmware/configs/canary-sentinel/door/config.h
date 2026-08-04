/**
 * @file config.h
 * @brief Canary Sentinel — "door" preset (Standard tier).
 *
 * Front-door / entry-threshold guardian. Balanced weights across all five
 * Standard-tier modalities, a fast present-debounce so a real approach commits
 * quickly, and a loiter timer so someone who lingers at the door is surfaced.
 *
 * Everything in this file is DATA. Behavior never forks by preset — only these
 * numbers do (firmware/ARCHITECTURE.md config rules). The project's
 * sentinel_config.h maps these macros onto a securacv::fusion::FusionConfig.
 *
 * The SENT_W_* / SENT_EV_* / SENT_STALE_* triples are the per-channel
 * ChannelSpec (evidence weight 0..100, evasion cost 0..100, staleness ms).
 * SENT_EV_* (evasion cost) is descriptive metadata surfaced in diagnostics —
 * how hard that modality is to defeat — it does not change scoring.
 */

#pragma once

// ── Identity ─────────────────────────────────────────────────────────────────
#define SENT_DEVICE_TYPE     "canary-sentinel"
#define SENT_DEVICE_ID       "canary_sentinel_001"
#define SENT_MANUFACTURER    "SecuraCV"
#define SENT_MODEL           "Canary Sentinel Standard (XIAO ESP32-C6 + MR60BHA2)"
#define SENT_TIER            "standard"

// ── Channel enables (compile-time modularity) ────────────────────────────────
#define FEATURE_PIR            1   // passive infrared motion
#define FEATURE_MMWAVE_RADAR   1   // 60GHz radar (still body + breathing)
#define FEATURE_WIFI_CSI       1   // device-free WiFi channel-perturbation
#define FEATURE_WIFI_RF        1   // aggregate WiFi device count (no MAC)
#define FEATURE_BLE            1   // aggregate BLE device count (no MAC)
#define FEATURE_AMBIENT_LIGHT  1   // BH1750 corroboration / blinding
#define FEATURE_CONTACT        0   // reed contact (Heavy)
#define FEATURE_VISION         0   // optical corroboration (Heavy hub)
#define FEATURE_TAMPER         0   // enclosure tamper (Heavy)

// ── Platform features ────────────────────────────────────────────────────────
#define FEATURE_WIFI_STA       1
#define FEATURE_MQTT           1
#define FEATURE_HA_DISCOVERY   1
#define FEATURE_STATUS_LED     1
#define FEATURE_WATCHDOG       1

// ── Per-channel evidence (weight, evasion_cost, stale_ms) ────────────────────
//                         weight  evasion  stale_ms
#define SENT_W_PIR         55
#define SENT_EV_PIR        25
#define SENT_STALE_PIR     2500
#define SENT_W_RADAR       80
#define SENT_EV_RADAR      80
#define SENT_STALE_RADAR   2000
#define SENT_W_CSI         60
#define SENT_EV_CSI        70
#define SENT_STALE_CSI     4000
#define SENT_W_RF          35
#define SENT_EV_RF         30
#define SENT_STALE_RF      8000
#define SENT_W_BLE         30
#define SENT_EV_BLE        30
#define SENT_STALE_BLE     8000
#define SENT_W_LIGHT       25
#define SENT_EV_LIGHT      20
#define SENT_STALE_LIGHT   3000
#define SENT_W_CONTACT     70
#define SENT_EV_CONTACT    60
#define SENT_STALE_CONTACT 2000
#define SENT_W_VISION      75
#define SENT_EV_VISION     65
#define SENT_STALE_VISION  2000
#define SENT_W_TAMPER      90
#define SENT_EV_TAMPER     85
#define SENT_STALE_TAMPER  2000

// ── Fusion thresholds (on the 0..100 fused score) ────────────────────────────
#define SENT_PRESENT_SCORE      45
#define SENT_CONFIRMED_SCORE    70
#define SENT_CLEAR_SCORE        25
#define SENT_INDEP_BONUS        18   // points per corroborating modality >= 2
#define SENT_MIN_CONFIRM        2    // independent classes required to Confirm
#define SENT_DENIED_SUSPICION   34   // anomaly points per blinded channel
#define SENT_ANOMALY_SCORE      55   // anomaly accumulator >= -> Anomaly
#define SENT_SILENT_BODY_ANOMALY 1   // uncorroborated dwelling body -> Anomaly

// ── FSM timing (ms) ──────────────────────────────────────────────────────────
#define SENT_PRESENT_DEBOUNCE_MS  1000   // fast commit at a door
#define SENT_CLEAR_DEBOUNCE_MS    8000
#define SENT_LOITER_DWELL_MS      30000  // linger at the door -> Loiter
#define SENT_ANOMALY_LATCH_MS     15000

// ── Coarse range bands (raw cm consumed on-device, never exported) ───────────
#define SENT_RANGE_NEAR_CM     150
#define SENT_RANGE_MID_CM      350

// ── Housekeeping ─────────────────────────────────────────────────────────────
#define SENT_HEARTBEAT_MS         5000
#define SENT_WATCHDOG_TIMEOUT_SEC 30
