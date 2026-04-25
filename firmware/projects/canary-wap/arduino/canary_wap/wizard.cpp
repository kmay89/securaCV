/*
 * SecuraCV Canary — Setup wizard + orchestration — Implementation
 *
 * See wizard.h for the design. Thin coordinator over the six Phase 4-9
 * sibling modules; keeps state in four fields + NVS.
 */

#include "wizard.h"

#include "household.h"
#include "familiar.h"
#include "baseline.h"
#include "notify.h"
#include "federated.h"
#include "dp.h"
#include "nvs_store.h"
#include "health_log.h"

#include <string.h>

namespace wizard {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool        s_initialized = false;
static SetupState  s_state = WIZ_UNCONFIGURED;
static char        s_zone[MAX_ZONE_NAME_LEN] = {0};

// ────────────────────────────────────────────────────────────────────────────
// NVS KEYS
// ────────────────────────────────────────────────────────────────────────────

static const char* NVS_KEY_STATE = "wz_state";
static const char* NVS_KEY_ZONE  = "wz_zone";

// ────────────────────────────────────────────────────────────────────────────
// HELPERS
// ────────────────────────────────────────────────────────────────────────────

static inline bool zone_set() {
  return s_zone[0] != '\0';
}

static void persist_state() {
  nvs_store::set_u32(NVS_KEY_STATE, (uint32_t)s_state);
}

static void persist_zone() {
  nvs_store::set_blob(NVS_KEY_ZONE, s_zone, sizeof(s_zone));
}

static const char* state_name(SetupState s) {
  switch (s) {
    case WIZ_UNCONFIGURED: return "unconfigured";
    case WIZ_PAIRING:      return "pairing";
    case WIZ_TRAINING:     return "training";
    case WIZ_READY:        return "ready";
  }
  return "unknown";
}

static void set_state(SetupState next) {
  if (next == s_state) return;
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Wizard: %s → %s", state_name(s_state), state_name(next));
  s_state = next;
  persist_state();
}

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;

  memset(s_zone, 0, sizeof(s_zone));
  if (nvs_store::get_blob(NVS_KEY_ZONE, s_zone, sizeof(s_zone))) {
    // Ensure NUL-termination even if persisted blob was garbage.
    s_zone[MAX_ZONE_NAME_LEN - 1] = '\0';
  }
  s_state = (SetupState)nvs_store::get_u32(NVS_KEY_STATE, (uint32_t)WIZ_UNCONFIGURED);
  if (s_state > WIZ_READY) s_state = WIZ_UNCONFIGURED;

  s_initialized = true;
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Wizard: init state=%s zone=%s",
    state_name(s_state), zone_set() ? s_zone : "<unset>");
  return true;
}

void deinit() {
  if (!s_initialized) return;
  memset(s_zone, 0, sizeof(s_zone));
  s_state = WIZ_UNCONFIGURED;
  s_initialized = false;
}

void tick(uint32_t /*now_ms*/) {
  if (!s_initialized) return;

  switch (s_state) {
    case WIZ_UNCONFIGURED:
      // Nothing to drive; stays here until the user sets a zone and
      // calls start_pairing().
      break;

    case WIZ_PAIRING:
      // Household closes the window after ENROLLMENT_WINDOW_MS; we
      // advance to TRAINING if any device was paired, else back to
      // UNCONFIGURED so the user can retry.
      if (!household::is_enrolling()) {
        if (household::count() > 0) {
          set_state(WIZ_TRAINING);
        } else {
          set_state(WIZ_UNCONFIGURED);
          health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
            "Wizard: pairing window closed with no devices; back to UNCONFIGURED");
        }
      }
      break;

    case WIZ_TRAINING:
      if (baseline::training_complete()) {
        set_state(WIZ_READY);
      }
      break;

    case WIZ_READY:
      // Stable resting state. restart_training() is the only transition
      // out of here.
      break;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// ZONE
// ────────────────────────────────────────────────────────────────────────────

bool set_zone_name(const char* name) {
  if (!s_initialized) return false;

  memset(s_zone, 0, sizeof(s_zone));
  if (name && name[0] != '\0') {
    strncpy(s_zone, name, MAX_ZONE_NAME_LEN - 1);
  }
  persist_zone();

  // If the zone was just cleared and we hadn't progressed past PAIRING,
  // return to UNCONFIGURED — the user is re-setting up. We don't touch
  // state past PAIRING because TRAINING/READY represent real learned
  // data that a zone rename shouldn't discard.
  if (!zone_set() && s_state <= WIZ_PAIRING) {
    set_state(WIZ_UNCONFIGURED);
  }

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Wizard: zone = '%s'", zone_set() ? s_zone : "<unset>");
  return true;
}

bool get_zone_name(char* out, size_t out_len) {
  if (!s_initialized || !out) return false;
  if (out_len < MAX_ZONE_NAME_LEN) return false;
  if (!zone_set()) { out[0] = '\0'; return false; }
  strncpy(out, s_zone, out_len - 1);
  out[out_len - 1] = '\0';
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// SETUP FLOW
// ────────────────────────────────────────────────────────────────────────────

bool start_pairing() {
  if (!s_initialized) return false;
  if (!zone_set()) {
    health_logging::log(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Wizard: start_pairing rejected — zone not set");
    return false;
  }
  household::begin_enrollment();
  set_state(WIZ_PAIRING);
  return true;
}

bool finish_pairing() {
  if (!s_initialized) return false;
  if (s_state != WIZ_PAIRING) return false;

  household::end_enrollment();
  if (household::count() > 0) {
    set_state(WIZ_TRAINING);
  } else {
    set_state(WIZ_UNCONFIGURED);
  }
  return true;
}

bool restart_training() {
  if (!s_initialized) return false;
  if (!baseline::restart_training()) return false;
  // Don't touch familiar — the user's ambient fingerprints carry over.
  // Household IRKs also survive; if the user moved to a new house they
  // can forget household via its own API.
  set_state(WIZ_TRAINING);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// NOTIFICATION HELPERS
// ────────────────────────────────────────────────────────────────────────────

bool always_ignore_last_decision() {
  if (!s_initialized) return false;
  notify::AlertDecision d;
  if (!notify::get_last_decision(&d)) return false;
  // We only expose "always ignore" for suppressed decisions — it would
  // be surprising UX to ignore a pattern we just fired a high-severity
  // alert on. Users who want to mute a fired alert should use context
  // controls (QUIET_HOURS) or explicitly forget_always_ignored later.
  if (d.fired) return false;

  // notify::AlertDecision now carries the raw fingerprint that produced
  // the decision (post-audit fix). Pipe it straight into familiar's
  // always-ignore Bloom filter; subsequent matching events will be
  // suppressed by Phase 8's notify::evaluate().
  return familiar::always_ignore(d.fingerprint);
}

bool set_context(notify::Context c) {
  if (!s_initialized) return false;
  return notify::set_context(c);
}

// ────────────────────────────────────────────────────────────────────────────
// STATUS
// ────────────────────────────────────────────────────────────────────────────

static void fill_status_common(Status* out) {
  memset(out, 0, sizeof(*out));
  out->state = s_state;
  if (zone_set()) {
    strncpy(out->zone_name, s_zone, MAX_ZONE_NAME_LEN - 1);
  }
  out->context = notify::get_context();

  out->training_progress_bps = baseline::training_progress_bps();
  out->training_complete     = baseline::training_complete();

  household::Stats h;
  if (household::get_stats(&h)) {
    out->household_paired_count           = h.enrolled_count;
    out->household_enrolling              = h.enrolling;
    out->household_enrollment_ms_remaining = h.enrollment_ms_remaining;
  }

  baseline::Stats bl;
  if (baseline::get_stats(&bl)) {
    out->baseline_populated_buckets = bl.populated_buckets;
  }
}

bool get_status(Status* out) {
  if (!s_initialized || !out) return false;
  fill_status_common(out);

  notify::Stats n;
  if (notify::get_stats(&n)) {
    out->total_alerts_fired        = n.total_fired;
    out->total_events_evaluated    = n.total_evaluated;
    out->total_ambient_suppressed  = n.total_suppressed_ambient;
    out->total_household_suppressed = n.total_suppressed_household;
  }
  return true;
}

bool get_status_for_export(Status* out) {
  if (!s_initialized || !out) return false;
  fill_status_common(out);

  // Use notify's noised variant for the activity counters.
  notify::Stats n;
  if (notify::get_stats_for_export(&n)) {
    out->total_alerts_fired         = n.total_fired;
    out->total_events_evaluated     = n.total_evaluated;
    out->total_ambient_suppressed   = n.total_suppressed_ambient;
    out->total_household_suppressed = n.total_suppressed_household;
  }
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  if (!s_initialized) return false;

  // Save everything we're about to mutate.
  const SetupState saved_state = s_state;
  char saved_zone[MAX_ZONE_NAME_LEN];
  memcpy(saved_zone, s_zone, sizeof(saved_zone));

  // 1. set_zone_name with a test string, verify round-trip.
  const char* test_zone = "_test_zone_";
  const bool s1 = set_zone_name(test_zone);
  char got[MAX_ZONE_NAME_LEN];
  const bool s1b = get_zone_name(got, sizeof(got)) && strcmp(got, test_zone) == 0;

  // 2. start_pairing should succeed (zone set) and move state to PAIRING.
  const bool s2 = start_pairing() && s_state == WIZ_PAIRING;

  // 3. finish_pairing with zero paired devices returns to UNCONFIGURED.
  const bool s3 = finish_pairing() && s_state == WIZ_UNCONFIGURED;

  // 4. Clearing zone leaves us UNCONFIGURED.
  const bool s4 = set_zone_name("") && !zone_set() && s_state == WIZ_UNCONFIGURED;

  // 5. start_pairing with no zone is rejected.
  const bool s5 = !start_pairing();

  // 6. Status fills in reasonable values.
  set_zone_name(test_zone);
  Status st;
  const bool s6 = get_status(&st) && st.state == s_state && strcmp(st.zone_name, test_zone) == 0;

  // Restore state.
  memcpy(s_zone, saved_zone, sizeof(s_zone));
  persist_zone();
  s_state = saved_state;
  persist_state();

  const bool ok = s1 && s1b && s2 && s3 && s4 && s5 && s6;
  if (!ok) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Wizard self-test FAIL: %d %d %d %d %d %d %d",
      (int)s1, (int)s1b, (int)s2, (int)s3, (int)s4, (int)s5, (int)s6);
  } else {
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Wizard self-test: OK");
  }
  return ok;
}

}  // namespace wizard
