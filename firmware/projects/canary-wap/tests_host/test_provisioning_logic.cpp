/* Host tests for provisioning_logic.h — the pure decisions behind the
 * canary-wap first-run wizard. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_provisioning_logic.cpp \
 *       -o /tmp/test_provisioning_logic && /tmp/test_provisioning_logic
 */

#include <cstdio>
#include <cstdlib>

#include "provisioning_logic.h"

using namespace provisioning_logic;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_setup_timeout() {
  const uint32_t WIN = 15u * 60u * 1000u;
  // Inactive wizard never times out.
  CHECK(!setup_timeout_due(false, WIN * 2, 0, WIN));
  // Fresh activity: not due.
  CHECK(!setup_timeout_due(true, 1000, 1000, WIN));
  CHECK(!setup_timeout_due(true, WIN - 1, 0, WIN));
  // Window elapsed: due.
  CHECK(setup_timeout_due(true, WIN, 0, WIN));
  // touch() semantics: activity late in the window restarts the countdown.
  uint32_t touched = WIN - 5000;
  CHECK(!setup_timeout_due(true, WIN + 1000, touched, WIN));
  CHECK(setup_timeout_due(true, touched + WIN, touched, WIN));
  // millis() wraparound: started just before wrap, now just after.
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(!setup_timeout_due(true, near_wrap + 60000u /* wraps */, near_wrap, WIN));
  CHECK(setup_timeout_due(true, near_wrap + WIN, near_wrap, WIN));
}

static void test_scan_cache() {
  const uint32_t TTL = 5u * 60u * 1000u;
  // Empty cache is never fresh, however recent.
  CHECK(!scan_cache_fresh(1000, 1000, false, TTL));
  // Recent + non-empty: fresh.
  CHECK(scan_cache_fresh(1000, 500, true, TTL));
  CHECK(scan_cache_fresh(TTL - 1, 0, true, TTL));
  // Aged out.
  CHECK(!scan_cache_fresh(TTL, 0, true, TTL));
  // Wrap-safe.
  uint32_t near_wrap = 0xFFFFFF00u;
  CHECK(scan_cache_fresh(near_wrap + 1000u /* wraps */, near_wrap, true, TTL));
}

static void test_sta_join() {
  // AP-only wins over everything: the user chose standalone.
  CHECK(!sta_join_allowed(true, true, true));
  // Normal configured+enabled device joins.
  CHECK(sta_join_allowed(false, true, true));
  // Unconfigured or disabled: no join.
  CHECK(!sta_join_allowed(false, false, true));
  CHECK(!sta_join_allowed(false, true, false));
}

static void test_ap_teardown() {
  const uint32_t GRACE = 120000u;
  // AP-only: never torn down, even long after a (stale) connect stamp.
  CHECK(!ap_teardown_due(true, true, GRACE * 10, 0, GRACE));
  // STA not connected: nothing to trade the AP for.
  CHECK(!ap_teardown_due(false, false, GRACE * 10, 0, GRACE));
  // Inside the grace window the AP must survive — this is the window in
  // which the provisioning phone re-associates and reads the success card.
  CHECK(!ap_teardown_due(false, true, GRACE, 0, GRACE));  // strict >
  CHECK(!ap_teardown_due(false, true, 8000, 0, GRACE));   // the old 8 s bug
  // Past the grace: teardown for radio stability.
  CHECK(ap_teardown_due(false, true, GRACE + 1, 0, GRACE));
  // Wrap-safe.
  uint32_t near_wrap = 0xFFFF0000u;
  CHECK(!ap_teardown_due(false, true, near_wrap + 60000u, near_wrap, GRACE));
  CHECK(ap_teardown_due(false, true, near_wrap + GRACE + 1, near_wrap, GRACE));
}

static void test_deferred_reboot() {
  const uint32_t GRACE = 120000u;
  // Unarmed (deadline 0): never due, no matter the clock.
  CHECK(!deferred_reboot_due(0, 0));
  CHECK(!deferred_reboot_due(0xFFFFFFFFu, 0));
  // Armed: not due inside the grace — this is the window in which the
  // provisioning phone re-associates and reads the success card (rebooting
  // at WL_CONNECTED, the old behavior, made the grace pointless).
  uint32_t deadline = 1000 + GRACE;
  CHECK(!deferred_reboot_due(1000, deadline));
  CHECK(!deferred_reboot_due(deadline - 1, deadline));
  // Due at/after the deadline.
  CHECK(deferred_reboot_due(deadline, deadline));
  CHECK(deferred_reboot_due(deadline + 5000, deadline));
  // Wrap-safe: deadline just past the wrap, now just before it.
  uint32_t near_wrap = 0xFFFFF000u;
  uint32_t wrapped_deadline = near_wrap + GRACE;  // wraps
  CHECK(!deferred_reboot_due(near_wrap, wrapped_deadline));
  CHECK(deferred_reboot_due(wrapped_deadline, wrapped_deadline));
}

static void test_reboot_deadline_extend() {
  const uint32_t MIN = 90000u;  // keep >= 90 s for the user to finish step 5
  // Disarmed stays disarmed.
  CHECK(reboot_deadline_extend(0, 1000, MIN) == 0);
  CHECK(reboot_deadline_extend(0, 0xFFFFFFFFu, MIN) == 0);
  // Deadline sooner than now+MIN gets pushed out to exactly now+MIN.
  CHECK(reboot_deadline_extend(1000 + 5000, 1000, MIN) == 1000 + MIN);
  // A deadline already further out than now+MIN is left alone (never pulled in).
  CHECK(reboot_deadline_extend(1000 + MIN + 10000, 1000, MIN) == 1000 + MIN + 10000);
  // Exactly at the floor: unchanged.
  CHECK(reboot_deadline_extend(1000 + MIN, 1000, MIN) == 1000 + MIN);
  // Wrap-safe: now near the wrap, floor wraps past 0.
  uint32_t near_wrap = 0xFFFFF000u;
  uint32_t soon = near_wrap + 1000u;              // deadline sooner than floor
  CHECK(reboot_deadline_extend(soon, near_wrap, MIN) ==
        (uint32_t)(near_wrap + MIN));
}

static void test_ble_discovery_start() {
  const uint32_t SETTLE = 45000u;
  const uint32_t MAXHOLD = 300000u;
  // Args: (ap_only, ap_active, now, boot_ref, settle, max_hold).
  // Normal mode, AP torn down → due (steady STA+BLE combo; the AP grace/handoff
  // window the phone needed is over). This is the clean steady-state start.
  CHECK(ble_discovery_start_due(false, false, 0, 0, SETTLE, MAXHOLD));
  CHECK(ble_discovery_start_due(false, false, 1000, 0, SETTLE, MAXHOLD));
  // Normal mode, AP still up (provisioning / STA connected but still in the AP
  // grace, or home WiFi not joined): held below the fallback so the 99%-duty
  // scan never fights the phone's SoftAP handshake or the post-join handoff.
  CHECK(!ble_discovery_start_due(false, true, 1000, 0, SETTLE, MAXHOLD));
  CHECK(!ble_discovery_start_due(false, true, MAXHOLD - 1, 0, SETTLE, MAXHOLD));
  // ...but never held FOREVER: past the max-hold, start regardless so Chirp/
  // Nearby offline features aren't permanently disabled on a WiFi-down device.
  CHECK(ble_discovery_start_due(false, true, MAXHOLD, 0, SETTLE, MAXHOLD));     // boundary >=
  CHECK(ble_discovery_start_due(false, true, MAXHOLD + 5000, 0, SETTLE, MAXHOLD));
  // AP-only (persisted standalone, or runtime AP-only): AP is permanent, no STA
  // to wait on. Held during the shorter settle so the operator's first join
  // lands cleanly, then due — the max-hold is irrelevant here.
  CHECK(!ble_discovery_start_due(true, true, SETTLE - 1, 0, SETTLE, MAXHOLD));
  CHECK(ble_discovery_start_due(true, true, SETTLE, 0, SETTLE, MAXHOLD));       // boundary >=
  CHECK(ble_discovery_start_due(true, true, SETTLE + 5000, 0, SETTLE, MAXHOLD));
  // Windows measured from the boot reference, not absolute time.
  CHECK(!ble_discovery_start_due(true, true, 100000 + SETTLE - 1, 100000, SETTLE, MAXHOLD));
  CHECK(ble_discovery_start_due(true, true, 100000 + SETTLE, 100000, SETTLE, MAXHOLD));
  CHECK(!ble_discovery_start_due(false, true, 100000 + MAXHOLD - 1, 100000, SETTLE, MAXHOLD));
  CHECK(ble_discovery_start_due(false, true, 100000 + MAXHOLD, 100000, SETTLE, MAXHOLD));
  // Wrap-safe: boot reference just before the millis() wrap, now just after.
  uint32_t near_wrap = 0xFFFFF000u;
  CHECK(!ble_discovery_start_due(true, true, near_wrap + 1000u /* wraps */, near_wrap, SETTLE, MAXHOLD));
  CHECK(ble_discovery_start_due(true, true, near_wrap + SETTLE, near_wrap, SETTLE, MAXHOLD));
  CHECK(ble_discovery_start_due(false, true, near_wrap + MAXHOLD, near_wrap, SETTLE, MAXHOLD));
}

static void test_ble_fresh_unit_start() {
  const uint32_t FRESH = 5000;
  // A fresh unit with nobody on its SoftAP: due after the short settle,
  // measured from the boot reference, wrap-safe.
  CHECK(!ble_fresh_unit_start_due(true, 0, FRESH - 1, 0, FRESH));
  CHECK(ble_fresh_unit_start_due(true, 0, FRESH, 0, FRESH));         // boundary >=
  CHECK(ble_fresh_unit_start_due(true, 0, 100000 + FRESH, 100000, FRESH));
  CHECK(!ble_fresh_unit_start_due(true, 0, 100000 + FRESH - 1, 100000, FRESH));
  const uint32_t near_wrap = 0xFFFFF000u;
  CHECK(ble_fresh_unit_start_due(true, 0, near_wrap + FRESH, near_wrap, FRESH));
  // A phone on the SoftAP means a WPA2 handshake may be in flight: never the
  // short path (the caller falls back to ble_discovery_start_due).
  CHECK(!ble_fresh_unit_start_due(true, 1, FRESH + 60000, 0, FRESH));
  // Not fresh (credentials stored): never the short path either.
  CHECK(!ble_fresh_unit_start_due(false, 0, FRESH + 60000, 0, FRESH));
}

// The fresh-unit path brings the channel up with its scanners HELD; they are
// released exactly when the long path would have started the whole channel.
// Pinned as a sequence: held at the fresh settle, still held through the
// SoftAP join window, released when that window clears (the AP torn down)
// or, if home Wi-Fi never comes, at the max-hold — and the alias is the
// long path's rule by name, so every verdict matches it.
static void test_ble_scanners_held_then_released() {
  const uint32_t FRESH = 5000;
  const uint32_t SETTLE = 45000u;
  const uint32_t MAXHOLD = 300000u;
  const uint32_t boot = 100000u;
  // t = fresh settle: the fresh path brings the channel up (scanners held)...
  CHECK(ble_fresh_unit_start_due(true, 0, boot + FRESH, boot, FRESH));
  // ...and the release is NOT yet due: the AP is up, nobody has joined.
  CHECK(!ble_scanners_release_due(false, true, boot + FRESH, boot, SETTLE, MAXHOLD));
  // A phone joins the SoftAP and provisions over the wizard instead: the
  // scanners stay held through the whole AP grace window.
  CHECK(!ble_scanners_release_due(false, true, boot + 60000u, boot, SETTLE, MAXHOLD));
  CHECK(!ble_scanners_release_due(false, true, boot + MAXHOLD - 1, boot, SETTLE, MAXHOLD));
  // The AP is torn down (the STA held past its grace): released now.
  CHECK(ble_scanners_release_due(false, false, boot + 130000u, boot, SETTLE, MAXHOLD));
  // Or home Wi-Fi never came and the AP stays up: released at the max-hold,
  // never held forever (the Nearby sensor is not a fresh unit's hostage).
  CHECK(ble_scanners_release_due(false, true, boot + MAXHOLD, boot, SETTLE, MAXHOLD));
  // A runtime AP-only state releases on the shorter settle, as the long path
  // would start.
  CHECK(!ble_scanners_release_due(true, true, boot + SETTLE - 1, boot, SETTLE, MAXHOLD));
  CHECK(ble_scanners_release_due(true, true, boot + SETTLE, boot, SETTLE, MAXHOLD));
  // The alias IS the long path's rule: every verdict agrees, across the grid.
  const bool bools[2] = { false, true };
  const uint32_t times[6] = { boot, boot + FRESH, boot + SETTLE - 1, boot + SETTLE,
                              boot + MAXHOLD - 1, boot + MAXHOLD + 7 };
  for (bool ap_only : bools) {
    for (bool ap_active : bools) {
      for (uint32_t now : times) {
        CHECK(ble_scanners_release_due(ap_only, ap_active, now, boot, SETTLE, MAXHOLD) ==
              ble_discovery_start_due(ap_only, ap_active, now, boot, SETTLE, MAXHOLD));
      }
    }
  }
  // Wrap-safe, like the rule it aliases.
  const uint32_t near_wrap = 0xFFFFF000u;
  CHECK(!ble_scanners_release_due(false, true, near_wrap + 1000u, near_wrap, SETTLE, MAXHOLD));
  CHECK(ble_scanners_release_due(false, true, near_wrap + MAXHOLD, near_wrap, SETTLE, MAXHOLD));
}

// The pairing channel's security profile for the setup door: the full
// (door_open, pairing_pending, authenticated_link_up) matrix, then the
// transitions the channel's set_setup_door() walks.
static void test_security_profile_matrix() {
  // Door shut: the owner's Numeric Comparison, whatever else is going on —
  // the strict profile is never withheld.
  CHECK(security_profile_for(false, false, false) == NUMERIC_COMPARISON);
  CHECK(security_profile_for(false, false, true)  == NUMERIC_COMPARISON);
  CHECK(security_profile_for(false, true,  false) == NUMERIC_COMPARISON);
  CHECK(security_profile_for(false, true,  true)  == NUMERIC_COMPARISON);
  // Door open, nothing in the way: Just Works.
  CHECK(security_profile_for(true, false, false) == JUST_WORKS);
  // Door open, a pairing pending: hold (the host reads the profile at the
  // start of a procedure; swapping under one is a half-applied pairing).
  CHECK(security_profile_for(true, true, false) == HOLD);
  // Door open, an authenticated or bonded link up: hold — Just Works is
  // never applied under a bonded session.
  CHECK(security_profile_for(true, false, true) == HOLD);
  // Both in the way: hold.
  CHECK(security_profile_for(true, true, true) == HOLD);
}

static void test_security_profile_transitions() {
  // The applied profile, as set_setup_door() keeps it: HOLD leaves it alone.
  SecurityProfile applied = NUMERIC_COMPARISON;
  auto step = [&](bool door_open, bool pairing_pending, bool auth_link) {
    const SecurityProfile p = security_profile_for(door_open, pairing_pending, auth_link);
    if (p != HOLD) applied = p;
    return p;
  };
  // Boot, credentials stored, door shut: Numeric Comparison applied.
  CHECK(step(false, false, false) == NUMERIC_COMPARISON);
  CHECK(applied == NUMERIC_COMPARISON);
  // Factory reset at runtime: the door opens, but the owner's bonded phone
  // is still connected (the dashboard that issued the reset). Held: the
  // bonded session keeps the strict profile.
  CHECK(step(true, false, true) == HOLD);
  CHECK(applied == NUMERIC_COMPARISON);
  // Still held while that link is up, pass after pass.
  CHECK(step(true, false, true) == HOLD);
  CHECK(applied == NUMERIC_COMPARISON);
  // The bonded link drops: Just Works applied on the next pass.
  CHECK(step(true, false, false) == JUST_WORKS);
  CHECK(applied == JUST_WORKS);
  // A Just Works client is connected (unauthenticated: not an authenticated
  // link): the door stays applied — re-asking is idempotent.
  CHECK(step(true, false, false) == JUST_WORKS);
  CHECK(applied == JUST_WORKS);
  // The join persisted credentials: the door shuts; Numeric Comparison is
  // restored even though that client is still connected (no pairing pends).
  CHECK(step(false, false, false) == NUMERIC_COMPARISON);
  CHECK(applied == NUMERIC_COMPARISON);
  // Later, the owner starts pairing mode and the door asks to open (a
  // second factory reset): held while the Numeric Comparison is pending...
  CHECK(step(true, true, false) == HOLD);
  CHECK(applied == NUMERIC_COMPARISON);
  // ...and the moment it completes as a bond, still held (the link is now
  // authenticated), so Just Works never lands under that session.
  CHECK(step(true, false, true) == HOLD);
  CHECK(applied == NUMERIC_COMPARISON);
  // Shutting the door while a pairing pends is never refused.
  CHECK(step(false, true, true) == NUMERIC_COMPARISON);
  CHECK(applied == NUMERIC_COMPARISON);
}

int main() {
  test_setup_timeout();
  test_scan_cache();
  test_sta_join();
  test_ap_teardown();
  test_deferred_reboot();
  test_reboot_deadline_extend();
  test_ble_discovery_start();
  test_ble_fresh_unit_start();
  test_ble_scanners_held_then_released();
  test_security_profile_matrix();
  test_security_profile_transitions();
  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL provisioning_logic tests PASSED\n");
  return 0;
}
