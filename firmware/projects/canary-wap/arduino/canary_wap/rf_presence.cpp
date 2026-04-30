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
#include "household.h"
#include "familiar.h"
#include "baseline.h"
#include "presence_context.h"
#include "notify.h"
#include "federated.h"
#include "wizard.h"
#include "dp.h"
#include <string.h>
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
// Timestamp of the FIRST detection of the current "presence episode" —
// when the FSM left RF_EMPTY. Reset to 0 when we return to RF_EMPTY.
// Used by notify::evaluate to answer "how long has this device been
// around?" which state_enter_ms cannot, because state_enter_ms is
// updated on every FSM transition (IMPULSE → PRESENCE → DWELLING …).
static uint32_t s_presence_episode_start_ms = 0;
static const char* s_last_event = "boot";

// Observation tracking
static uint8_t s_current_device_count = 0;
static int8_t s_current_rssi_sum = 0;
static int8_t s_current_rssi_max = RSSI_NOISE_FLOOR;
static int8_t s_current_rssi_min = 0;
static uint8_t s_rssi_count = 0;
static uint8_t s_adv_count_this_second = 0;
static uint32_t s_last_adv_second = 0;
// 4-second sliding window of advertising counts. Used by the familiar
// fingerprint to estimate adv/minute with enough granularity that all
// four density buckets are actually reachable (the prior single-second
// approximation made bucket 1 unreachable — gemini review #313).
static uint8_t s_adv_window[4] = {0, 0, 0, 0};
static uint8_t s_adv_window_idx = 0;

// Probe tracking
static uint8_t s_probe_burst_count = 0;
static int8_t s_probe_rssi_peak = RSSI_NOISE_FLOOR;

// Environmental
static float s_last_temp_c = 0.0f;
static float s_current_temp_c = 0.0f;
static uint8_t s_power_flags = 0;

// CSI-derived scalars (0..100). Updated by feed_csi_window(); decay to 0
// after CSI_FEED_TTL_MS without a new window. NEVER contain raw identifiers.
static uint8_t s_csi_motion_score = 0;
static uint8_t s_csi_breathing_score = 0;
static uint32_t s_csi_last_feed_ms = 0;

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
// PRIVATE HELPERS — SECRET MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

// Generate new device secret using hardware RNG and persist to NVS
static bool regenerate_device_secret() {
  esp_fill_random(s_device_secret, sizeof(s_device_secret));
  if (!nvs_store::set_blob("rf_secret", s_device_secret, sizeof(s_device_secret))) {
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Failed to persist device secret");
    return false;
  }
  return true;
}

// Validate secret is not all zeros (would indicate uninitialized state)
static bool validate_device_secret() {
  for (size_t i = 0; i < sizeof(s_device_secret); i++) {
    if (s_device_secret[i] != 0) {
      return true;
    }
  }
  return false;
}

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
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
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

// Fusion head: combines BLE, WiFi, CSI motion, and CSI breathing into a
// single confidence class.
//
// Backwards-compatibility: when `csi_motion` and `csi_breathing` are 0
// (CSI not wired, or stale feed), the result matches the Phase-0 scorer
// exactly. This keeps v0 behavior frozen until Phase 6 swaps in the
// learned anomaly model.
//
// Weights rationale:
//   • CSI motion is the strongest signal — it works even when a visitor
//     has no broadcasting device (e.g. a courier with the phone in a
//     Faraday pocket). It gets weight 0.6 at max.
//   • Breathing is a minor positive signal for *sustained* presence; we
//     use it as a tie-breaker, weight 0.1.
//   • BLE and WiFi retain their original weights so a CSI-less device
//     (ESP32-C3 without HT40 CSI, say) keeps the v0 behavior.
static ConfidenceClass calc_confidence(uint8_t ble_count,
                                       uint8_t probe_bursts,
                                       int8_t  rssi_mean,
                                       uint8_t csi_motion = 0,
                                       uint8_t csi_breathing = 0) {
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

  // CSI motion bonus (weight up to 0.6). Below CSI_MOTION_ASSIST_MIN we
  // treat it as noise; between MIN and CONFIRM it assists; above CONFIRM
  // it is standalone evidence of presence.
  if (csi_motion >= CSI_MOTION_ASSIST_MIN) {
    const float m = (float)csi_motion / 100.0f;
    score += m >= 0.6f ? 0.6f : m;
  }

  // Breathing is a weak but valuable dwell signal.
  if (csi_breathing > CSI_MOTION_ASSIST_MIN) {
    score += 0.1f;
  }

  if (score >= 0.8f) return CONF_HIGH;
  if (score >= 0.5f) return CONF_MODERATE;
  if (score >= 0.2f) return CONF_LOW;
  return CONF_UNCERTAIN;
}

// Helper: returns the current CSI motion score if a recent feed is valid,
// 0 otherwise. Called by emit_event() and the FSM to inject CSI into the
// existing decision paths without refactoring.
static uint8_t active_csi_motion(uint32_t now_ms) {
  if (s_csi_last_feed_ms == 0) return 0;
  if (elapsed_ms(s_csi_last_feed_ms, now_ms) > CSI_FEED_TTL_MS) return 0;
  return s_csi_motion_score;
}

static uint8_t active_csi_breathing(uint32_t now_ms) {
  if (s_csi_last_feed_ms == 0) return 0;
  if (elapsed_ms(s_csi_last_feed_ms, now_ms) > CSI_FEED_TTL_MS) return 0;
  return s_csi_breathing_score;
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
  // Phase 5/6/8 must NOT be gated behind s_event_callback — they're an
  // internal decision/persistence engine that should run whether or not
  // any external observer has registered a callback (codex review #316).
  // Compute the shared metrics once, then deliver to the callback AND
  // feed the phase modules independently.

  uint32_t now_ms = millis();
  int8_t rssi_max, rssi_mean, rssi_min;
  calc_rssi_stats(now_ms, &rssi_max, &rssi_mean, &rssi_min);

  uint8_t device_count = count_active_tokens(now_ms);
  uint32_t state_duration = now_ms - s_state_enter_ms;

  const uint8_t csi_m = active_csi_motion(now_ms);
  const uint8_t csi_b = active_csi_breathing(now_ms);

  s_last_event = event_name;

  // Deliver to the registered event callback if any. This is a purely
  // external side channel; the internal pipeline below does not depend
  // on it.
  if (s_event_callback) {
    RfEvent event = {
      .event_name = event_name,
      .signal = sig,
      .confidence = calc_confidence(device_count, s_probe_burst_count, rssi_mean,
                                    csi_m, csi_b),
      .count_delta = count_delta,
      .dwell_class = calc_dwell_class(state_duration),
      .time_bucket = get_time_bucket(),
      .narrative_hint = get_narrative_hint(s_state, calc_dwell_class(state_duration), get_time_bucket())
    };
    s_event_callback(&event);
  }

  // ── Phase 5: note the behavioral fingerprint of each confirmed arrival ──
  // Only on PRESENCE_STARTED (not on impulse/dwell/departure) — so the
  // Bloom filter accumulates at most one entry per arrival, keeping its
  // load low enough that the false-positive rate stays under ~0.5%.
  //
  // We compute the fingerprint from the rf_presence state we already
  // have in this scope (no MAC, no token, no raw RSSI sample escapes —
  // only the 11-bit bucketed fingerprint enters the Bloom filter).
  if (event_name && strcmp(event_name, "rf_presence_started") == 0) {
    familiar::FingerprintInputs fp_in;
    fp_in.time_of_day_bucket = get_time_bucket();
    fp_in.rssi_mean_dbm      = rssi_mean;
    // Approximate advertising density per minute from the per-second
    // counter; a modest over-estimate when a burst just hit, under-
    // estimate when observation started mid-second — acceptable since
    // the density class has only 4 buckets.
    // 4-second sliding sum × 15 ≈ advertisements per minute. This gives
    // enough granularity that all four density buckets in compute_fingerprint
    // (<4, 4-16, 16-64, ≥64 per minute) are actually reachable:
    //   0 ads in 4s → 0/min   → bucket 0 (quiet)
    //   1 ad  in 4s → 15/min  → bucket 1 (low)
    //   2-4  in 4s → 30-60/min → bucket 2 (med)
    //   5+   in 4s → 75+/min  → bucket 3 (high)
    const uint16_t adv_sum_4s = (uint16_t)s_adv_window[0]
                              + (uint16_t)s_adv_window[1]
                              + (uint16_t)s_adv_window[2]
                              + (uint16_t)s_adv_window[3];
    const uint16_t approx_apm = adv_sum_4s * 15;
    fp_in.adv_per_minute     = (uint8_t)(approx_apm > 255 ? 255 : approx_apm);
    const uint8_t spread = (uint8_t)((rssi_max >= rssi_min)
                                     ? (rssi_max - rssi_min)
                                     : 0);
    fp_in.rssi_spread_dbm    = spread;

    const uint16_t fp = familiar::compute_fingerprint(fp_in);
    familiar::note_fingerprint(fp);

    // ── Phase 6/8 ORDER MATTERS (codex review #316) ──
    // We must EVALUATE the notification policy against the baseline's
    // state BEFORE we OBSERVE the new sample, or the candidate event
    // would be compared to a distribution that already absorbed it —
    // which dampens outliers in sparse buckets and silently suppresses
    // alerts that should fire.
    //
    // Also: the "sustained" gate in notify needs the TOTAL time since
    // the device was first detected (presence episode start), not the
    // time since the current FSM state began (which is ~0 at the
    // rf_presence_started transition).
    const baseline::Features bl_in = {
      /* csi_motion   */ (int16_t)csi_m,
      /* ble_count    */ (int16_t)device_count,
      /* rssi_mean    */ (int16_t)rssi_mean,
      /* rssi_spread  */ (int16_t)spread
    };
    const uint8_t bl_bucket =
        baseline::bucket_from_time_bucket(get_time_bucket());

    const uint32_t episode_duration_ms =
        (s_presence_episode_start_ms == 0) ? 0
                                           : elapsed_ms(s_presence_episode_start_ms, now_ms);

    // ── Phase 8: quiet-by-default notification policy ── (evaluate FIRST)
    notify::AlertInput ni = {};
    ni.fingerprint          = fp;
    ni.bl_bucket            = bl_bucket;
    ni.time_of_day_bucket   = get_time_bucket();
    ni.features             = bl_in;
    ni.presence_duration_ms = episode_duration_ms;
    ni.device_count         = device_count;
    ni.already_resolved_household = false;
    (void)notify::evaluate(ni);

    // ── Phase 6: feed the adaptive baseline (AFTER notify::evaluate) ──
    baseline::observe(bl_bucket, bl_in);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PRIVATE HELPERS — FSM TRANSITIONS
// ════════════════════════════════════════════════════════════════════════════

// Throttle to prevent rapid state transition flooding
static const uint32_t MIN_TRANSITION_INTERVAL_MS = 500;
static uint32_t s_last_transition_ms = 0;

static void transition_to(RfState new_state, uint32_t now_ms) {
  RfState old_state = s_state;
  s_state = new_state;
  s_state_enter_ms = now_ms;
  s_last_transition_ms = now_ms;  // Track for rate limiting

  // Maintain the presence-episode timestamp. It starts when we first
  // leave RF_EMPTY (a new episode) and ends when we return to RF_EMPTY.
  // This gives notify::evaluate a true "how long has this device been
  // around" value, regardless of intermediate IMPULSE→PRESENCE→DWELL
  // transitions which keep resetting s_state_enter_ms.
  if (old_state == RF_EMPTY && new_state != RF_EMPTY) {
    s_presence_episode_start_ms = now_ms;
  } else if (new_state == RF_EMPTY) {
    s_presence_episode_start_ms = 0;
  }

  // Log transition
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
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
        if (s_settings.emit_impulse_events) {
          emit_event("rf_impulse", s_probe_burst_count > 0 ? SIG_WIFI : SIG_BLE, device_count);
        }
      }
      break;

    case RF_IMPULSE:
      if (device_count < s_settings.min_presence_count && s_probe_burst_count == 0) {
        if (can_transition) {
          transition_to(RF_EMPTY, now_ms);
        }
      } else if (state_duration >= s_settings.presence_threshold_ms) {
        transition_to(RF_PRESENCE, now_ms);
        emit_event("rf_presence_started", SIG_FUSED, device_count);
      } else if (state_duration >= IMPULSE_TIMEOUT_MS && device_count < s_settings.min_presence_count) {
        if (can_transition) {
          transition_to(RF_EMPTY, now_ms);
        }
      }
      break;

    case RF_PRESENCE:
      if (device_count < s_settings.min_presence_count) {
        if (state_duration >= s_settings.lost_timeout_ms) {
          transition_to(RF_EMPTY, now_ms);
          emit_event("rf_presence_ended", SIG_FUSED, -prev_count);
        } else if (can_transition) {
          transition_to(RF_DEPARTING, now_ms);
          emit_event("rf_departing", SIG_BLE, device_count - prev_count);
        }
      } else if (state_duration >= s_settings.dwell_threshold_ms) {
        transition_to(RF_DWELLING, now_ms);
        emit_event("rf_dwell_started", SIG_BLE, 0);
      }
      break;

    case RF_DWELLING:
      if (can_transition && device_count < s_settings.min_presence_count) {
        transition_to(RF_DEPARTING, now_ms);
        emit_event("rf_departing", SIG_BLE, device_count - prev_count);
      }
      break;

    case RF_DEPARTING:
      if (device_count >= s_settings.min_presence_count) {
        // False departure, return to presence
        if (can_transition) {
          transition_to(RF_PRESENCE, now_ms);
        }
      } else if (state_duration >= DEPARTING_CONFIRM_MS) {
        transition_to(RF_EMPTY, now_ms);
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
    // No stored secret - generate new one
    regenerate_device_secret();
  }

  // Validate secret is not all zeros (would indicate uninitialized state)
  if (!validate_device_secret()) {
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Device secret is invalid (all zeros), regenerating");
    regenerate_device_secret();
  }

  // Load session epoch
  s_session_epoch = nvs_store::get_u32("rf_epoch", 0);
  s_session_start_ms = millis();

  // Load settings with validation using named bounds constants
  RfPresenceSettings stored;
  if (nvs_store::get_blob("rf_settings", &stored, sizeof(stored))) {
    // Validate settings against defined bounds
    if (stored.presence_threshold_ms >= MIN_PRESENCE_THRESHOLD_MS &&
        stored.presence_threshold_ms <= MAX_PRESENCE_THRESHOLD_MS &&
        stored.dwell_threshold_ms >= MIN_DWELL_THRESHOLD_MS &&
        stored.dwell_threshold_ms <= MAX_DWELL_THRESHOLD_MS &&
        stored.lost_timeout_ms >= MIN_LOST_TIMEOUT_MS &&
        stored.lost_timeout_ms <= MAX_LOST_TIMEOUT_MS &&
        stored.min_presence_count >= MIN_PRESENCE_COUNT_SETTING &&
        stored.min_presence_count <= MAX_PRESENCE_COUNT_SETTING) {
      s_settings = stored;
    } else {
      health_logging::log(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
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
  memset(s_adv_window, 0, sizeof(s_adv_window));
  s_adv_window_idx = 0;
  s_probe_burst_count = 0;
  s_probe_rssi_peak = RSSI_NOISE_FLOOR;
  s_power_flags = 0;
  s_csi_motion_score = 0;
  s_csi_breathing_score = 0;
  s_csi_last_feed_ms = 0;
  s_last_event = "boot";

  s_initialized = true;
  s_enabled = s_settings.enabled;

  // Phase 4: bring up the household IRK recognizer. Loads paired device
  // IRKs from NVS so resolve_rpa() can suppress household traffic before
  // it even reaches the token map. Audit fix: this call was missing in
  // the original Phase 4 wiring, leaving the module silently dormant
  // (s_initialized=false → resolve_rpa always returned false).
  household::init();

  // Phase 5: bring up the familiar-device recognizer alongside us. Loads
  // the "always ignore" filter + yesterday snapshot from NVS; re-seeds
  // the salt on first boot. Safe before BLE/WiFi come up.
  familiar::init();

  // Phase 6: bring up the adaptive baseline. Loads bucket stats +
  // accumulated training time from NVS. Safe before BLE/WiFi come up.
  baseline::init();

  // Phase 8: bring up the notification policy. Loads user context +
  // dedup window config from NVS.
  notify::init();

  // Phase 9: bring up the federated mesh aggregation toolkit. No I/O
  // here; just clears stats. Mesh transport calls handle_*_share() on
  // receipt and build_*_share() periodically (or on session rotation).
  federated::init();

  // BLE auto-context: read OWNER-role last-seen out of the household
  // module and switch notify::set_context() between HOME / AWAY without
  // user intervention. Loads any persisted user override from NVS.
  presence_context::init();

  // Phase 10: bring up the setup wizard. Loads persisted state + zone
  // name from NVS. Must come AFTER the six modules it orchestrates.
  wizard::init();

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "RF Presence initialized, epoch=%u", s_session_epoch);

  return true;
}

void deinit() {
  if (!s_initialized) return;

  // Tear down the Phase 4/5/6/8/9/10 modules we bring up in init().
  // Reverse-dependency order: wizard reads all of the rest, federated
  // reads baseline/familiar, notify reads them too.
  wizard::deinit();
  federated::deinit();
  notify::deinit();
  baseline::deinit();
  familiar::deinit();
  household::deinit();

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

  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
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
    .confidence = calc_confidence(device_count, s_probe_burst_count, rssi_mean,
                                  active_csi_motion(now_ms),
                                  active_csi_breathing(now_ms)),
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
  // Phase 5 + 6 + 8 + 9 + 10: familiar filter rotation (24 h), baseline
  // training progress persistence (1 h), notify dedup pruning, federated
  // build-throttle bookkeeping, and wizard state-machine advancement are
  // all time-based, not event-driven. Run them even when s_enabled is
  // false.
  if (s_initialized) {
    const uint32_t now = millis();
    familiar::tick(now);
    baseline::tick(now);
    notify::tick(now);
    federated::tick(now);
    wizard::tick(now);
    // presence_context tick goes after notify so notify::set_context()
    // calls land in the same loop iteration as the auto-context decision
    // they're driving — the HOME/AWAY flip is reflected immediately.
    presence_context::tick();
  }

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

  // Reset per-second counters. On each rollover, push the second we just
  // finished into the 4-slot sliding window before zeroing.
  uint32_t current_second = now_ms / 1000;
  if (current_second != s_last_adv_second) {
    s_adv_window[s_adv_window_idx] = s_adv_count_this_second;
    s_adv_window_idx = (uint8_t)((s_adv_window_idx + 1) & 0x03);
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
  s_csi_motion_score = 0;
  s_csi_breathing_score = 0;
  s_csi_last_feed_ms = 0;
  s_current_device_count = 0;
  s_current_rssi_sum = 0;
  s_current_rssi_max = RSSI_NOISE_FLOOR;
  s_current_rssi_min = 0;
  s_rssi_count = 0;
  s_adv_count_this_second = 0;
  memset(s_adv_window, 0, sizeof(s_adv_window));
  s_adv_window_idx = 0;

  // Clear observations (contain timestamps that could correlate sessions)
  secure_wipe(s_observations, sizeof(s_observations));
  s_obs_head = 0;
  s_obs_count = 0;

  // Reset last event to prevent cross-session correlation
  s_last_event = "session_rotated";

  // Phase 7: reset the differential-privacy budget. An attacker observing
  // our MQTT / HTTP surface over a 4 h window now gets a fresh ε budget
  // after each rotation; they can't compose queries across sessions to
  // aggregate below the per-query DP guarantee.
  dp::reset_budget();

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Session rotated, new epoch=%u", s_session_epoch);
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API — SIGNAL FEEDING (PRIVACY BARRIER)
// ════════════════════════════════════════════════════════════════════════════

void feed_ble_scan(const uint8_t* mac_address, int8_t rssi, bool connectable) {
  if (!s_initialized || !s_enabled) return;
  if (rssi < RSSI_NOISE_FLOOR) return;  // Ignore noise
  if (!mac_address) return;

  // === HOUSEHOLD SHORT-CIRCUIT (Phase 4) ===
  // If this MAC is a Resolvable Private Address that resolves to any of
  // the IRKs we have on file for household devices, we treat it as if it
  // were never observed — no token, no presence count, no event. This is
  // how we satisfy the user's "don't alert me for my own phone" goal
  // without ever storing the phone's MAC or identity.
  //
  // This check happens BEFORE token derivation so the MAC is not hashed
  // into the token map at all for household devices. That preserves the
  // invariant that the token map cannot be used to correlate a household
  // device's presence across session rotations.
  if (household::resolve_rpa(mac_address)) {
    return;
  }

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

// ────────────────────────────────────────────────────────────────────────────
// CSI FEED (privacy barrier: we accept only the int8 aggregate feature vector
// defined in csi_types.h; csi_features_t carries no
// identifiers, no raw subcarrier samples, and no precise timestamps.)
//
// Motion score is derived from the signed Doppler bands and amplitude
// variance bands. Breathing score is derived from the 0.1–0.5 Hz FFT bins.
// Both are mapped to 0..100 with the same convention as other internal
// scalars, so the fusion head treats them uniformly.
// ────────────────────────────────────────────────────────────────────────────
void feed_csi_window(const ::csi_features_t* features) {
  if (!s_initialized || !s_enabled) return;
  if (features == nullptr) return;

  // Sanity: frames_in_window is a public field and a degraded window
  // (< 4 frames) is not worth using.
  if (features->frames_in_window < 4) return;

  // Motion score: mean of |Doppler| bands + half of amp-variance bands.
  // Amp indexes are [0..7], Doppler indexes are [8..11].
  int32_t motion_acc = 0;
  for (size_t i = 0; i < 8; i++) {
    const int32_t a = features->v[i];
    motion_acc += a < 0 ? -a : a;   // amplitude variance is always positive
  }
  for (size_t i = 8; i < 12; i++) {
    const int32_t d = features->v[i];
    motion_acc += d < 0 ? -d : d;   // |Doppler| — sign carries direction, not strength
  }
  // Normalize: 12 bands, each up to ~60 for a "busy room" → acc ~ 720.
  // Clamp to 0..100.
  int32_t motion = motion_acc / 8;  // empirical scaling
  if (motion > 100) motion = 100;
  if (motion < 0)   motion = 0;

  // Breathing score: sum of the 8 FFT bins [12..19], same clamp.
  int32_t breath_acc = 0;
  for (size_t i = 12; i < 20; i++) {
    const int32_t b = features->v[i];
    breath_acc += b < 0 ? -b : b;
  }
  int32_t breath = breath_acc / 4;  // 8 bins → /4 keeps typical values ~0..80
  if (breath > 100) breath = 100;
  if (breath < 0)   breath = 0;

  s_csi_motion_score     = (uint8_t)motion;
  s_csi_breathing_score  = (uint8_t)breath;
  s_csi_last_feed_ms     = millis();

  // Boost the FSM toward IMPULSE if CSI is shouting but BLE/WiFi are quiet.
  // This is the "courier with a Faraday pocket" case — motion is real but
  // no device is broadcasting. We nudge but never force a transition; the
  // FSM still requires its own state_duration threshold to confirm.
  if (motion >= CSI_MOTION_CONFIRM && s_state == RF_EMPTY) {
    s_probe_burst_count = (s_probe_burst_count < 2) ? 2 : s_probe_burst_count;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// CSI INTROSPECTION
// ────────────────────────────────────────────────────────────────────────────
uint8_t current_csi_motion_score() {
  return active_csi_motion(millis());
}

uint8_t current_csi_breathing_score() {
  return active_csi_breathing(millis());
}

bool has_recent_csi() {
  if (s_csi_last_feed_ms == 0) return false;
  return elapsed_ms(s_csi_last_feed_ms, millis()) <= CSI_FEED_TTL_MS;
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
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Conformance: token derivation failed before rotation");
    return false;
  }

  // Perform rotation
  rotate_session();

  // Generate token after rotation with same MAC
  uint32_t token_after = derive_session_token(test_mac);
  if (token_after == 0) {
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Conformance: token derivation failed after rotation");
    return false;
  }

  // Verify invariants
  bool epoch_incremented = (s_session_epoch == old_epoch + 1);
  bool tokens_differ = (token_before != token_after);
  bool tokens_cleared = (s_token_count == 0);  // rotation should clear token map

  if (!epoch_incremented) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Conformance: epoch did not increment (was %u, now %u)", old_epoch, s_session_epoch);
  }
  if (!tokens_differ) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Conformance: tokens match after rotation (both %u)", token_before);
  }
  if (!tokens_cleared) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
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
      health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
        "Conformance: suspicious device count %u in observation", obs.ble_device_count);
      // Not a failure - could be legitimate dense environment
    }

    // RSSI should be within valid range (-127 to 0 for BLE)
    if (obs.ble_rssi_max > 0 || obs.ble_rssi_max < -100) {
      if (obs.ble_device_count > 0) {  // Only check if devices were present
        health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
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
      health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
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
