/*
 * SecuraCV Canary WAP — pure provisioning decisions (host-testable)
 *
 * Arduino-free: stdint only. Every branchy decision the provisioning flow
 * makes lives here so a host g++ run (test_provisioning_logic.cpp) can pin
 * it — the .ino keeps only the wiring. Wrap-safe time math throughout
 * (unsigned subtraction), matching the WAP's existing conventions.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_PROVISIONING_LOGIC_H
#define SECURACV_PROVISIONING_LOGIC_H

#include <stdint.h>

namespace provisioning_logic {

// The first-boot setup window exists to close an *abandoned* portal, not to
// reboot the device under a slow human. Due only when active and the full
// window has elapsed since the last sign of life.
inline bool setup_timeout_due(bool active, uint32_t now_ms,
                              uint32_t last_activity_ms, uint32_t window_ms) {
  return active && (uint32_t)(now_ms - last_activity_ms) >= window_ms;
}

// A cached WiFi scan is served instead of kicking a fresh radio sweep —
// scanning hops the single radio across channels and knocks the provisioning
// phone off the SoftAP mid-fetch. Fresh = scanned within ttl and non-empty.
// An empty cache is never fresh: a boot-time scan that found nothing should
// be retried, not served as "no networks".
inline bool scan_cache_fresh(uint32_t now_ms, uint32_t scanned_at_ms,
                             bool have_results, uint32_t ttl_ms) {
  return have_results && (uint32_t)(now_ms - scanned_at_ms) < ttl_ms;
}

// Should the firmware try to join home WiFi? Not in AP-only mode — the
// device runs standalone on its own SoftAP by explicit user choice.
inline bool sta_join_allowed(bool ap_only, bool configured, bool enabled) {
  return !ap_only && configured && enabled;
}

// Should the management SoftAP ever be torn down for radio stability?
// Never in AP-only mode (the AP *is* the product there), and otherwise only
// once the STA has held its association past the grace window — long enough
// for the provisioning phone to re-associate on the followed channel, get a
// DHCP lease back, and watch the wizard's success card render.
inline bool ap_teardown_due(bool ap_only, bool sta_connected, uint32_t now_ms,
                            uint32_t connected_since_ms, uint32_t grace_ms) {
  return !ap_only && sta_connected &&
         (uint32_t)(now_ms - connected_since_ms) > grace_ms;
}

// Deferred post-provisioning reboot: armed (deadline != 0) once the STA
// joins during first-boot setup, due when the grace elapses. Rebooting the
// moment WL_CONNECTED fired killed the AP ~1 s after the join — before the
// provisioning phone could re-associate and watch the success card — which
// made the whole AP grace pointless in the very flow it exists for.
// Wrap-safe signed-window compare.
inline bool deferred_reboot_due(uint32_t now_ms, uint32_t deadline_ms) {
  return deadline_ms != 0 && (int32_t)(now_ms - deadline_ms) >= 0;
}

// BLE discovery (Opera advertising + Nearby *active* scanning) shares the one
// 2.4 GHz radio with the SoftAP. The Nearby scanner holds the radio at ~99%
// duty for 5 s bursts (window 99 / interval 100), pinned to the WiFi/BLE core —
// and the first burst fires at boot. If a phone's WPA2 handshake to the
// provisioning AP overlaps a burst, the handshake frames are starved and the
// join fails intermittently ("broken loop" of retries). So hold BLE discovery
// until the SoftAP join window is truly over. The precise "over" signal is the
// AP being torn down: the firmware keeps the SoftAP up for a grace window even
// after the STA gets an IP (so a just-provisioned phone can re-associate and
// read the success card), then drops it via wifi_drop_ap() to run the stable
// STA+BLE combo — so gating on the AP being DOWN (not merely WL_CONNECTED) also
// keeps the 99%-duty scan out of that protected handoff window. In AP-only mode
// the AP is permanent (no STA to ever wait on), so start after a settle window
// that lets the operator's first association land cleanly. And a normal device
// that never reaches the home network — home WiFi down or gone — must NOT hold
// BLE off forever, so a longer max-hold fallback brings discovery up regardless
// (accepting steady-state coexistence, which is degraded, not broken).
// Start-only: once BLE discovery is up we never stop it here, so a later STA
// blip won't tear it down. Wrap-safe unsigned time math.
inline bool ble_discovery_start_due(bool ap_only, bool ap_active,
                                    uint32_t now_ms, uint32_t boot_ref_ms,
                                    uint32_t ap_only_settle_ms,
                                    uint32_t max_hold_ms) {
  // Normal mode, AP torn down: STA held past the grace → stable STA+BLE combo,
  // and the join/handoff window the AP grace protects is over.
  if (!ap_only && !ap_active) return true;
  const uint32_t held = (uint32_t)(now_ms - boot_ref_ms);
  // AP-only (persisted standalone, or a runtime AP-only/no-STA mode): the AP is
  // permanent, so start after the settle window and accept coexistence.
  if (ap_only) return held >= ap_only_settle_ms;
  // Normal mode but the AP is still up (home WiFi never joined): don't hold BLE
  // off forever — fall back after a longer hold.
  return held >= max_hold_ms;
}

// A FRESH unit — no Wi-Fi credentials stored yet, and no phone on its SoftAP
// — is the one case the deferral above costs the most: its Bluetooth setup
// door (ble_improv) is the way a phone hands it Wi-Fi with one tap, and the
// five-minute max-hold would keep that door shut for exactly the minutes the
// person is standing there. The two reasons for the deferral do not apply to
// it: nobody's WPA2 handshake is in flight (zero stations), and the heap
// guard still refuses a bring-up the RAM cannot afford. So a fresh unit
// brings BLE up after a short settle instead. The moment a phone joins the
// SoftAP (the wizard path), the caller falls back to the rule above.
// Wrap-safe unsigned time math.
inline bool ble_fresh_unit_start_due(bool fresh_unit, int ap_stations,
                                     uint32_t now_ms, uint32_t boot_ref_ms,
                                     uint32_t fresh_settle_ms) {
  if (!fresh_unit || ap_stations > 0) return false;
  return (uint32_t)(now_ms - boot_ref_ms) >= fresh_settle_ms;
}

// The fresh-unit path above brings the pairing channel up with its scanners
// HELD (bluetooth_channel::init(scanners_held=true)): the server, every
// service (the setup door among them) and advertising come up after the
// short settle, but the continuous presence scan and the active scan
// settings — the ~99%-duty bursts the deferral exists to keep away from a
// phone's WPA2 handshake — wait. They are released on the loop task exactly
// when the long path would have started the whole channel: this predicate
// is ble_discovery_start_due, by name, so the two cannot drift. Called with
// the same arguments the long path uses. Start-only, like the rule it
// aliases: once released they stay released.
inline bool ble_scanners_release_due(bool ap_only, bool ap_active,
                                     uint32_t now_ms, uint32_t boot_ref_ms,
                                     uint32_t ap_only_settle_ms,
                                     uint32_t max_hold_ms) {
  return ble_discovery_start_due(ap_only, ap_active, now_ms, boot_ref_ms,
                                 ap_only_settle_ms, max_hold_ms);
}

// Whether the BLE discovery gates above should treat the unit as AP-only.
// The sketch's Wi-Fi state machine reads WIFI_PROV_AP_ONLY for two very
// different units: one whose owner disconnected from home Wi-Fi at runtime
// (/api/wifi/disconnect — its credentials are kept, the AP is permanent for
// now, nobody's WPA2 handshake is coming, so the 45 s settle is right) and
// a FRESH unit that has nothing configured at all (its boot state is AP-only
// because there is no STA to try). Reading the fresh unit as AP-only ended
// the fresh path's scanner hold at that same 45 s settle — exactly while a
// phone could be joining the SoftAP for the wizard. So: AP-only for the
// discovery gates is the persisted standalone choice, or a runtime AP-only
// state WITH credentials behind it; a credential-less unit is not. A fresh
// unit then follows the long path's rule: its scanners are released when
// the AP drops after a join (the STA held past its grace) or at the
// max-hold, never at the settle.
inline bool ap_only_for_discovery(bool ap_only_persisted, bool state_ap_only,
                                  bool credentials_configured) {
  return ap_only_persisted || (state_ap_only && credentials_configured);
}

// The Bluetooth setup door's first-boot window is re-armed by a POWER CYCLE,
// not by a reboot: a never-provisioned unit restarts itself every 15 minutes
// (the wizard's abandonment timer), and if each restart re-armed the window
// the door would be open for the rest of the unit's life. So the spent time
// is carried across software resets in RTC-noinit memory (the pattern of
// hardware_state.h's crash breadcrumb): a magic word says the record was
// written by this firmware, and the record is honored only when the chip
// did NOT just power up. After a true power-on (or a brownout, which is a
// power event, or a reset the chip cannot name) RTC memory is whatever it
// was, so the record is cleared — a stale or random word never shortens or
// extends the window. True = keep the record; false = zero it.
inline bool door_window_record_valid(uint32_t magic, uint32_t expected_magic,
                                     bool power_cycle) {
  return magic == expected_magic && !power_cycle;
}

// Which pairing security profile the pairing channel should hold, given the
// setup door's wish (bluetooth_channel::set_setup_door). The profiles are
// ble_hs_cfg fields NimBLE reads at the START of a pairing, so a swap is
// applied between procedures only:
//   - door shut: Numeric Comparison (bonding, MITM, SC, DISPLAY_YESNO) —
//     the owner's profile, restored whatever else is going on (it is the
//     strict one; a pairing already under way keeps the profile it began
//     with, since the host read the fields when it started);
//   - door open, nothing in the way: Just Works (no bond, no MITM, SC,
//     NO_INPUT_OUTPUT) — the one a phone with no prior bond can open in a
//     tap, yielding an unauthenticated key only the Improv service's
//     *_ENC characteristics accept;
//   - door open while a Numeric Comparison is pending or the owner's
//     pairing mode is in flight (pairing_pending), or while a connected
//     link is AUTHENTICATED or BONDED (authenticated_link_up): HOLD — keep
//     what is applied and ask again next pass. Just Works must never be
//     applied under a bonded session: a re-pairing the phone starts on that
//     link would be answered by the weaker profile, and the owner's bonded
//     peer is exactly the party the strict profile exists for.
enum SecurityProfile : uint8_t {
  NUMERIC_COMPARISON = 0,  // apply the owner's profile (door shut)
  JUST_WORKS,              // apply the setup door's profile (door open)
  HOLD,                    // apply nothing; the caller retries next pass
};

inline SecurityProfile security_profile_for(bool door_open, bool pairing_pending,
                                            bool authenticated_link_up) {
  if (!door_open) return NUMERIC_COMPARISON;
  if (pairing_pending || authenticated_link_up) return HOLD;
  return JUST_WORKS;
}

// Keep an armed post-provisioning reboot from firing while the user is
// still actively working the wizard's final step (running the self-test,
// reading the recovery-kit card). Given an armed deadline, returns a
// deadline at least `min_remaining_ms` in the future — but never pulls a
// later deadline in, and never arms a disarmed (0) one. Wrap-safe.
inline uint32_t reboot_deadline_extend(uint32_t deadline_ms, uint32_t now_ms,
                                       uint32_t min_remaining_ms) {
  if (deadline_ms == 0) return 0;  // disarmed stays disarmed
  const uint32_t floor = now_ms + min_remaining_ms;
  // Push out only if the current deadline is sooner than the floor.
  return ((int32_t)(deadline_ms - floor) < 0) ? floor : deadline_ms;
}

}  // namespace provisioning_logic

#endif  // SECURACV_PROVISIONING_LOGIC_H
