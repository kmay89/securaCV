/*
 * SecuraCV Canary — Quiet-by-default notification policy — Implementation
 *
 * See notify.h for the design. Implementation notes:
 *   • Dedup is a tiny ring of (key, fired_at_ms). On each evaluate() we
 *     scan the ring linearly — 32 entries is a handful of L1 hits on S3
 *     so a sorted/hashed structure would be overkill.
 *   • Reason strings are composed from a fixed vocabulary via snprintf.
 *     We deliberately include NO variable content besides numbers (time,
 *     duration, feature values). No MAC, no label, no raw RSSI sample.
 *   • Severity is classified from the combination of anomaly signal and
 *     sustained-ness. Dwelling + anomalous → HIGH. Anomaly only → MEDIUM.
 *     Sustained without anomaly → LOW. Everything else → INFO.
 */

#include "notify.h"
#include "familiar.h"
#include "household.h"   // for the audit re-check
#include "baseline.h"
#include "dp.h"
#include "nvs_store.h"
#include "health_log.h"

#include <string.h>
#include <stdio.h>
#include <atomic>

namespace notify {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool s_initialized = false;

struct DedupEntry {
  uint16_t key;
  uint32_t fired_at_ms;  // 0 = slot empty
};
static DedupEntry s_dedup[DEDUP_RING_CAP];

static Context  s_context = CTX_HOME;
static uint32_t s_dedup_window_ms = DEFAULT_DEDUP_WINDOW_MS;

// Stats (in-RAM)
static uint32_t s_total_evaluated = 0;
static uint32_t s_total_fired = 0;
static uint32_t s_total_suppressed_household = 0;
static uint32_t s_total_suppressed_ambient = 0;
static uint32_t s_total_suppressed_always_ignored = 0;
static uint32_t s_total_suppressed_dedup = 0;
static uint32_t s_total_suppressed_context = 0;
static uint32_t s_total_suppressed_severity = 0;
static uint32_t s_total_suppressed_transient = 0;

// Last decision — double-buffered so a polling consumer on a different
// task (web server, MQTT handler) can never observe a torn write. The
// writer fills the INACTIVE buffer, then atomically flips s_last_idx
// (a single-byte store); the reader captures the index first and then
// copies the buffer it pointed to. AlertDecision is ~170 B (bounded by
// reason[REASON_MAX]), so non-atomic copy without this pattern risks
// string corruption — gemini review #316.
static std::atomic<bool> s_have_last{false};
static AlertDecision     s_last_buf[2] = {};
static std::atomic<uint8_t> s_last_idx{0};

// ────────────────────────────────────────────────────────────────────────────
// NVS KEYS
// ────────────────────────────────────────────────────────────────────────────

static const char* NVS_KEY_CONTEXT       = "nf_ctx";
static const char* NVS_KEY_DEDUP_WINDOW  = "nf_dedup";

// Publish a decision to the double-buffered last-decision slot. Lock-
// free: the writer fills the INACTIVE buffer in full, then atomically
// flips the index. A concurrent reader captures the index BEFORE
// copying, so it either sees the previous snapshot or the new one —
// never a torn half.
static void publish_last(const AlertDecision& d) {
  const uint8_t write_idx = (uint8_t)(s_last_idx.load(std::memory_order_relaxed) ^ 1);
  s_last_buf[write_idx] = d;  // full struct copy into the inactive buffer
  s_last_idx.store(write_idx, std::memory_order_release);
  s_have_last.store(true, std::memory_order_release);
}

// ────────────────────────────────────────────────────────────────────────────
// DEDUP RING
// ────────────────────────────────────────────────────────────────────────────

static inline uint32_t elapsed_ms(uint32_t start, uint32_t now) {
  return now - start;  /* unsigned sub → wrap-safe */
}

// Return true if key was fired within dedup_window_ms of now.
static bool dedup_hit(uint16_t key, uint32_t now_ms) {
  for (size_t i = 0; i < DEDUP_RING_CAP; i++) {
    if (s_dedup[i].key != key || s_dedup[i].fired_at_ms == 0) continue;
    if (elapsed_ms(s_dedup[i].fired_at_ms, now_ms) < s_dedup_window_ms) {
      return true;
    }
  }
  return false;
}

// Record a fired event in the dedup ring. Prefers an empty slot; falls
// back to the oldest occupied slot if full.
static void dedup_record(uint16_t key, uint32_t now_ms) {
  size_t oldest_idx = 0;
  uint32_t oldest_age = 0;
  for (size_t i = 0; i < DEDUP_RING_CAP; i++) {
    if (s_dedup[i].fired_at_ms == 0) {
      s_dedup[i].key = key;
      s_dedup[i].fired_at_ms = now_ms ? now_ms : 1;  // never 0
      return;
    }
    const uint32_t age = elapsed_ms(s_dedup[i].fired_at_ms, now_ms);
    if (age > oldest_age) {
      oldest_age = age;
      oldest_idx = i;
    }
  }
  s_dedup[oldest_idx].key = key;
  s_dedup[oldest_idx].fired_at_ms = now_ms ? now_ms : 1;
}

static void dedup_prune(uint32_t now_ms) {
  for (size_t i = 0; i < DEDUP_RING_CAP; i++) {
    if (s_dedup[i].fired_at_ms == 0) continue;
    if (elapsed_ms(s_dedup[i].fired_at_ms, now_ms) >= s_dedup_window_ms) {
      s_dedup[i].key = 0;
      s_dedup[i].fired_at_ms = 0;
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// SEVERITY CLASSIFICATION
// ────────────────────────────────────────────────────────────────────────────

static Severity classify(const AlertInput& in, bool anomaly, bool sustained) {
  if (anomaly && sustained && in.device_count >= 2) return SEV_CRITICAL;
  if (anomaly && sustained)                         return SEV_HIGH;
  if (anomaly)                                      return SEV_MEDIUM;
  if (sustained)                                    return SEV_LOW;
  return SEV_INFO;
}

// Context gate: the minimum severity that fires in this context.
static Severity min_severity_for_context(Context c) {
  switch (c) {
    case CTX_TRAVELING:   return SEV_LOW;      // most sensitive
    case CTX_AWAY:        return SEV_LOW;
    case CTX_HOME:        return SEV_MEDIUM;   // only real anomalies
    case CTX_QUIET_HOURS: return SEV_HIGH;     // night mode
    default:              return SEV_MEDIUM;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// REASON STRING COMPOSITION
// ────────────────────────────────────────────────────────────────────────────

// Coarse "hour of day" from 10-min bucket (0..143). Returns 0..23.
static inline uint8_t hour_of_day_from_bucket(uint8_t b) {
  return (uint8_t)((b / 6) % 24);
}

// Returns a short qualifier for the hour — "morning", "afternoon", etc.
static const char* hour_qualifier(uint8_t hour) {
  if (hour < 5)   return "very late";
  if (hour < 8)   return "early morning";
  if (hour < 12)  return "morning";
  if (hour < 17)  return "afternoon";
  if (hour < 21)  return "evening";
  return "late night";
}

static const char* severity_name(Severity s) {
  switch (s) {
    case SEV_INFO:     return "info";
    case SEV_LOW:      return "low";
    case SEV_MEDIUM:   return "medium";
    case SEV_HIGH:     return "high";
    case SEV_CRITICAL: return "critical";
  }
  return "unknown";
}

static void build_fire_reason(char* out, size_t cap,
                              const AlertInput& in,
                              bool anomaly, bool sustained,
                              Severity sev) {
  const uint8_t hour = hour_of_day_from_bucket(in.time_of_day_bucket);
  const uint32_t dur_min = in.presence_duration_ms / 60000UL;
  const uint32_t dur_sec = (in.presence_duration_ms / 1000UL) % 60UL;

  const char* signal_kind = anomaly
      ? (sustained ? "sustained unusual presence" : "unusual presence")
      : (sustained ? "sustained presence"         : "brief presence");

  // Include duration only if ≥ 30 s (matches our SEV_LOW gate roughly);
  // otherwise "brief" is more honest than "0 min 3 sec".
  if (in.presence_duration_ms >= 30UL * 1000UL) {
    snprintf(out, cap,
      "%s in %s (hour %u), ~%um%02us, %u device%s, %s",
      signal_kind, hour_qualifier(hour), (unsigned)hour,
      (unsigned)dur_min, (unsigned)dur_sec,
      (unsigned)in.device_count, in.device_count == 1 ? "" : "s",
      severity_name(sev));
  } else {
    snprintf(out, cap,
      "%s in %s (hour %u), %u device%s, %s",
      signal_kind, hour_qualifier(hour), (unsigned)hour,
      (unsigned)in.device_count, in.device_count == 1 ? "" : "s",
      severity_name(sev));
  }
}

static void build_suppress_reason(char* out, size_t cap,
                                  const AlertInput& in,
                                  SuppressReason r) {
  const uint8_t hour = hour_of_day_from_bucket(in.time_of_day_bucket);
  const char* tag = "suppressed";
  switch (r) {
    case SUP_HOUSEHOLD:         tag = "household device, suppressed"; break;
    case SUP_AMBIENT:           tag = "matches yesterday's pattern, suppressed"; break;
    case SUP_ALWAYS_IGNORED:    tag = "user-ignored pattern, suppressed"; break;
    case SUP_DEDUP:             tag = "recent duplicate, suppressed"; break;
    case SUP_CONTEXT_TOO_QUIET: tag = "context gates it, suppressed"; break;
    case SUP_SEVERITY_TOO_LOW:  tag = "below severity gate, suppressed"; break;
    case SUP_TRANSIENT:         tag = "transient pass-by, suppressed"; break;
    case SUP_NONE:              tag = "fired";  // shouldn't reach here
      break;
  }
  snprintf(out, cap, "%s (hour %u)", tag, (unsigned)hour);
}

// ────────────────────────────────────────────────────────────────────────────
// DEDUP KEY
// ────────────────────────────────────────────────────────────────────────────

// Stable hash of (fingerprint, baseline bucket) so the same device at
// the same time-of-day bucket produces the same dedup key across events.
// Uses Knuth multiplicative hash; takes the HIGH 16 bits of the product
// (low bits of a multiplicative hash carry less entropy and are
// essentially just an LCG step — gemini review #316).
static uint16_t compute_dedup_key(const AlertInput& in) {
  const uint32_t h = (uint32_t)in.fingerprint * 2654435761U;
  return (uint16_t)((h >> 16) ^ ((uint32_t)in.bl_bucket << 8));
}

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;

  memset(s_dedup, 0, sizeof(s_dedup));
  s_context         = (Context)nvs_store::get_u32(NVS_KEY_CONTEXT, (uint32_t)CTX_HOME);
  s_dedup_window_ms = nvs_store::get_u32(NVS_KEY_DEDUP_WINDOW, DEFAULT_DEDUP_WINDOW_MS);

  // Sanity-clamp persisted context.
  if (s_context > CTX_TRAVELING) s_context = CTX_HOME;

  s_total_evaluated = 0;
  s_total_fired = 0;
  s_total_suppressed_household = 0;
  s_total_suppressed_ambient = 0;
  s_total_suppressed_always_ignored = 0;
  s_total_suppressed_dedup = 0;
  s_total_suppressed_context = 0;
  s_total_suppressed_severity = 0;
  s_total_suppressed_transient = 0;
  s_have_last.store(false, std::memory_order_relaxed);
  s_last_idx.store(0, std::memory_order_relaxed);

  s_initialized = true;
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Notify: init (context=%u, dedup_window=%ums)",
    (unsigned)s_context, (unsigned)s_dedup_window_ms);
  return true;
}

void deinit() {
  if (!s_initialized) return;
  memset(s_dedup, 0, sizeof(s_dedup));
  memset(s_last_buf, 0, sizeof(s_last_buf));
  s_have_last.store(false, std::memory_order_relaxed);
  s_last_idx.store(0, std::memory_order_relaxed);
  s_initialized = false;
}

// ────────────────────────────────────────────────────────────────────────────
// EVALUATE
// ────────────────────────────────────────────────────────────────────────────

AlertDecision evaluate(const AlertInput& in) {
  AlertDecision d = {};
  d.dedup_key   = compute_dedup_key(in);
  d.fingerprint = in.fingerprint;  // forward so wizard / UI can hash it

  if (!s_initialized) return d;

  s_total_evaluated++;
  const uint32_t now_ms = millis();

  // Defense-in-depth: if the caller says household already resolved, or
  // if we can re-resolve a recent household event ourselves, drop. This
  // duplicates rf_presence's upstream short-circuit; it's cheap and
  // closes a window where future callers might forget the upstream check.
  if (in.already_resolved_household) {
    d.suppress_reason = SUP_HOUSEHOLD;
    s_total_suppressed_household++;
    build_suppress_reason(d.reason, REASON_MAX, in, SUP_HOUSEHOLD);
    publish_last(d);
    return d;
  }

  // 1. Transient filter. Fewer than MIN_SUSTAINED_MS of presence AND
  //    no baseline anomaly = passer-by, never fire.
  const bool anomaly = baseline::is_anomaly(in.bl_bucket, in.features);
  const bool sustained = in.presence_duration_ms >= MIN_SUSTAINED_MS;

  if (!anomaly && !sustained) {
    d.suppress_reason = SUP_TRANSIENT;
    s_total_suppressed_transient++;
    build_suppress_reason(d.reason, REASON_MAX, in, SUP_TRANSIENT);
    publish_last(d);
    return d;
  }

  // 2. Ambient fingerprint match.
  if (familiar::is_ambient(in.fingerprint)) {
    d.suppress_reason = SUP_AMBIENT;
    s_total_suppressed_ambient++;
    build_suppress_reason(d.reason, REASON_MAX, in, SUP_AMBIENT);
    publish_last(d);
    return d;
  }

  // 3. Always-ignore (user-confirmed pattern).
  if (familiar::is_always_ignored(in.fingerprint)) {
    d.suppress_reason = SUP_ALWAYS_IGNORED;
    s_total_suppressed_always_ignored++;
    build_suppress_reason(d.reason, REASON_MAX, in, SUP_ALWAYS_IGNORED);
    publish_last(d);
    return d;
  }

  // 4. Dedup window.
  if (dedup_hit(d.dedup_key, now_ms)) {
    d.suppress_reason = SUP_DEDUP;
    s_total_suppressed_dedup++;
    build_suppress_reason(d.reason, REASON_MAX, in, SUP_DEDUP);
    publish_last(d);
    return d;
  }

  // 5. Classify severity. Compose the fire-reason string.
  const Severity sev = classify(in, anomaly, sustained);

  // 6. Context gate: severity must clear the context's minimum.
  const Severity min_sev = min_severity_for_context(s_context);
  if ((uint8_t)sev < (uint8_t)min_sev) {
    d.severity = sev;
    d.suppress_reason = (s_context == CTX_QUIET_HOURS || s_context == CTX_HOME)
                        ? SUP_CONTEXT_TOO_QUIET
                        : SUP_SEVERITY_TOO_LOW;
    if (d.suppress_reason == SUP_CONTEXT_TOO_QUIET) s_total_suppressed_context++;
    else                                            s_total_suppressed_severity++;
    build_suppress_reason(d.reason, REASON_MAX, in, d.suppress_reason);
    publish_last(d);
    return d;
  }

  // Cleared all gates — fire.
  d.fired = true;
  d.severity = sev;
  d.suppress_reason = SUP_NONE;
  build_fire_reason(d.reason, REASON_MAX, in, anomaly, sustained, sev);
  dedup_record(d.dedup_key, now_ms);
  s_total_fired++;

  health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
    "Notify FIRE [%s] %s", severity_name(sev), d.reason);

  publish_last(d);
  return d;
}

void tick(uint32_t now_ms) {
  if (!s_initialized) return;
  // Prune expired dedup entries; cheap — O(DEDUP_RING_CAP).
  dedup_prune(now_ms);
}

// ────────────────────────────────────────────────────────────────────────────
// CONTEXT + DEDUP WINDOW
// ────────────────────────────────────────────────────────────────────────────

bool set_context(Context c) {
  if (!s_initialized) return false;
  if (c > CTX_TRAVELING) return false;
  s_context = c;
  nvs_store::set_u32(NVS_KEY_CONTEXT, (uint32_t)c);
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Notify: context → %u", (unsigned)c);
  return true;
}

Context get_context() { return s_context; }

bool set_dedup_window_ms(uint32_t ms) {
  if (!s_initialized) return false;
  // Sanity bounds: 1 s minimum, 1 h maximum.
  if (ms < 1000UL)                 ms = 1000UL;
  if (ms > 60UL * 60UL * 1000UL)   ms = 60UL * 60UL * 1000UL;
  s_dedup_window_ms = ms;
  nvs_store::set_u32(NVS_KEY_DEDUP_WINDOW, ms);
  return true;
}

uint32_t get_dedup_window_ms() { return s_dedup_window_ms; }

// ────────────────────────────────────────────────────────────────────────────
// INTROSPECTION
// ────────────────────────────────────────────────────────────────────────────

bool get_stats(Stats* out) {
  if (!out) return false;
  out->total_evaluated                 = s_total_evaluated;
  out->total_fired                     = s_total_fired;
  out->total_suppressed_household      = s_total_suppressed_household;
  out->total_suppressed_ambient        = s_total_suppressed_ambient;
  out->total_suppressed_always_ignored = s_total_suppressed_always_ignored;
  out->total_suppressed_dedup          = s_total_suppressed_dedup;
  out->total_suppressed_context        = s_total_suppressed_context;
  out->total_suppressed_severity       = s_total_suppressed_severity;
  out->total_suppressed_transient      = s_total_suppressed_transient;
  out->current_context                 = s_context;
  out->dedup_window_ms                 = s_dedup_window_ms;
  return true;
}

bool get_stats_for_export(Stats* out) {
  if (!get_stats(out)) return false;
  out->total_evaluated                 = dp::noisy_u32(out->total_evaluated,                 1);
  out->total_fired                     = dp::noisy_u32(out->total_fired,                     1);
  out->total_suppressed_household      = dp::noisy_u32(out->total_suppressed_household,      1);
  out->total_suppressed_ambient        = dp::noisy_u32(out->total_suppressed_ambient,        1);
  out->total_suppressed_always_ignored = dp::noisy_u32(out->total_suppressed_always_ignored, 1);
  out->total_suppressed_dedup          = dp::noisy_u32(out->total_suppressed_dedup,          1);
  out->total_suppressed_context        = dp::noisy_u32(out->total_suppressed_context,        1);
  out->total_suppressed_severity       = dp::noisy_u32(out->total_suppressed_severity,       1);
  out->total_suppressed_transient      = dp::noisy_u32(out->total_suppressed_transient,      1);
  // current_context + dedup_window_ms are user-set; don't noise.
  return true;
}

bool get_last_decision(AlertDecision* out) {
  if (!s_initialized || !out) return false;
  if (!s_have_last.load(std::memory_order_acquire)) return false;
  // Snapshot the active buffer index, then copy. If the writer flips
  // mid-copy, we still finish reading a self-consistent buffer because
  // the writer only writes into the INACTIVE buffer.
  const uint8_t idx = s_last_idx.load(std::memory_order_acquire);
  *out = s_last_buf[idx];
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  if (!s_initialized) return false;

  // Save state so the test doesn't pollute dedup/stats.
  DedupEntry saved_dedup[DEDUP_RING_CAP];
  memcpy(saved_dedup, s_dedup, sizeof(s_dedup));
  const Context saved_ctx  = s_context;
  const uint32_t saved_dw  = s_dedup_window_ms;
  const uint32_t st_eval = s_total_evaluated;
  const uint32_t st_fire = s_total_fired;
  const uint32_t st_hh = s_total_suppressed_household;
  const uint32_t st_am = s_total_suppressed_ambient;
  const uint32_t st_ai = s_total_suppressed_always_ignored;
  const uint32_t st_dd = s_total_suppressed_dedup;
  const uint32_t st_cx = s_total_suppressed_context;
  const uint32_t st_sv = s_total_suppressed_severity;
  const uint32_t st_tr = s_total_suppressed_transient;
  const bool saved_have = s_have_last.load(std::memory_order_relaxed);
  const uint8_t saved_idx = s_last_idx.load(std::memory_order_relaxed);
  AlertDecision saved_last_buf[2];
  memcpy(saved_last_buf, s_last_buf, sizeof(saved_last_buf));

  memset(s_dedup, 0, sizeof(s_dedup));
  s_context = CTX_AWAY;  // wide gate so severity isn't a bottleneck

  // Build a benign input.
  AlertInput base = {};
  base.fingerprint          = 0x123;
  base.bl_bucket            = 5;
  base.time_of_day_bucket   = 30;  // hour 5 (early morning)
  base.features             = { 60, 3, -55, 10 };
  base.presence_duration_ms = 120UL * 1000UL;  // 2 min → sustained
  base.device_count         = 1;
  base.already_resolved_household = false;

  // 1. Household short-circuit fires.
  AlertInput i1 = base;
  i1.already_resolved_household = true;
  AlertDecision d1 = evaluate(i1);
  const bool ok1 = !d1.fired && d1.suppress_reason == SUP_HOUSEHOLD && d1.reason[0] != '\0';

  // 2. Transient (short duration, no anomaly hits) → SUP_TRANSIENT.
  AlertInput i2 = base;
  i2.presence_duration_ms = 10UL * 1000UL;
  AlertDecision d2 = evaluate(i2);
  const bool ok2 = !d2.fired && d2.suppress_reason == SUP_TRANSIENT;

  // 3. A sustained event that doesn't hit household/ambient/always-ignored/
  //    dedup should fire (in CTX_AWAY, SEV_LOW clears the gate).
  AlertInput i3 = base;
  i3.fingerprint = 0x456;  // distinct from prior dedup keys
  AlertDecision d3 = evaluate(i3);
  const bool ok3 = d3.fired && d3.suppress_reason == SUP_NONE && d3.reason[0] != '\0';

  // 4. Re-evaluate the SAME fingerprint/bucket → dedup blocks it.
  AlertDecision d4 = evaluate(i3);
  const bool ok4 = !d4.fired && d4.suppress_reason == SUP_DEDUP;

  // Restore state + stats.
  memcpy(s_dedup, saved_dedup, sizeof(s_dedup));
  s_context = saved_ctx;
  s_dedup_window_ms = saved_dw;
  s_total_evaluated = st_eval;
  s_total_fired = st_fire;
  s_total_suppressed_household = st_hh;
  s_total_suppressed_ambient = st_am;
  s_total_suppressed_always_ignored = st_ai;
  s_total_suppressed_dedup = st_dd;
  s_total_suppressed_context = st_cx;
  s_total_suppressed_severity = st_sv;
  s_total_suppressed_transient = st_tr;
  memcpy(s_last_buf, saved_last_buf, sizeof(s_last_buf));
  s_last_idx.store(saved_idx, std::memory_order_relaxed);
  s_have_last.store(saved_have, std::memory_order_relaxed);

  const bool ok = ok1 && ok2 && ok3 && ok4;
  if (!ok) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Notify self-test FAIL: 1=%d 2=%d 3=%d 4=%d",
      (int)ok1, (int)ok2, (int)ok3, (int)ok4);
  } else {
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Notify self-test: OK");
  }
  return ok;
}

}  // namespace notify
