/*
 * SecuraCV Canary — RF Presence Detection
 * Version 0.1.0
 *
 * Privacy-preserving RF-based presence detection using BLE and WiFi signals.
 * Implements spec/canary_free_signals_v0.md
 *
 * Core Invariants (enforced in code):
 * - NO persistent MAC address storage
 * - NO device fingerprinting or vendor inference
 * - NO cross-session correlation
 * - Ephemeral session tokens rotate every SESSION_ROTATE_MS
 * - Only aggregated, anonymized observations persist
 *
 * This module observes the RF environment to detect presence without
 * identifying who or what is present. It is a witness, not an analyst.
 */

#ifndef SECURACV_RF_PRESENCE_H
#define SECURACV_RF_PRESENCE_H

#include <Arduino.h>

namespace rf_presence {

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

// Privacy configuration
static const uint32_t SESSION_ROTATE_MS = 4 * 60 * 60 * 1000;  // 4 hours
static const uint32_t OBSERVATION_TTL_MS = 60 * 1000;          // 60 seconds
static const size_t   OBSERVATION_BUFFER_SIZE = 64;
static const size_t   SESSION_TOKEN_MAP_SIZE = 32;

// FSM timing thresholds (milliseconds)
static const uint32_t IMPULSE_TIMEOUT_MS = 5000;       // Max impulse duration
static const uint32_t PRESENCE_THRESHOLD_MS = 10000;   // Confirm presence
static const uint32_t DWELL_THRESHOLD_MS = 60000;      // Confirm dwelling
static const uint32_t LOST_TIMEOUT_MS = 30000;         // Declare empty
static const uint32_t DEPARTING_CONFIRM_MS = 15000;    // Confirm departure

// Signal processing
static const int8_t  RSSI_NOISE_FLOOR = -90;           // Below this = noise
static const uint8_t MIN_PRESENCE_COUNT = 1;           // Min devices for presence
static const uint8_t PROBE_BURST_THRESHOLD = 3;        // Probes/sec for burst

// ════════════════════════════════════════════════════════════════════════════
// ENUMS
// ════════════════════════════════════════════════════════════════════════════

// RF Presence FSM states
enum RfState : uint8_t {
  RF_EMPTY = 0,       // No RF presence detected
  RF_IMPULSE,         // Brief signal, awaiting confirmation
  RF_PRESENCE,        // Confirmed RF activity
  RF_DWELLING,        // Stable, sustained presence
  RF_DEPARTING        // Signals weakening, count dropping
};

// Signal source type
enum SignalSource : uint8_t {
  SIG_NONE = 0,
  SIG_BLE,
  SIG_WIFI,
  SIG_FUSED           // Multiple signals combined
};

// Confidence class
enum ConfidenceClass : uint8_t {
  CONF_UNCERTAIN = 0, // < 0.2
  CONF_LOW,           // 0.2 - 0.5
  CONF_MODERATE,      // 0.5 - 0.8
  CONF_HIGH           // >= 0.8
};

// Dwell classification
enum DwellClass : uint8_t {
  DWELL_TRANSIENT = 0,  // < 30 seconds
  DWELL_LINGERING,      // 30-120 seconds
  DWELL_SUSTAINED       // > 120 seconds
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES — INTERNAL (never exported)
// ════════════════════════════════════════════════════════════════════════════

// Session token for deduplication (ephemeral, rotates)
// INVARIANT: Cannot be correlated across sessions
// INVARIANT: Cannot be reversed to MAC address
struct SessionToken {
  uint32_t token;           // Derived from MAC + session epoch
  uint32_t last_seen_ms;    // When last observed
  int8_t   rssi;            // Most recent RSSI
  // NO: mac_address, device_name, oui, vendor
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES — OBSERVATION (privacy-safe aggregates)
// ════════════════════════════════════════════════════════════════════════════

// RF Observation (what gets stored in ring buffer)
// Contains ONLY aggregated, anonymized data
struct RfObservation {
  uint32_t timestamp_ms;      // Internal only, not exported

  // BLE aggregates (no identifiers)
  uint8_t  ble_device_count;  // Unique session tokens in window
  int8_t   ble_rssi_max;      // Strongest signal
  int8_t   ble_rssi_mean;     // Average signal
  int8_t   ble_rssi_min;      // Weakest signal
  uint8_t  ble_adv_density;   // Advertisements per second (0-255)

  // WiFi aggregates (no identifiers)
  uint8_t  wifi_probe_count;  // Probe bursts detected
  int8_t   wifi_rssi_peak;    // Peak probe RSSI

  // Environmental context
  int8_t   temp_delta_c;      // Temperature change (x10 for 0.1C resolution)
  uint8_t  power_flags;       // Brownout/voltage events
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES — EVENTS (exported vocabulary)
// ════════════════════════════════════════════════════════════════════════════

// RF Presence Event (what gets emitted)
// Vocabulary is strictly controlled per spec
struct RfEvent {
  const char*     event_name;     // From allowed vocabulary
  SignalSource    signal;         // Primary signal source
  ConfidenceClass confidence;     // Confidence classification
  int8_t          count_delta;    // Change in device count
  DwellClass      dwell_class;    // Dwell classification (if applicable)
  uint8_t         time_bucket;    // Coarse time (per PWK invariant)
  const char*     narrative_hint; // Optional hedge phrase (nullable)

  // NO: mac_address, device_name, precise_timestamp, vendor
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES — STATE SNAPSHOT
// ════════════════════════════════════════════════════════════════════════════

// Current RF presence state (for status API)
struct RfStateSnapshot {
  RfState         state;          // Current FSM state
  ConfidenceClass confidence;     // Current confidence
  uint8_t         device_count;   // Current anonymous count
  int8_t          rssi_mean;      // Current average RSSI
  uint32_t        state_duration_ms; // Time in current state
  DwellClass      dwell_class;    // Current dwell classification
  const char*     state_name;     // Human-readable state
  uint32_t        uptime_s;       // Device uptime
  const char*     last_event;     // Most recent event name
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES — CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

// RF Presence settings (persisted to NVS)
struct RfPresenceSettings {
  bool     enabled;                   // Feature enabled
  uint32_t presence_threshold_ms;     // Override PRESENCE_THRESHOLD_MS
  uint32_t dwell_threshold_ms;        // Override DWELL_THRESHOLD_MS
  uint32_t lost_timeout_ms;           // Override LOST_TIMEOUT_MS
  uint8_t  min_presence_count;        // Override MIN_PRESENCE_COUNT
  bool     emit_impulse_events;       // Emit RF_IMPULSE transitions
  bool     emit_narrative_hints;      // Include narrative_hint in events
};

// ════════════════════════════════════════════════════════════════════════════
// CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

// Event callback - called when state transitions occur
typedef void (*RfEventCallback)(const RfEvent* event);

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

// Initialization
bool init();
void deinit();
bool is_initialized();

// Enable/disable
bool enable();
void disable();
bool is_enabled();

// State access
RfState get_state();
RfStateSnapshot get_snapshot();
const char* state_name(RfState state);

// Settings
RfPresenceSettings get_settings();
bool set_settings(const RfPresenceSettings& settings);

// Callbacks
void set_event_callback(RfEventCallback cb);

// Update (call from loop)
// This processes raw BLE/WiFi scans through the privacy barrier
void update();

// Manual session rotation (for testing/privacy)
void rotate_session();

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL API (for integration with BLE/WiFi scanners)
// ════════════════════════════════════════════════════════════════════════════

// Feed raw BLE scan result through privacy barrier
// IMPORTANT: mac_address is used ONLY to derive session token, never stored
void feed_ble_scan(const uint8_t* mac_address, int8_t rssi, bool connectable);

// Feed WiFi probe detection through privacy barrier
// IMPORTANT: mac_address is used ONLY for dedup, never stored
void feed_wifi_probe(const uint8_t* mac_address, int8_t rssi);

// Feed environmental signals
void feed_temperature(float temp_celsius);
void feed_power_event(uint8_t flags);  // See POWER_FLAG_* constants

// Power event flags
static const uint8_t POWER_FLAG_BROWNOUT = 0x01;
static const uint8_t POWER_FLAG_LOW_VOLTAGE = 0x02;
static const uint8_t POWER_FLAG_LOAD_SPIKE = 0x04;

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE TESTING
// ════════════════════════════════════════════════════════════════════════════

// These functions support conformance testing per spec section 10
// All conformance checks log failures to health_log for diagnostics

// Verify no MAC addresses in persistent storage
// Checks struct sizes and scans token map for suspicious patterns
bool conformance_check_no_mac_storage();

// Verify session tokens rotate correctly
// WARNING: This test has a side effect - it rotates the session!
// Only call when you want to actually rotate, or in isolated test mode
bool conformance_check_token_rotation();

// Verify observation buffer contains only aggregates
// Scans observations for values outside expected ranges
bool conformance_check_aggregate_only();

// Verify secure memory wiping is functioning
// Tests that secure_wipe actually zeros memory
bool conformance_check_secure_wipe();

// Get session epoch (for rotation testing)
uint32_t get_session_epoch();

// ════════════════════════════════════════════════════════════════════════════
// SETTINGS BOUNDS (for API validation)
// ════════════════════════════════════════════════════════════════════════════

// Minimum and maximum values for configurable settings
static const uint32_t MIN_PRESENCE_THRESHOLD_MS = 1000;    // 1 second
static const uint32_t MAX_PRESENCE_THRESHOLD_MS = 300000;  // 5 minutes
static const uint32_t MIN_DWELL_THRESHOLD_MS = 5000;       // 5 seconds
static const uint32_t MAX_DWELL_THRESHOLD_MS = 600000;     // 10 minutes
static const uint32_t MIN_LOST_TIMEOUT_MS = 5000;          // 5 seconds
static const uint32_t MAX_LOST_TIMEOUT_MS = 300000;        // 5 minutes
static const uint8_t  MIN_PRESENCE_COUNT_SETTING = 1;
static const uint8_t  MAX_PRESENCE_COUNT_SETTING = 50;

} // namespace rf_presence

#endif // SECURACV_RF_PRESENCE_H
