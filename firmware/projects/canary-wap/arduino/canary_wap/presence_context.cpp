/*
 * SecuraCV Canary — Auto-context implementation.
 *
 * See presence_context.h for the state-machine spec and threat model.
 */

#include "presence_context.h"
#include "household.h"
#include "notify.h"
#include "health_log.h"
#include "nvs_store.h"

#include <Arduino.h>

namespace presence_context {

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool s_initialized = false;

// User override: when set, pins notify context to s_override_ctx until
// s_override_started_ms + OVERRIDE_TTL_MS, then expires automatically.
static bool             s_override_active     = false;
static notify::Context  s_override_ctx        = notify::CTX_HOME;
static uint32_t         s_override_started_ms = 0;

// Last computed automatic context (separate from notify::get_context which
// might be a manual override). Used for hysteresis and stats.
static notify::Context  s_last_auto_ctx       = notify::CTX_AWAY;

// Last effective context the state machine pushed to notify. Used to
// detect transitions for the audit callback so we don't fire on every tick.
static notify::Context  s_last_pushed_ctx     = notify::CTX_AWAY;

// Optional audit hook. Caller (canary_wap.ino) wires this up to a
// witness-chain entry so context changes are forensically reconstructible.
static ContextChangeCallback s_change_cb = nullptr;

// NVS keys for override survival across reboots.
static const char* NVS_KEY_OVR_ACTIVE = "pc_ovr_act";
static const char* NVS_KEY_OVR_CTX    = "pc_ovr_ctx";
static const char* NVS_KEY_OVR_AT     = "pc_ovr_at";

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

static void load_override_from_nvs() {
  s_override_active     = nvs_store::get_u32(NVS_KEY_OVR_ACTIVE, 0) != 0;
  uint32_t ctx_raw      = nvs_store::get_u32(NVS_KEY_OVR_CTX, (uint32_t)notify::CTX_HOME);
  if (ctx_raw > (uint32_t)notify::CTX_TRAVELING) ctx_raw = (uint32_t)notify::CTX_HOME;
  s_override_ctx        = (notify::Context)ctx_raw;
  s_override_started_ms = nvs_store::get_u32(NVS_KEY_OVR_AT, 0);
}

static void persist_override() {
  nvs_store::set_u32(NVS_KEY_OVR_ACTIVE, s_override_active ? 1 : 0);
  nvs_store::set_u32(NVS_KEY_OVR_CTX,    (uint32_t)s_override_ctx);
  nvs_store::set_u32(NVS_KEY_OVR_AT,     s_override_started_ms);
}

bool init() {
  if (s_initialized) return true;
  load_override_from_nvs();
  // The override timestamp loaded from NVS is from a previous boot, which
  // we can't reconcile against the current monotonic millis(). Reset it to
  // "now" so the TTL window starts fresh after reboot — that's the most
  // user-friendly behavior (user is unlikely to want a 23 h-old override
  // to instantly re-expire after a power blip).
  if (s_override_active) s_override_started_ms = millis();
  s_initialized = true;
  return true;
}

void deinit() {
  s_initialized = false;
}

// ────────────────────────────────────────────────────────────────────────────
// STATE MACHINE
// ────────────────────────────────────────────────────────────────────────────

static void push_context(notify::Context c, const char* reason) {
  if (c == s_last_pushed_ctx) return;
  notify::Context old = s_last_pushed_ctx;
  notify::set_context(c);
  s_last_pushed_ctx = c;
  if (s_change_cb) s_change_cb(old, c, reason);
}

void tick() {
  if (!s_initialized) return;
  const uint32_t now = millis();

  // Override path.
  if (s_override_active) {
    // TTL expiry check (use unsigned underflow-safe age computation).
    const uint32_t age = (now >= s_override_started_ms)
                          ? (now - s_override_started_ms) : 0;
    if (age >= OVERRIDE_TTL_MS) {
      s_override_active = false;
      persist_override();
      health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
        "presence_context: override expired (24 h TTL)");
      // Fall through to automatic mode.
    } else {
      push_context(s_override_ctx, "user_override");
      return;
    }
  }

  // Automatic mode.
  bool seen = household::any_role_seen_within(
                household::ROLE_OWNER, OWNER_RECENT_MS, now);

  notify::Context base;
  if (seen) {
    base = notify::CTX_HOME;
  } else if (!household::any_role_seen_within(
                household::ROLE_OWNER, OWNER_AWAY_MS, now)) {
    base = notify::CTX_AWAY;
  } else {
    // In the hysteresis band — keep the previous automatic context to
    // avoid flapping when an owner phone is intermittently visible.
    base = s_last_auto_ctx;
  }

  // (Future hook) quiet-hours overlay would go here. We deliberately do
  // not check wall-clock time yet — the canary_wap firmware doesn't ship
  // an authoritative time source we can trust before NTP sync, and the
  // notify::CTX_QUIET_HOURS context is settable via the existing manual
  // API in the meantime.
  notify::Context effective = base;

  s_last_auto_ctx = base;
  push_context(effective, seen ? "owner_present" : "owner_absent");
}

// ────────────────────────────────────────────────────────────────────────────
// OVERRIDE
// ────────────────────────────────────────────────────────────────────────────

bool set_override(notify::Context c) {
  if ((uint8_t)c > (uint8_t)notify::CTX_TRAVELING) return false;
  s_override_active     = true;
  s_override_ctx        = c;
  s_override_started_ms = millis();
  persist_override();
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "presence_context: override -> ctx=%u (24 h TTL)", (unsigned)c);
  return true;
}

void clear_override() {
  if (!s_override_active) return;
  s_override_active = false;
  persist_override();
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "presence_context: override cleared");
}

// ────────────────────────────────────────────────────────────────────────────
// INTROSPECTION
// ────────────────────────────────────────────────────────────────────────────

bool get_status(Status* out) {
  if (!out) return false;
  const uint32_t now = millis();
  out->auto_context        = s_last_auto_ctx;
  out->effective_context   = s_last_pushed_ctx;
  out->override_active     = s_override_active;
  if (s_override_active) {
    const uint32_t age = (now >= s_override_started_ms)
                          ? (now - s_override_started_ms) : 0;
    out->override_ms_remaining = (age >= OVERRIDE_TTL_MS)
                                  ? 0 : (OVERRIDE_TTL_MS - age);
  } else {
    out->override_ms_remaining = 0;
  }
  out->owner_seen_recently = household::any_role_seen_within(
                               household::ROLE_OWNER, OWNER_RECENT_MS, now);

  // Compute the youngest owner age across all slots. Walk and take the min;
  // if no owner has ever been seen, return UINT32_MAX (caller can decide
  // how to render "never").
  uint32_t youngest_age = UINT32_MAX;
  for (uint8_t i = 0; i < household::MAX_HOUSEHOLD_DEVICES; i++) {
    if (household::get_role(i) != household::ROLE_OWNER) continue;
    const uint32_t ts = household::last_seen_ms(i);
    if (ts == 0) continue;
    const uint32_t age = (now >= ts) ? (now - ts) : 0;
    if (age < youngest_age) youngest_age = age;
  }
  out->ms_since_any_owner = youngest_age;
  return true;
}

void set_change_callback(ContextChangeCallback cb) { s_change_cb = cb; }

}  // namespace presence_context
