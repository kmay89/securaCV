/*
 * SecuraCV Canary — Setup wizard + orchestration (Phase 10)
 * Version 0.1.0
 *
 * Apple-like setup: one-tap pair, walk around tagging zones with a live
 * CSI meter, watch the 72-hour training bar tick. Three facts make this
 * module exist:
 *
 *   1. The firmware now has six sibling modules (household, familiar,
 *      baseline, notify, federated, dp) each with its own init / enroll /
 *      stats surface. A setup flow that talks to all of them via one
 *      state machine is way easier to wire to UI than six separate APIs.
 *
 *   2. The user needs a ZONE — a short human tag ("Back Door") that
 *      appears in alert reasons and on the wizard's live meter. We
 *      persist that here so every module can read it by the same name.
 *
 *   3. The "Always ignore this pattern" button in an alert has to tie
 *      back to the fingerprint of the LAST decision. notify already has
 *      get_last_decision(); wizard exposes the one-line helper that
 *      pulls that fingerprint and calls familiar::always_ignore(fp).
 *
 * This module is transport-agnostic. Phase 11 (tests) and the HTTP /
 * MQTT routing layer (in wap_server.cpp) wire the calls from the
 * outside world; here we only define the orchestration API.
 *
 * PRIVACY NOTES
 * =============
 *   • The zone name is user-chosen and user-facing; it's stored in NVS
 *     plaintext (same treatment as household::HouseholdSlot::label).
 *     If the user puts an identifier there, that's their call.
 *   • State is a small enum + a few timestamps; nothing leaks. The
 *     status export via get_status_for_export() returns DP-noised
 *     counters from the underlying modules.
 *   • always_ignore_last_decision does NOT export the fingerprint —
 *     it only pipes the in-RAM value from notify into familiar.
 */

#ifndef SECURACV_WIZARD_H
#define SECURACV_WIZARD_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "notify.h"  // notify::Context

namespace wizard {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

// Max length of a zone name (including NUL). Bounded for NVS blob size
// and for inclusion in the reason-string assembly without overflow.
static const size_t MAX_ZONE_NAME_LEN = 32;

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// High-level setup state, persisted to NVS. Progresses monotonically;
// restart_training() (a user-initiated reset) can bump us back to
// TRAINING without requiring re-pairing.
enum SetupState : uint8_t {
  WIZ_UNCONFIGURED = 0,  // fresh device; no zone, no paired phones
  WIZ_PAIRING      = 1,  // enrollment window open
  WIZ_TRAINING     = 2,  // 72 h baseline training under way
  WIZ_READY        = 3,  // training complete; quiet-by-default active
};

// Aggregated snapshot of every sibling module, suitable for the UI.
// Counters come through the underlying _for_export paths (DP-noised).
struct Status {
  // Top-level wizard state
  SetupState state;
  char       zone_name[MAX_ZONE_NAME_LEN];

  // User context — copied through from notify.
  notify::Context context;

  // Setup progress (0..10000 basis points).
  uint32_t training_progress_bps;
  bool     training_complete;

  // Counts
  uint8_t  household_paired_count;
  bool     household_enrolling;
  uint32_t household_enrollment_ms_remaining;
  uint16_t baseline_populated_buckets;

  // Activity summary (noised)
  uint32_t total_alerts_fired;
  uint32_t total_events_evaluated;
  uint32_t total_ambient_suppressed;
  uint32_t total_household_suppressed;
};

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

bool init();
void deinit();

// Call from rf_presence::update alongside the other module ticks.
// Advances the state machine based on household/baseline progress.
void tick(uint32_t now_ms);

// ════════════════════════════════════════════════════════════════════════════
// ZONE
// ════════════════════════════════════════════════════════════════════════════

// User sets the zone name (e.g. "Back Door"). Persisted to NVS. Empty
// name is accepted and returns the device to UNCONFIGURED state if it
// hadn't progressed past PAIRING.
bool set_zone_name(const char* name);

// Current zone name into the caller's buffer. Returns false if no zone
// set yet, or out_len < MAX_ZONE_NAME_LEN.
bool get_zone_name(char* out, size_t out_len);

// ════════════════════════════════════════════════════════════════════════════
// SETUP FLOW
// ════════════════════════════════════════════════════════════════════════════

// Open a household enrollment window. Moves state to PAIRING.
// Delegates to household::begin_enrollment(). Returns false if zone
// has not been named yet (users should tag the zone first).
bool start_pairing();

// Close enrollment early. If at least one phone was paired, moves to
// TRAINING; otherwise stays UNCONFIGURED so the user can retry.
bool finish_pairing();

// Force-restart training. Wipes baseline buckets, resets the 72 h
// window, moves state to TRAINING. Used when the user moves the
// device or has a big lifestyle change.
bool restart_training();

// ════════════════════════════════════════════════════════════════════════════
// NOTIFICATION HELPERS
// ════════════════════════════════════════════════════════════════════════════

// Wire the last-decision fingerprint to the familiar always-ignore
// filter. Returns false if no decision has been made yet, or if the
// decision was a fire (we don't ignore things we just alerted on —
// user should have a cooldown between "got an alert" and "mute it").
bool always_ignore_last_decision();

// User context (HOME / AWAY / QUIET_HOURS / TRAVELING). Delegates to
// notify::set_context; wraps with our own logging so the state
// transition is visible in the wizard UI's event log.
bool set_context(notify::Context c);

// ════════════════════════════════════════════════════════════════════════════
// STATUS
// ════════════════════════════════════════════════════════════════════════════

bool get_status(Status* out);
bool get_status_for_export(Status* out);   // DP-noised counters

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// Walks the state machine: UNCONFIGURED → PAIRING (after zone set) →
// TRAINING → READY. Does NOT actually pair a BLE peer (that requires a
// real SMP handshake); instead it verifies the state transitions and
// that the status struct is populated. Restores all original state
// (zone, context, NVS keys) on exit.
bool conformance_self_test();

}  // namespace wizard

#endif  // SECURACV_WIZARD_H
