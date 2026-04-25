/*
 * SecuraCV Canary — Quiet-by-default notification policy (Phase 8)
 * Version 0.1.0
 *
 * The whole stack up to this point (household IRK, familiar Bloom,
 * 72 h adaptive baseline, DP noise on exports) exists so that *this*
 * module can answer one question:
 *
 *     Does this arrival deserve to wake the user?
 *
 * Our answer is almost always **no**. The default is silence.
 *
 * FIVE-WAY FILTER (all must clear for an alert to fire)
 * =====================================================
 *   1. Anomaly — baseline::is_anomaly(bucket, features) must be true,
 *      OR the presence must be sustained long enough to bypass baseline.
 *   2. Not household — if household::resolve_rpa matched upstream in
 *      rf_presence, the event never reaches us. We also re-check here
 *      as a defense-in-depth audit.
 *   3. Not ambient — the event's fingerprint must NOT appear in the
 *      familiar recognizer's "yesterday" filter.
 *   4. Not always-ignored — the fingerprint must NOT appear in the
 *      user's persistent "ignore this pattern" filter.
 *   5. Not a dedup echo — we must not have fired on the same dedup key
 *      within the last dedup_window_ms (default 15 minutes).
 *   6. Severity passes context gate — e.g., "Quiet Hours" only lets
 *      HIGH or CRITICAL through.
 *
 * NATURAL-LANGUAGE REASON
 * =======================
 * Every decision carries a short, human-readable explanation suitable
 * for direct display: *"Unknown presence, unusual hour, 8 min sustained,
 * high anomaly."* Strings are bounded at REASON_MAX chars and composed
 * from a fixed vocabulary — we never embed a MAC, a token, or a label.
 *
 * OUTPUT
 * ======
 * Phase 8 only DECIDES; it does not DELIVER. Actual notification delivery
 * (MQTT publish, HTTP push, iOS APNS webhook, audible buzzer) lands in
 * Phase 10's wizard / delivery layer. For now the decision is logged via
 * health_logging at LEVEL_WARNING if fired, LEVEL_DEBUG if suppressed,
 * and get_last_decision() can be polled by a future consumer.
 */

#ifndef SECURACV_NOTIFY_H
#define SECURACV_NOTIFY_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "baseline.h"   // baseline::Features

namespace notify {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Max length of the natural-language reason string (including NUL).
static const size_t REASON_MAX = 160;

// Dedup ring capacity. Number of distinct (dedup_key) entries we track
// for the "don't fire same alert twice in 15 min" rule. Fixed storage.
static const size_t DEDUP_RING_CAP = 32;

// Default dedup window. 15 minutes matches the Apple Exposure Notification
// Rolling Proximity Identifier refresh cadence — a natural unit for
// "same device, same spot" alert de-duplication.
static const uint32_t DEFAULT_DEDUP_WINDOW_MS = 15UL * 60UL * 1000UL;

// Minimum sustained presence before we consider firing a non-anomaly
// alert. Anything shorter is most likely a passer-by.
static const uint32_t MIN_SUSTAINED_MS = 90UL * 1000UL;  // 90 seconds

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// User's current context. Persisted to NVS.
enum Context : uint8_t {
  CTX_HOME        = 0,   // someone's home — minimize alerts
  CTX_AWAY        = 1,   // house is empty — alert on anything new
  CTX_QUIET_HOURS = 2,   // night / focus mode — suppress below HIGH
  CTX_TRAVELING   = 3,   // extended away — highest sensitivity
};

enum Severity : uint8_t {
  SEV_INFO     = 0,
  SEV_LOW      = 1,
  SEV_MEDIUM   = 2,
  SEV_HIGH     = 3,
  SEV_CRITICAL = 4,
};

// Reasons the policy may suppress an alert (mutually distinguishable
// so the user can be told why).
enum SuppressReason : uint8_t {
  SUP_NONE              = 0,  // not suppressed
  SUP_HOUSEHOLD         = 1,
  SUP_AMBIENT           = 2,
  SUP_ALWAYS_IGNORED    = 3,
  SUP_DEDUP             = 4,
  SUP_CONTEXT_TOO_QUIET = 5,
  SUP_SEVERITY_TOO_LOW  = 6,
  SUP_TRANSIENT         = 7,  // too brief to matter
};

// Inputs the caller composes from rf_presence + sibling modules.
// We do not query those modules internally — keeps this module unit-testable
// and keeps the caller in charge of defining what "current event" means.
struct AlertInput {
  uint16_t                  fingerprint;           // familiar::compute_fingerprint output
  uint8_t                   bl_bucket;             // baseline::bucket_from_time_bucket
  uint8_t                   time_of_day_bucket;    // rf_presence::get_time_bucket (10-min)
  baseline::Features        features;
  uint32_t                  presence_duration_ms;  // time in RF_PRESENCE so far
  uint8_t                   device_count;
  bool                      already_resolved_household;  // if true, we suppress immediately
};

// The policy's verdict for a single event.
struct AlertDecision {
  bool            fired;
  Severity        severity;
  SuppressReason  suppress_reason;  // SUP_NONE if fired
  uint16_t        dedup_key;        // stable hash of (fingerprint ⊕ bucket)
  char            reason[REASON_MAX];
};

struct Stats {
  uint32_t total_evaluated;
  uint32_t total_fired;
  uint32_t total_suppressed_household;
  uint32_t total_suppressed_ambient;
  uint32_t total_suppressed_always_ignored;
  uint32_t total_suppressed_dedup;
  uint32_t total_suppressed_context;
  uint32_t total_suppressed_severity;
  uint32_t total_suppressed_transient;
  Context  current_context;
  uint32_t dedup_window_ms;
};

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

bool init();
void deinit();

// ════════════════════════════════════════════════════════════════════════════
// EVALUATE (hot path — called from rf_presence on each presence_started)
// ════════════════════════════════════════════════════════════════════════════

// Run the 5-way filter. Returns a fully-populated AlertDecision. On fire,
// the decision is also logged via health_logging at LEVEL_WARNING, so any
// future consumer subscribed to the health log can react even without
// polling get_last_decision().
AlertDecision evaluate(const AlertInput& in);

// Main-loop tick. Ages out dedup entries; no-op otherwise.
void tick(uint32_t now_ms);

// ════════════════════════════════════════════════════════════════════════════
// CONTEXT
// ════════════════════════════════════════════════════════════════════════════

bool    set_context(Context c);        // persists to NVS
Context get_context();

// ════════════════════════════════════════════════════════════════════════════
// DEDUP WINDOW
// ════════════════════════════════════════════════════════════════════════════

bool     set_dedup_window_ms(uint32_t ms);
uint32_t get_dedup_window_ms();

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION
// ════════════════════════════════════════════════════════════════════════════

bool get_stats(Stats* out);
bool get_stats_for_export(Stats* out);  // DP-noised counters

// Fetch the most recent decision for diagnostics. Returns false if the
// module has never evaluated an event.
bool get_last_decision(AlertDecision* out);

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// End-to-end self-test: exercises each suppression branch + a fire path
// against synthetic inputs, verifies reasons are populated. Restores all
// state + stats on exit.
bool conformance_self_test();

}  // namespace notify

#endif  // SECURACV_NOTIFY_H
