/*
 * SecuraCV Canary — Auto-context from owner BLE presence
 *
 * Watches for OWNER-role household IRKs in the BLE scan stream and flips
 * notify::set_context() between CTX_HOME and CTX_AWAY automatically. The
 * existing manual context API still works — a user-set override pins the
 * context for OVERRIDE_TTL_MS so this module won't fight a deliberate
 * "I'm away on vacation, alert harder" toggle.
 *
 * STATE MACHINE
 * ─────────────
 *   tick():
 *     if user_override.active:
 *       apply user_override.context (no auto-switch)
 *     else:
 *       seen_recently = household::any_role_seen_within(ROLE_OWNER, RECENT_MS)
 *       if seen_recently:
 *         base = CTX_HOME
 *       elif age_since_any_owner_seen >= AWAY_MS:
 *         base = CTX_AWAY
 *       else:
 *         base = (no change — hysteresis band)
 *
 *       effective = quiet_hours_window_active() && base == CTX_HOME
 *                     ? CTX_QUIET_HOURS
 *                     : base
 *       notify::set_context(effective)
 *
 * HYSTERESIS
 * ──────────
 * RECENT_MS = 5 min, AWAY_MS = 10 min. Any owner seen within the last
 * 5 min ⇒ HOME. No owner seen for 10+ min ⇒ AWAY. The 5-minute band in
 * between leaves the previous state unchanged so a brief drop in BLE
 * coverage (phone went into a Faraday-cage purse) doesn't immediately
 * flip the device into AWAY mode and start firing alerts on the cat.
 *
 * SECURITY
 * ────────
 * - Only ROLE_OWNER drives this. ROLE_FAMILY and ROLE_GUEST are visible
 *   in "who's home" displays but don't toggle context. Reasoning: a guest
 *   leaving shouldn't make us silent; we want OWNER-tied auto-quiet to be
 *   a deliberate user choice, not a side effect of who happens to come by.
 * - User override is rate-limited and capped (OVERRIDE_TTL_MS = 24 h) so
 *   a stale "I'm away" doesn't silently linger forever.
 * - Context transitions are audit-logged (witness chain entry) so the
 *   reason for any suppression decision is reconstructible after the fact.
 */

#ifndef SECURACV_PRESENCE_CONTEXT_H
#define SECURACV_PRESENCE_CONTEXT_H

#include <stdint.h>
#include "notify.h"

namespace presence_context {

// Time windows for the auto-context state machine.
static constexpr uint32_t OWNER_RECENT_MS = 5  * 60 * 1000;  // CTX_HOME if owner seen ≤ this
static constexpr uint32_t OWNER_AWAY_MS   = 10 * 60 * 1000;  // CTX_AWAY if no owner seen for ≥ this
static constexpr uint32_t OVERRIDE_TTL_MS = 24 * 60 * 60 * 1000UL;  // manual override expires after 24 h

// ── Lifecycle ──────────────────────────────────────────────────────────────

bool init();
void deinit();

// Pump the state machine. Cheap (one comparison + one household:: query)
// — safe to call from the main loop at ~1 Hz.
void tick();

// ── Manual override (user explicitly pins context for up to OVERRIDE_TTL_MS)
// Returns false if `c` is out of range. Pinned context overrides automatic
// transitions until clear_override() or the TTL expires.
bool set_override(notify::Context c);
void clear_override();

// ── Introspection ──────────────────────────────────────────────────────────

struct Status {
  notify::Context auto_context;     // what auto-mode would set right now
  notify::Context effective_context; // what notify::get_context() actually sees
  bool            override_active;
  uint32_t        override_ms_remaining;
  bool            owner_seen_recently;  // any ROLE_OWNER IRK within OWNER_RECENT_MS
  uint32_t        ms_since_any_owner;   // 0 if any owner seen this scan cycle
};
bool get_status(Status* out);

// Optional callback fired on every effective-context change. Used by
// canary_wap.ino to write a witness-chain audit entry.
typedef void (*ContextChangeCallback)(notify::Context old_ctx, notify::Context new_ctx, const char* reason);
void set_change_callback(ContextChangeCallback cb);

}  // namespace presence_context

#endif  // SECURACV_PRESENCE_CONTEXT_H
