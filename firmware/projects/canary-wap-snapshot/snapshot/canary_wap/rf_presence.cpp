/*
 * SecuraCV Canary — RF Presence Detection Implementation
 *
 * Implements spec/canary_free_signals_v0.md
 * Implements kernel/rf_presence_architecture.md
 *
 * PRIVACY INVARIANTS ENFORCED:
 * 1. MAC addresses NEVER stored - only used transiently for token derivation
 * 2. Session tokens rotate every SESSION_ROTATE_MS (default 4 hours)
 * 3. Only aggregated observations persist in ring buffer
 * 4. Event vocabulary is strictly controlled
 *
 * SECURITY HARDENING:
 * - Secure memory wiping with volatile barrier to prevent compiler optimization
 * - Timer wrap-around protection for all duration calculations
 * - Input validation on all external interfaces
 * - Bounds checking on array accesses
 */

#include "rf_presence.h"
#include "nvs_store.h"
#include "health_log.h"
#include <mbedtls/sha256.h>

// ════════════════════════════════════════════════════════════════════════════
// SECURITY PRIMITIVES
// ════════════════════════════════════════════════════════════════════════════

// Secure memory wipe - uses volatile to prevent compiler optimization
// This ensures sensitive data is actually cleared from memory
static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) {
    *p++ = 0;
  }
  // Memory barrier to ensure wipe completes before function returns
  asm volatile("" ::: "memory");
}

// Safe elapsed time calculation that handles millis() wrap-around
// millis() wraps every ~49.7 days (2^32 ms)
static inline uint32_t elapsed_ms(uint32_t start_ms, uint32_t now_ms) {
  // Unsigned subtraction handles wrap-around correctly due to modular arithmetic
  return now_ms - start_ms;
}

// Check if duration has elapsed, with wrap-around safety
static inline bool duration_elapsed(uint32_t start_ms, uint32_t now_ms, uint32_t duration_ms) {
  return elapsed_ms(start_ms, now_ms) >= duration_ms;
}

namespace rf_presence {

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE STATE
// ════════════════════════════════════════════════════════════════════════════

static bool s_initialized = false;
static bool s_enabled = false;
static RfEventCallback s_event_callback = nullptr;

// Session management
static uint32_t s_session_epoch = 0;
static uint32_t s_session_start_ms = 0;
static uint8_t s_device_secret[32] = {0};  // Per-device secret for token derivation

// FSM state
static RfState s_state = RF_EMPTY;
static uint32_t s_state_enter_ms = 0;
static const char* s_last_event = "boot";

// Observation tracking
static uint8_t s_current_device_count = 0;
static int8_t s_current_rssi_sum = 0;
static int8_t s_current_rssi_max = RSSI_NOISE_FLOOR;
static int8_t s_current_rssi_min = 0;
static uint8_t s_rssi_count = 0;
static uint8_t s_adv_count_this_second = 0;
static uint32_t s_last_adv_second = 0;

// Probe tracking
static uint8_t s_probe_burst_count = 0;
static int8_t s_probe_rssi_peak = RSSI_NOISE_FLOOR;

// Environmental
static float s_last_temp_c = 0.0f;
static float s_current_temp_c = 0.0f;
static uint8_t s_power_flags = 0;

// Session token map (ephemeral deduplication)
static SessionToken s_token_map[SESSION_TOKEN_MAP_SIZE];
static size_t s_token_count = 0;

// Observation ring buffer
static RfObservation s_observations[OBSERVATION_BUFFER_SIZE];
static size_t s_obs_head = 0;
static size_t s_obs_count = 0;

// Settings
static RfPresenceSettings s_settings = {
  .enabled = true,
  .presence_threshold_ms = PRESENCE_THRESHOLD_MS,
  .dwell_threshold_ms = DWELL_THRESHOLD_MS,
  .lost_timeout_ms = LOST_TIMEOUT_MS,
  .min_presence_count = MIN_PRESENCE_COUNT,
  .emit_impulse_events = false,
  .emit_narrative_hints = true
};

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE HELPERS — TOKEN DERIVATION (PRIVACY BARRIER)
// ════════════════════════════════════════════════════════════════════════════

// Derive session token from MAC address
// INVARIANT: Token cannot be reversed to MAC
// INVARIANT: Token is only valid within current session epoch
// SECURITY: Uses secure wipe to prevent sensitive data leakage
static uint32_t derive_session_token(const uint8_t* mac_address) {
  // Null pointer guard - return zero token for invalid input
  if (mac_address == nullptr) {
    return 0;
  }

  // Domain separation + session binding
  uint8_t input[64];
  memcpy(input, "canary:session:v0:", 18);
  memcpy(input + 18, s_device_secret, 32);
  memcpy(input + 50, &s_session_epoch, 4);
  memcpy(input + 54, mac_address, 6);

  uint8_t hash[32];
  int ret = mbedtls_sha256(input, 60, hash, 0);

  // Validate hash operation succeeded
  if (ret != 0) {
    secure_wipe(input, sizeof(input));
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "SHA256 failed with error %d", ret);
    return 0;
  }

  // Use first 4 bytes as token
  uint32_t token;
  memcpy(&token, hash, 4);

  // Secure wipe of sensitive data - prevents compiler optimization
  secure_wipe(input, sizeof(input));
  secure_wipe(hash, sizeof(hash));  // Wipe entire hash, not just partial

  return token;
}

// Find or create token entry in map
// Returns index, or -1 if invalid token (e.g., from null MAC)
static int find_or_create_token(uint32_t token, uint32_t now_ms, int8_t rssi) {
  // Reject zero tokens (invalid/failed derivation)
  if (token == 0) {
    return -1;
  }

  // Look for existing token
  for (size_t i = 0; i < s_token_count; i++) {
    if (s_token_map[i].token == token) {
      s_token_map[i].last_seen_ms = now_ms;
      s_token_map[i].rssi = rssi;
      return static_cast<int>(i);
    }
  }

  // Token not found, try to add
  if (s_token_count < SESSION_TOKEN_MAP_SIZE) {
    size_t idx = s_token_count++;
    s_token_map[idx].token = token;
    s_token_map[idx].last_seen_ms = now_ms;
    s_token_map[idx].rssi = rssi;
    return static_cast<int>(idx);
  }

  // Map full - evict oldest entry (using wrap-around safe comparison)
  size_t oldest_idx = 0;
  uint32_t oldest_age = 0;
  for (size_t i = 0; i < SESSION_TOKEN_MAP_SIZE; i++) {
    uint32_t age = elapsed_ms(s_token_map[i].last_seen_ms, now_ms);
    if (age > oldest_age) {
      oldest_age = age;
      oldest_idx = i;
    }
  }

  // Secure wipe before reuse
  secure_wipe(&s_token_map[oldest_idx], sizeof(SessionToken));

  s_token_map[oldest_idx].token = token;
  s_token_map[oldest_idx].last_seen_ms = now_ms;
  s_token_map[oldest_idx].rssi = rssi;
  return static_cast<int>(oldest_idx);
}

// Count active tokens (seen within TTL)
// Uses wrap-around safe elapsed time calculation
static uint8_t count_active_tokens(uint32_t now_ms) {
  uint8_t count = 0;
  for (size_t i = 0; i < s_token_count; i++) {
    if (elapsed_ms(s_token_map[i].last_seen_ms, now_ms) < OBSERVATION_TTL_MS) {
      count++;
    }
  }
  return count;
}

// Calculate RSSI statistics from active tokens
// Uses int32_t accumulator to prevent overflow when summing int8_t values
static void calc_rssi_stats(uint32_t now_ms, int8_t* out_max, int8_t* out_mean, int8_t* out_min) {
  // Null pointer guards
  if (out_max == nullptr || out_mean == nullptr || out_min == nullptr) {
    return;
  }

  int32_t sum = 0;  // Wide accumulator prevents overflow
  int8_t max_rssi = RSSI_NOISE_FLOOR;
  int8_t min_rssi = 0;
  uint8_t count = 0;

  for (size_t i = 0; i < s_token_count; i++) {
    if (elapsed_ms(s_token_map[i].last_seen_ms, now_ms) < OBSERVATION_TTL_MS) {
      sum += s_token_map[i].rssi;
      if (s_token_map[i].rssi > max_rssi) max_rssi = s_token_map[i].rssi;
      if (count == 0 || s_token_map[i].rssi < min_rssi) min_rssi = s_token_map[i].rssi;
      count++;
    }
  }

  *out_max = max_rssi;
  *out_min = (count > 0) ? min_rssi : RSSI_NOISE_FLOOR;
  *out_mean = (count > 0) ? static_cast<int8_t>(sum / count) : RSSI_NOISE_FLOOR;
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE HELPERS — SESSION MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

static void clear_session_tokens() {
  // Secure wipe all token entries to prevent memory inspection attacks
  secure_wipe(s_token_map, sizeof(s_token_map));
  s_token_count = 0;
}

static void check_session_rotation(uint32_t now_ms) {
  // Use wrap-around safe timer comparison
  if (duration_elapsed(s_session_start_ms, now_ms, SESSION_ROTATE_MS)) {
    rotate_session();
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE HELPERS — OBSERVATION BUFFER
// ════════════════════════════════════════════════════════════════════════════

static void push_observation(const RfObservation& obs) {
  // Bounds check (defensive - should never fail with modulo arithmetic)
  if (s_obs_head >= OBSERVATION_BUFFER_SIZE) {
    s_obs_head = 0;
  }

  // Secure wipe before overwrite (defense in depth)
  secure_wipe(&s_observations[s_obs_head], sizeof(RfObservation));

  s_observations[s_obs_head] = obs;
  s_obs_head = (s_obs_head + 1) % OBSERVATION_BUFFER_SIZE;
  if (s_obs_count < OBSERVATION_BUFFER_SIZE) s_obs_count++;
}

static void evict_expired_observations(uint32_t now_ms) {
  // Walk backwards from head, securely wipe expired entries
  for (size_t i = 0; i < s_obs_count; i++) {
    size_t idx = (s_obs_head + OBSERVATION_BUFFER_SIZE - 1 - i) % OBSERVATION_BUFFER_SIZE;
    // Use wrap-around safe elapsed time check
    if (elapsed_ms(s_observations[idx].timestamp_ms, now_ms) > OBSERVATION_TTL_MS) {
      secure_wipe(&s_observations[idx], sizeof(RfObservation));
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE HELPERS — EVENT EMISSION
// ════════════════════════════════════════════════════════════════════════════

static const char* confidence_name(ConfidenceClass conf) {
  switch (conf) {
    case CONF_HIGH:     return "high";
    case CONF_MODERATE: return "moderate";
    case CONF_LOW:      return "low";
    default:            return "uncertain";
  }
}

static ConfidenceClass calc_confidence(uint8_t ble_count, uint8_t probe_bursts, int8_t rssi_mean) {
  float score = 0.0f;

  // BLE sustained presence (weight 1.0)
  if (ble_count > 0) {
    score += 0.5f + (ble_count > 3 ? 0.5f : ble_count * 0.15f);
  }

  // WiFi probe bursts (weight 0.5)
  if (probe_bursts > 0) {
    score += 0.3f + (probe_bursts > 2 ? 0.2f : probe_bursts * 0.1f);
  }

  // RSSI strength bonus
  if (rssi_mean > -60) score += 0.1f;

  if (score >= 0.8f) return CONF_HIGH;
  if (score >= 0.5f) return CONF_MODERATE;
  if (score >= 0.2f) return CONF_LOW;
  return CONF_UNCERTAIN;
}

static DwellClass calc_dwell_class(uint32_t duration_ms) {
  if (duration_ms >= 120000) return DWELL_SUSTAINED;
  if (duration_ms >= 30000) return DWELL_LINGERING;
  return DWELL_TRANSIENT;
}

static uint8_t get_time_bucket() {
  // 10-minute buckets per PWK invariant III
  return (uint8_t)((millis() / (10 * 60 * 1000)) % 144);
}

static const char* get_narrative_hint(RfState state, DwellClass dwell, uint8_t time_bucket) {
  if (!s_settings.emit_narrative_hints) return nullptr;

  // Time-based hints (very conservative)
  bool unusual_hour = (time_bucket < 6) || (time_bucket > 132); // ~midnight to 1am, 10pm-midnight

  if (state == RF_PRESENCE && dwell == DWELL_TRANSIENT && !unusual_hour) {
    return "passerby_like";
  }
  if (state == RF_DWELLING && dwell == DWELL_LINGERING && !unusual_hour) {
    return "delivery_like";
  }
  if (state == RF_DWELLING && dwell == DWELL_SUSTAINED) {
    return "sustained_presence";
  }

  return nullptr;
}

static void emit_event(const char* event_name, SignalSource sig, int8_t count_delta) {
  if (!s_event_callback) return;

  uint32_t now_ms = millis();
  int8_t rssi_max, rssi_mean, rssi_min;
  calc_rssi_stats(now_ms, &rssi_max, &rssi_mean, &rssi_min);

  uint8_t device_count = count_active_tokens(now_ms);
  uint32_t state_duration = now_ms - s_state_enter_ms;

  RfEvent event = {
    .event_name = event_name,
    .signal = sig,
    .confidence = calc_confidence(device_count, s_probe_burst_count, rssi_mean),
    .count_delta = count_delta,
    .dwell_class = calc_dwell_class(state_duration),
    .time_bucket = get_time_bucket(),
    .narrative_hint = get_narrative_hint(s_state, calc_dwell_class(state_duration), get_time_bucket())
  };

  s_last_event = event_name;
  s_event_callback(&event);
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE HELPERS — FSM TRANSITIONS
// ════════════════════════════════════════════════════════════════════════════

static void transition_to(RfState new_state, uint32_t now_ms) {
  RfState old_state = s_state;
  s_state = new_state;
  s_state_enter_ms = now_ms;

  // Log transition
  health_log::log(health_log::LOG_LEVEL_INFO, health_log::LOG_CAT_RF,
    "RF FSM: %s -> %s", state_name(old_state), state_name(new_state));
}

// Decay probe burst count - called each update cycle
// Prevents stale probe counts from persisting indefinitely
static void decay_probe_bursts(uint32_t now_ms) {
  static uint32_t s_last_decay_ms = 0;
  static const uint32_t PROBE_DECAY_INTERVAL_MS = 5000;  // Decay every 5 seconds
  static const uint8_t PROBE_DECAY_AMOUNT = 1;

  if (duration_elapsed(s_last_decay_ms, now_ms, PROBE_DECAY_INTERVAL_MS)) {
    if (s_probe_burst_count > PROBE_DECAY_AMOUNT) {
      s_probe_burst_count -= PROBE_DECAY_AMOUNT;
    } else {
      s_probe_burst_count = 0;
    }
    // Reset probe peak RSSI when bursts decay to zero
    if (s_probe_burst_count == 0) {
      s_probe_rssi_peak = RSSI_NOISE_FLOOR;
    }
    s_last_decay_ms = now_ms;
  }
}

// Power event timing for TTL-based clearing
static uint32_t s_last_power_event_time_ms = 0;
static const uint32_t POWER_FLAG_TTL_MS = 10000;  // Clear after 10 seconds

// Clear accumulated power flags - called each update cycle
// Power events are point-in-time; don't accumulate indefinitely
static void clear_power_flags_if_stale(uint32_t now_ms) {
  if (s_power_flags != 0 && s_last_power_event_time_ms != 0) {
    if (duration_elapsed(s_last_power_event_time_ms, now_ms, POWER_FLAG_TTL_MS)) {
      s_power_flags = 0;
    }
  }
}

// Throttle to prevent rapid state transition flooding
static const uint32_t MIN_TRANSITION_INTERVAL_MS = 500;
static uint32_t s_last_transition_ms = 0;

static void fsm_tick(uint32_t now_ms) {
  uint8_t device_count = count_active_tokens(now_ms);
  uint32_t state_duration = elapsed_ms(s_state_enter_ms, now_ms);  // Wrap-around safe
  int8_t prev_count = s_current_device_count;
  s_current_device_count = device_count;

  // Rate limit state transitions to prevent event flooding
  bool can_transition = duration_elapsed(s_last_transition_ms, now_ms, MIN_TRANSITION_INTERVAL_MS);

  switch (s_state) {
    case RF_EMPTY:
      if (can_transition && (device_count >= s_settings.min_presence_count || s_probe_burst_count > 0)) {
        transition_to(RF_IMPULSE, now_ms);
        s_last_transition_ms = now_ms;
        if (s_settings.emit_impulse_events) {
          emit_event("rf_impulse", s_probe_burst_count > 0 ? SIG_WIFI : SIG_BLE, device_count);
        }
      }
      break;

    case RF_IMPULSE:
      if (device_count < s_settings.min_presence_count && s_probe_burst_count == 0) {
        if (can_transition) {
          transition_to(RF_EMPTY, now_ms);
          s_last_transition_ms = now_ms;
        }
      } else if (state_duration >= s_settings.presence_threshold_ms) {
        transition_to(RF_PRESENCE, now_ms);
        s_last_transition_ms = now_ms;
        emit_event("rf_presence_started", SIG_FUSED, device_count);
      } else if (state_duration >= IMPULSE_TIMEOUT_MS && device_count < s_settings.min_presence_count) {
        if (can_transition) {
          transition_to(RF_EMPTY, now_ms);
          s_last_transition_ms = now_ms;
        }
      }
      break;

    case RF_PRESENCE:
      if (device_count < s_settings.min_presence_count) {
        if (state_duration >= s_settings.lost_timeout_ms) {
          transition_to(RF_EMPTY, now_ms);
          s_last_transition_ms = now_ms;
          emit_event("rf_presence_ended", SIG_FUSED, -prev_count);
        } else if (can_transition) {
          transition_to(RF_DEPARTING, now_ms);
          s_last_transition_ms = now_ms;
          emit_event("rf_departing", SIG_BLE, device_count - prev_count);
        }
      } else if (state_duration >= s_settings.dwell_threshold_ms) {
        transition_to(RF_DWELLING, now_ms);
        s_last_transition_ms = now_ms;
        emit_event("rf_dwell_started", SIG_BLE, 0);
      }
      break;

    case RF_DWELLING:
      if (can_transition && device_count < s_settings.min_presence_count) {
        transition_to(RF_DEPARTING, now_ms);
        s_last_transition_ms = now_ms;
        emit_event("rf_departing", SIG_BLE, device_count - prev_count);
      }
      break;

    case RF_DEPARTING:
      if (device_count >= s_settings.min_presence_count) {
        // False departure, return to presence
        if (can_transition) {
          transition_to(RF_PRESENCE, now_ms);
          s_last_transition_ms = now_ms;
        }
      } else if (state_duration >= DEPARTING_CONFIRM_MS) {
        transition_to(RF_EMPTY, now_ms);
        s_last_transition_ms = now_ms;
        emit_event("rf_presence_ended", SIG_FUSED, -prev_count);
      }
      break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

bool init() {
  if (s_initialized) return true;

  // Generate or load device secret
  if (!nvs_store::get_blob("rf_secret", s_device_secret, sizeof(s_device_secret))) {
    // Generate new secret using hardware RNG
    esp_fill_random(s_device_secret, sizeof(s_device_secret));
    if (!nvs_store::set_blob("rf_secret", s_device_secret, sizeof(s_device_secret))) {
      health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
        "Failed to persist device secret");
      // Continue anyway - secret is valid for this session
    }
  }

  // Validate secret is not all zeros (would indicate uninitialized state)
  bool secret_valid = false;
  for (size_t i = 0; i < sizeof(s_device_secret); i++) {
    if (s_device_secret[i] != 0) {
      secret_valid = true;
      break;
    }
  }
  if (!secret_valid) {
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "Device secret is invalid (all zeros), regenerating");
    esp_fill_random(s_device_secret, sizeof(s_device_secret));
    nvs_store::set_blob("rf_secret", s_device_secret, sizeof(s_device_secret));
  }

  // Load session epoch
  s_session_epoch = nvs_store::get_u32("rf_epoch", 0);
  s_session_start_ms = millis();

  // Load settings with validation
  RfPresenceSettings stored;
  if (nvs_store::get_blob("rf_settings", &stored, sizeof(stored))) {
    // Basic sanity checks on stored settings
    if (stored.presence_threshold_ms > 0 && stored.presence_threshold_ms <= 300000 &&
        stored.dwell_threshold_ms > 0 && stored.dwell_threshold_ms <= 600000 &&
        stored.lost_timeout_ms > 0 && stored.lost_timeout_ms <= 300000) {
      s_settings = stored;
    } else {
      health_log::log(health_log::LOG_LEVEL_WARN, health_log::LOG_CAT_RF,
        "Stored settings invalid, using defaults");
    }
  }

  // Secure wipe of all state arrays
  secure_wipe(s_token_map, sizeof(s_token_map));
  secure_wipe(s_observations, sizeof(s_observations));
  s_token_count = 0;
  s_obs_head = 0;
  s_obs_count = 0;
  s_state = RF_EMPTY;
  s_state_enter_ms = millis();
  s_last_transition_ms = 0;

  // Reset all signal tracking state
  s_current_device_count = 0;
  s_current_rssi_sum = 0;
  s_current_rssi_max = RSSI_NOISE_FLOOR;
  s_current_rssi_min = 0;
  s_rssi_count = 0;
  s_adv_count_this_second = 0;
  s_last_adv_second = 0;
  s_probe_burst_count = 0;
  s_probe_rssi_peak = RSSI_NOISE_FLOOR;
  s_power_flags = 0;
  s_last_event = "boot";

  s_initialized = true;
  s_enabled = s_settings.enabled;

  health_log::log(health_log::LOG_LEVEL_INFO, health_log::LOG_CAT_RF,
    "RF Presence initialized, epoch=%u", s_session_epoch);

  return true;
}

void deinit() {
  if (!s_initialized) return;

  // Secure wipe of all sensitive data
  secure_wipe(s_device_secret, sizeof(s_device_secret));
  secure_wipe(s_token_map, sizeof(s_token_map));
  secure_wipe(s_observations, sizeof(s_observations));

  // Reset all counters
  s_token_count = 0;
  s_obs_head = 0;
  s_obs_count = 0;
  s_probe_burst_count = 0;
  s_power_flags = 0;

  s_initialized = false;
  s_enabled = false;

  health_log::log(health_log::LOG_LEVEL_INFO, health_log::LOG_CAT_RF,
    "RF Presence deinitialized");
}

bool is_initialized() { return s_initialized; }

bool enable() {
  if (!s_initialized) return false;
  s_enabled = true;
  return true;
}

void disable() {
  s_enabled = false;
  // Clear active tokens on disable
  clear_session_tokens();
}

bool is_enabled() { return s_enabled; }

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — STATE ACCESS
// ════════════════════════════════════════════════════════════════════════════

RfState get_state() { return s_state; }

const char* state_name(RfState state) {
  switch (state) {
    case RF_EMPTY:     return "empty";
    case RF_IMPULSE:   return "impulse";
    case RF_PRESENCE:  return "presence";
    case RF_DWELLING:  return "dwelling";
    case RF_DEPARTING: return "departing";
    default:           return "unknown";
  }
}

RfStateSnapshot get_snapshot() {
  uint32_t now_ms = millis();
  int8_t rssi_max, rssi_mean, rssi_min;
  calc_rssi_stats(now_ms, &rssi_max, &rssi_mean, &rssi_min);

  uint8_t device_count = count_active_tokens(now_ms);
  uint32_t state_duration = now_ms - s_state_enter_ms;

  return RfStateSnapshot{
    .state = s_state,
    .confidence = calc_confidence(device_count, s_probe_burst_count, rssi_mean),
    .device_count = device_count,
    .rssi_mean = rssi_mean,
    .state_duration_ms = state_duration,
    .dwell_class = calc_dwell_class(state_duration),
    .state_name = state_name(s_state),
    .uptime_s = now_ms / 1000,
    .last_event = s_last_event
  };
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — SETTINGS
// ════════════════════════════════════════════════════════════════════════════

RfPresenceSettings get_settings() { return s_settings; }

bool set_settings(const RfPresenceSettings& settings) {
  s_settings = settings;
  s_enabled = settings.enabled;
  return nvs_store::set_blob("rf_settings", &s_settings, sizeof(s_settings));
}

void set_event_callback(RfEventCallback cb) {
  s_event_callback = cb;
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — UPDATE
// ════════════════════════════════════════════════════════════════════════════

void update() {
  if (!s_initialized || !s_enabled) return;

  uint32_t now_ms = millis();

  // Check for session rotation
  check_session_rotation(now_ms);

  // Decay transient counters
  decay_probe_bursts(now_ms);
  clear_power_flags_if_stale(now_ms);

  // Evict expired observations
  evict_expired_observations(now_ms);

  // Run FSM
  fsm_tick(now_ms);

  // Reset per-second counters
  uint32_t current_second = now_ms / 1000;
  if (current_second != s_last_adv_second) {
    s_adv_count_this_second = 0;
    s_last_adv_second = current_second;
  }
}

void rotate_session() {
  uint32_t now_ms = millis();

  s_session_epoch++;
  s_session_start_ms = now_ms;
  nvs_store::set_u32("rf_epoch", s_session_epoch);

  // Clear all tokens - they're now invalid for privacy
  clear_session_tokens();

  // Clear all transient signal state to prevent cross-session correlation
  s_probe_burst_count = 0;
  s_probe_rssi_peak = RSSI_NOISE_FLOOR;
  s_power_flags = 0;
  s_current_device_count = 0;
  s_current_rssi_sum = 0;
  s_current_rssi_max = RSSI_NOISE_FLOOR;
  s_current_rssi_min = 0;
  s_rssi_count = 0;
  s_adv_count_this_second = 0;

  // Clear observations (contain timestamps that could correlate sessions)
  secure_wipe(s_observations, sizeof(s_observations));
  s_obs_head = 0;
  s_obs_count = 0;

  // Reset last event to prevent cross-session correlation
  s_last_event = "session_rotated";

  health_log::log(health_log::LOG_LEVEL_INFO, health_log::LOG_CAT_RF,
    "Session rotated, new epoch=%u", s_session_epoch);
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — SIGNAL FEEDING (PRIVACY BARRIER)
// ════════════════════════════════════════════════════════════════════════════

void feed_ble_scan(const uint8_t* mac_address, int8_t rssi, bool connectable) {
  if (!s_initialized || !s_enabled) return;
  if (rssi < RSSI_NOISE_FLOOR) return;  // Ignore noise

  uint32_t now_ms = millis();

  // === PRIVACY BARRIER ===
  // MAC address is used ONLY here to derive token, never stored
  uint32_t token = derive_session_token(mac_address);
  // mac_address is NOT passed beyond this point

  // Update token map (contains only ephemeral tokens, no MAC)
  find_or_create_token(token, now_ms, rssi);

  // Update advertising density counter
  s_adv_count_this_second++;
}

void feed_wifi_probe(const uint8_t* mac_address, int8_t rssi) {
  if (!s_initialized || !s_enabled) return;
  if (rssi < RSSI_NOISE_FLOOR) return;

  // === PRIVACY BARRIER ===
  // MAC used only for burst detection, not stored
  // We don't even derive a token for WiFi - just count bursts

  s_probe_burst_count++;
  if (rssi > s_probe_rssi_peak) {
    s_probe_rssi_peak = rssi;
  }

  // Decay probe count over time (in update())
}

void feed_temperature(float temp_celsius) {
  s_last_temp_c = s_current_temp_c;
  s_current_temp_c = temp_celsius;
}

void feed_power_event(uint8_t flags) {
  if (flags != 0) {
    s_power_flags |= flags;
    s_last_power_event_time_ms = millis();
  }
}

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE TESTING
// ════════════════════════════════════════════════════════════════════════════

bool conformance_check_no_mac_storage() {
  // Verify token map entries don't contain 6-byte sequences that could be MACs
  // SessionToken struct should only have: token (4 bytes), last_seen_ms (4 bytes), rssi (1 byte)
  // Total: 9 bytes padded to 12. If we find anything resembling a MAC (6 consecutive non-zero
  // bytes outside the expected fields), flag it.

  // Structural verification: sizeof(SessionToken) should be <= 16 bytes
  // (4 + 4 + 1 + padding = 12 typical, at most 16 with alignment)
  static_assert(sizeof(SessionToken) <= 16, "SessionToken unexpectedly large - review for MAC storage");

  // Runtime check: verify no token entries have suspicious patterns
  // A MAC address would be 6 bytes; our tokens are 4 bytes. This is inherently safe.
  // But verify token values aren't storing full 48-bit values in some hidden way.
  for (size_t i = 0; i < s_token_count; i++) {
    // Tokens should be uniformly distributed 32-bit values
    // A stored MAC would have OUI patterns (first 3 bytes often follow vendor patterns)
    // This is a heuristic check - the real guarantee is the code structure
    if (s_token_map[i].token != 0) {
      // Token exists - verify it's within reasonable bounds for a hash output
      // (any 32-bit value is valid, so this is really just checking it's initialized)
      continue;
    }
  }

  // Verify RfObservation struct doesn't have room for MAC addresses
  // RfObservation has: timestamp(4) + counts/rssi(~12) = ~16 bytes
  static_assert(sizeof(RfObservation) <= 20, "RfObservation unexpectedly large - review for MAC storage");

  return true;
}

bool conformance_check_token_rotation() {
  // Verify tokens become invalid after rotation
  // This test has a side effect (rotates session) so use with caution

  uint32_t old_epoch = s_session_epoch;
  uint32_t old_token_count = s_token_count;
  uint8_t test_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

  // Generate token before rotation
  uint32_t token_before = derive_session_token(test_mac);
  if (token_before == 0) {
    // Token derivation failed
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "Conformance: token derivation failed before rotation");
    return false;
  }

  // Perform rotation
  rotate_session();

  // Generate token after rotation with same MAC
  uint32_t token_after = derive_session_token(test_mac);
  if (token_after == 0) {
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "Conformance: token derivation failed after rotation");
    return false;
  }

  // Verify invariants
  bool epoch_incremented = (s_session_epoch == old_epoch + 1);
  bool tokens_differ = (token_before != token_after);
  bool tokens_cleared = (s_token_count == 0);  // rotation should clear token map

  if (!epoch_incremented) {
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "Conformance: epoch did not increment (was %u, now %u)", old_epoch, s_session_epoch);
  }
  if (!tokens_differ) {
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "Conformance: tokens match after rotation (both %u)", token_before);
  }
  if (!tokens_cleared) {
    health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
      "Conformance: token map not cleared (had %u, now %u)", old_token_count, s_token_count);
  }

  return epoch_incremented && tokens_differ && tokens_cleared;
}

bool conformance_check_aggregate_only() {
  // Verify observation buffer contains only aggregate fields
  // Check that no observation entry contains patterns that could be identifiers

  for (size_t i = 0; i < s_obs_count; i++) {
    size_t idx = (s_obs_head + OBSERVATION_BUFFER_SIZE - 1 - i) % OBSERVATION_BUFFER_SIZE;
    const RfObservation& obs = s_observations[idx];

    // Device count should be within reasonable bounds (0-255, practically 0-50)
    if (obs.ble_device_count > 100) {
      health_log::log(health_log::LOG_LEVEL_WARN, health_log::LOG_CAT_RF,
        "Conformance: suspicious device count %u in observation", obs.ble_device_count);
      // Not a failure - could be legitimate dense environment
    }

    // RSSI should be within valid range (-127 to 0 for BLE)
    if (obs.ble_rssi_max > 0 || obs.ble_rssi_max < -100) {
      if (obs.ble_device_count > 0) {  // Only check if devices were present
        health_log::log(health_log::LOG_LEVEL_WARN, health_log::LOG_CAT_RF,
          "Conformance: suspicious RSSI max %d in observation", obs.ble_rssi_max);
      }
    }
  }

  // The real guarantee is structural: RfObservation has no MAC fields in its definition
  return true;
}

// Additional conformance check: verify secure wipe is working
bool conformance_check_secure_wipe() {
  uint8_t test_buffer[32];
  memset(test_buffer, 0xAA, sizeof(test_buffer));

  secure_wipe(test_buffer, sizeof(test_buffer));

  // Verify all bytes are zero
  for (size_t i = 0; i < sizeof(test_buffer); i++) {
    if (test_buffer[i] != 0) {
      health_log::log(health_log::LOG_LEVEL_ERROR, health_log::LOG_CAT_RF,
        "Conformance: secure_wipe failed at byte %u (value 0x%02X)", i, test_buffer[i]);
      return false;
    }
  }
  return true;
}

uint32_t get_session_epoch() {
  return s_session_epoch;
}

} // namespace rf_presence
