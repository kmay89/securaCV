/*
 * SecuraCV Canary — Household Device Recognition
 * Version 0.1.0
 *
 * Privacy-preserving recognition of household devices for alert suppression,
 * modeled on Apple/Google Exposure Notification's use of Identity Resolving
 * Keys (IRKs) — but inverted. EN uses rotating identifiers so that *you*
 * can't be tracked by strangers; we use IRKs so that *your own devices*
 * are the only ones whose identity we resolve, and only to *quiet* them.
 *
 * DESIGN PRINCIPLES
 * =================
 *   1. We store the 16-byte IRK of paired household devices. We never
 *      store a MAC address, a name, a vendor, an OUI, or a serial number.
 *   2. On every BLE advertisement, we try to resolve the Resolvable Private
 *      Address (RPA) against the stored IRK set. If it resolves, the device
 *      is suppressed — no presence event, no token derivation.
 *   3. Household suppression is a STRONG promise: a user who paired their
 *      phone will never receive a "someone just arrived" alert because of
 *      that phone, even as the phone rotates its MAC every ~15 minutes.
 *   4. The IRK is a symmetric secret. If it leaked, an attacker could de-
 *      anonymize that specific device across RPA rotations. We therefore
 *      store IRKs only in NVS (flash-encrypted at rest on ESP32 with secure
 *      boot) and never export them over any wire, MQTT topic, or web UI.
 *
 * ENROLLMENT
 * ==========
 * A household device is added by BLE-bonding it to the canary during an
 * enrollment window. The bonding handshake (LE SC or legacy) delivers the
 * peer's IRK via an Identity Information PDU; the BLE stack passes it to
 * `household::add_irk()`. We never prompt for it, copy it from an app,
 * or ingest it from any other source — the pairing flow is the only path.
 *
 * BYTE ORDER CONVENTION
 * =====================
 * All multi-byte values use the SAME byte order as received from the
 * NimBLE stack (little-endian on air for BLE):
 *   mac[0]  = LSB of the address
 *   mac[5]  = MSB (contains the RPA type bits in its top two bits)
 *   irk[0..15] = as delivered by the SMP Identity Information PDU
 *
 * See BT Core Spec 5.3, Vol 3, Part H, §2.2.2 (function `ah`) for the
 * RPA resolution algorithm.
 */

#ifndef SECURACV_HOUSEHOLD_H
#define SECURACV_HOUSEHOLD_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace household {

// ════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ════════════════════════════════════════════════════════════════════════════

static const size_t IRK_LEN = 16;                // AES-128 key length
static const size_t MAX_HOUSEHOLD_DEVICES = 8;   // Hard cap — keeps NVS small
static const size_t MAX_LABEL_LEN = 16;          // Including NUL terminator

// Default enrollment window after begin_enrollment() is called.
// The BLE bonding handshake must complete within this window, after which
// the module auto-closes the window to prevent drive-by bonding attempts.
static const uint32_t ENROLLMENT_WINDOW_MS = 60 * 1000;  // 60 seconds

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// Outcome of a resolve_rpa() call, useful for diagnostics without revealing
// which slot matched (callers that just want a bool can use the simpler API).
struct ResolveResult {
  bool     matched;        // true if the MAC resolved to some stored IRK
  int8_t   slot;           // -1 if no match; 0..MAX-1 if matched
  bool     looked_like_rpa;// true if the MAC had the RPA type bits set
};

// Per-device trust role. Encoded into the low byte of the slot's `flags`
// field — the existing NVS layout is preserved (uint16_t at offset 36),
// so devices upgrading from main get ROLE_GUEST (the all-zero default)
// automatically. Higher numeric values do NOT imply more permissions —
// these are categories, not levels — but the ordering is stable so it's
// safe to compare with == in the hot path.
enum DeviceRole : uint8_t {
  ROLE_GUEST  = 0,  // Default for newly-paired device. Treated as household
                    // (suppresses arrival alerts) but does NOT influence the
                    // home/away auto-context — guests may come and go.
  ROLE_FAMILY = 1,  // Trusted household member, but not the operator. Same
                    // suppression as guest plus counted in "who is home"
                    // displays. Doesn't toggle auto-context by itself.
  ROLE_OWNER  = 2,  // Operator's own device. Hard-suppresses every alert
                    // about their own arrival/departure. Presence drives
                    // auto-context: any owner-IRK seen recently → CTX_HOME,
                    // none seen for the AWAY window → CTX_AWAY.
};

// ════════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ════════════════════════════════════════════════════════════════════════════

// Load IRK slots from NVS. Safe to call before WiFi/BLE init.
bool init();

// Secure-wipe all in-memory IRK material. Does NOT erase NVS persistence.
void deinit();

// ════════════════════════════════════════════════════════════════════════════
// ENROLLMENT
// ════════════════════════════════════════════════════════════════════════════

// Open a time-limited enrollment window. While open, the BLE stack's
// bonding handshake will call add_irk() on success. After the window
// closes (timeout or explicit end_enrollment()), bonding attempts are
// rejected by the BLE layer.
void begin_enrollment();

// Is the enrollment window currently open?
bool is_enrolling();

// Close the enrollment window early (user hits "done").
void end_enrollment();

// Milliseconds remaining in the enrollment window; 0 if closed.
uint32_t enrollment_ms_remaining();

// ════════════════════════════════════════════════════════════════════════════
// ADD / REMOVE
// ════════════════════════════════════════════════════════════════════════════

// Add a newly-bonded device's IRK. Called by the BLE stack on successful
// LE Secure Connections pairing when the enrollment window is open.
//
//   irk       16-byte IRK (see BYTE ORDER CONVENTION above)
//   label     optional short human tag (e.g. "Alice's iPhone"); may be NULL
//
// Returns slot index 0..MAX-1 on success, -1 on failure (no space,
// duplicate IRK, or enrollment window closed).
int add_irk(const uint8_t irk[IRK_LEN], const char* label);

// Remove a stored IRK by slot index. Secure-wipes the slot in memory and NVS.
bool remove_by_slot(uint8_t slot);

// Remove ALL stored IRKs ("forget all household devices"). Secure-wipes
// all slots and erases the NVS blob.
bool remove_all();

// How many household devices are currently enrolled.
uint8_t count();

// ════════════════════════════════════════════════════════════════════════════
// RESOLUTION (the hot path — called from rf_presence::feed_ble_scan)
// ════════════════════════════════════════════════════════════════════════════

// Fast yes/no resolution. Returns true if `mac` is a Resolvable Private
// Address that resolves to any stored IRK. O(count()) AES-128 ops.
//
// NOTE: mac may be any BLE address format. If the address is NOT an RPA
// (static, public, non-resolvable), this returns false — the caller should
// still run the normal presence pipeline, because we have no way to tell
// whether it's a household device or a stranger. The IRK trick only works
// on RPAs, which is what modern phones broadcast.
bool resolve_rpa(const uint8_t mac[6]);

// Detailed resolution with diagnostics. Same cost as resolve_rpa() but
// returns slot index and RPA-type flag for debugging / status UIs.
ResolveResult resolve_rpa_detailed(const uint8_t mac[6]);

// ════════════════════════════════════════════════════════════════════════════
// ROLE / PRESENCE — owner-aware suppression and auto-context
// ════════════════════════════════════════════════════════════════════════════

// Get the role currently assigned to a slot. Returns ROLE_GUEST for unused
// slots (safe default — least permissive).
DeviceRole get_role(uint8_t slot);

// Assign a role to an enrolled slot. Persists to NVS. The caller is
// expected to enforce its own authentication (REST endpoint validates the
// bearer token + bonded session before reaching this). Returns false if
// the slot is empty or `role` is out of range. Always logs to the witness
// chain via the supplied audit_log callback so role changes are auditable.
typedef void (*RoleAuditCallback)(uint8_t slot, DeviceRole old_role, DeviceRole new_role);
bool set_role(uint8_t slot, DeviceRole role, RoleAuditCallback audit_cb);

// Update the in-memory last-seen timestamp for slot N. Called from the hot
// path when an RPA resolves successfully. Not persisted (volatile) — phones
// re-emit advertisements every few seconds, so we'll relearn after reboot.
void mark_seen(uint8_t slot, uint32_t now_ms);

// device-millis() of the most recent successful RPA resolve for slot N.
// Returns 0 if never seen since boot.
uint32_t last_seen_ms(uint8_t slot);

// True if any slot with role >= min_role has been seen within the last
// `window_ms`. Used by presence_context to decide CTX_HOME vs CTX_AWAY.
//
// Carefully: does NOT reveal which slot matched (no side channel for
// "which family member is home"). Returns aggregate yes/no only.
bool any_role_seen_within(DeviceRole min_role, uint32_t window_ms, uint32_t now_ms);

// ════════════════════════════════════════════════════════════════════════════
// INTROSPECTION (for status API — does NOT expose IRK material)
// ════════════════════════════════════════════════════════════════════════════

// Copy the user-facing label for slot N into out_buf. Returns false if the
// slot is empty or slot index is out of range.
bool get_label(uint8_t slot, char* out_buf, size_t out_len);

// Enrollment timestamp in device-millis for slot N (not wall time; survives
// reboot as 0). Used by the UI to show "paired N days ago".
uint32_t get_added_ms(uint8_t slot);

// Statistics block exposed on /api/household/status.
struct Stats {
  uint8_t  enrolled_count;
  bool     enrolling;
  uint32_t enrollment_ms_remaining;
  uint32_t total_resolves_attempted;
  uint32_t total_resolves_matched;
  uint32_t total_non_rpa_seen;
};
bool get_stats(Stats* out);

// Same fields as get_stats(), but with differential-privacy Gaussian
// noise applied to the monotonic counter fields (total_* queries and
// matches). Use this variant whenever the Stats struct is about to
// cross the export boundary (HTTP, MQTT, mesh). The enrolled_count
// and enrolling flag are NOT noised — they're user-visible constants
// (≤ MAX_HOUSEHOLD_DEVICES) whose exact values the user needs. Only
// the activity counters carry privacy-sensitive behavior information.
bool get_stats_for_export(Stats* out);

// ════════════════════════════════════════════════════════════════════════════
// CONFORMANCE
// ════════════════════════════════════════════════════════════════════════════

// Runs a self-test: generates a random IRK, synthesizes an RPA from it,
// verifies round-trip resolution. Does NOT mutate stored IRKs. Returns
// true if both AES and the resolver agree on a known IRK↔RPA mapping.
bool conformance_self_test();

// Scans the slot array for any 6-byte sequence resembling a MAC OUI.
// (Structural guarantee: the slot struct has no MAC field; this is a
// defense-in-depth heuristic for defensive scans.)
bool conformance_no_mac_in_slots();

}  // namespace household

#endif  // SECURACV_HOUSEHOLD_H
