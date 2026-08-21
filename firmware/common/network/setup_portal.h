/**
 * @file setup_portal.h
 * @brief The shared headless setup portal: SoftAP + captive DNS + join wizard.
 *
 * This is the recovery path docs/design/onboarding_shared_module.md promised
 * the two portal-less boards ("canary-sense: No portal... no recovery path but
 * re-flashing. canary-vision: Same as Sense."): a device-unique setup network
 * a phone can join, a captive page that lists nearby networks, and a tested
 * join that persists credentials only on success. First adopters are
 * canary-sense and canary-vision — greenfield, exactly as that plan's Phase 4
 * ordered them; canary-display and the WAP keep their own proven portals until
 * the migration phases land.
 *
 * Behavior contract (each rule was paid for once — see LESSONS_LEARNED
 * §Networking and the display's provision.cpp, which this module distills):
 *
 *   - Captive DNS answers A queries ONLY; AAAA/HTTPS get NODATA. Runs the
 *     whole life of the AP, draining several packets per pass.
 *   - OS probes stay on plain HTTP port 80: Android gets a real 204, Windows
 *     the exact NCSI bodies, Apple a redirect so the sheet pops.
 *   - AP SSID suffix comes from the caller's device identity (pseudonym /
 *     pubkey fingerprint) — never the MAC.
 *   - The AP password is minted once and persisted (NVS "securacv"/"ap_pass");
 *     a store that will not hold it degrades to a per-session SSID so a phone
 *     never remembers this name with a key the next boot lacks.
 *   - Credentials persist only after the join actually worked.
 *   - After success the AP lingers for the phone's ack; instant teardown makes
 *     every successful provision look failed on the phone.
 *   - While idle in recovery (saved credentials exist), the saved network is
 *     quietly retried every minute — a router that was just rebooting rejoins
 *     without a human. Never while a phone is associated or a join is testing.
 *   - Wi-Fi password fields are masked text, never type="password".
 *
 * Non-blocking by design: setup_portal_begin() raises the AP and returns;
 * setup_portal_loop() is pumped from the board's loop() so sensing continues
 * while the portal waits. (The display's portal blocks instead — its glass IS
 * the product; a radar or a camera keeps witnessing while unprovisioned.)
 *
 * The pure decision half is setup_portal_logic.h, pinned by
 * firmware/tests_host/test_setup_portal_logic.cpp.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace canary {
namespace net {

struct SetupPortalConfig {
  /// Product name the wizard page shows, e.g. "Canary Sense".
  const char* product_name;
  /// Device-unique SSID suffix (>= 4 chars used), derived from the salted
  /// pseudonym or pubkey fingerprint — never from the MAC.
  const char* id_suffix;
  /// AP channel. Boards with an ESP-NOW fleet band pass their fallback
  /// channel so an unprovisioned boxed pair stays audible to each other;
  /// everyone else passes 1. Phones scan every channel for an SSID.
  uint8_t ap_channel;
  /// Saved credentials exist (portal raised for RECOVERY, not first boot):
  /// enables the quiet background retry of the saved network.
  bool have_saved_credentials;
  /// Persist a working join. Return false if the store refused; the session
  /// still completes (the join is live) and the failure is logged honestly.
  bool (*save_credentials)(const char* ssid, const char* pass);
  /// Retry the saved network (recovery only; may be null when
  /// have_saved_credentials is false). Implemented by the board's wifi_mgr
  /// as a plain WiFi.begin(saved_ssid, saved_pass).
  void (*begin_saved)();
};

/**
 * @brief Raise the setup AP, captive DNS, and wizard HTTP server.
 *
 * Idempotent while active. Returns false only when the radio could not
 * bring the AP up. The caller owns WiFi mode until this returns; afterwards
 * the portal owns the radio until it tears down (see
 * setup_portal_join_in_flight for the one window where that matters most).
 */
bool setup_portal_begin(const SetupPortalConfig& cfg);

/**
 * @brief One pass: drain DNS, serve HTTP, drive the join state machine.
 *
 * Cheap when idle. Call from loop() every pass while
 * setup_portal_active() — the board's own Wi-Fi supervision must stand down
 * for the duration (the portal is the retry policy while it is up).
 */
void setup_portal_loop(uint32_t now_ms);

/** @brief True from begin() until the post-join teardown (or stop()). */
bool setup_portal_active();

/**
 * @brief True while a candidate join owns the radio (wizard or background).
 *
 * Nothing else may retune the channel or call WiFi.begin() during this
 * window — the same contract as the display portal's join-in-flight guard.
 */
bool setup_portal_join_in_flight();

/**
 * @brief True once a join succeeded and the portal tore itself down.
 *
 * Latched; reading clears it. The board's wifi_mgr uses this to adopt the
 * now-connected STA link (mark online, reset its retry state).
 */
bool setup_portal_take_joined();

/** @brief Tear down unconditionally (rare; teardown is normally automatic). */
void setup_portal_stop();

}  // namespace net
}  // namespace canary
