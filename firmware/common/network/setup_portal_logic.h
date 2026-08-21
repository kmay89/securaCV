/**
 * @file setup_portal_logic.h
 * @brief Pure decision half of the shared headless setup portal.
 *
 * The I/O half (SoftAP + captive DNS + the wizard's HTTP surface) lives in
 * setup_portal.cpp beside this header. Everything here is timestamp math with
 * no Arduino and no heap, so `firmware/tests_host/test_setup_portal_logic.cpp`
 * can pin the choreography that was paid for on real phones:
 *
 *   - a successful join must NOT tear the AP down instantly — the phone's
 *     /status poll has to win the STA channel-change race and render its own
 *     "done", or every successful provision looks failed (the WAP lesson the
 *     display's portal re-learned on the 4.3" bench);
 *   - a portal raised for RECOVERY (saved credentials that stopped working)
 *     must keep quietly retrying the saved network underneath, because a
 *     router that was simply rebooting comes back on its own — a headless
 *     sensor may not wait for a human that a power blip never notified;
 *   - but never while a phone is associated or a wizard join is in flight:
 *     a background begin() yanks the radio out from under the human who is
 *     mid-setup, which reads as "the setup network kicked me off".
 *
 * All time math is wrap-safe signed-delta, per the firmware idiom for
 * millis() arithmetic.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace canary {
namespace net {

/** Portal timing constants — shared by every adopter so the choreography
 *  cannot drift per board. Values match the display portal's bench-tuned
 *  numbers (provision.cpp), which inherited them from the WAP. */
struct SetupPortalTiming {
  /// Success: wait up to this long for the phone's /status poll to ack…
  uint32_t ap_linger_max_ms = 25000;
  /// …then this beat after the ack, then teardown.
  uint32_t ap_linger_ack_ms = 1600;
  /// A wizard join attempt reads as failed after this long. 30 s, not 15:
  /// at range edge a slow association is indistinguishable from a wrong
  /// password until well past 15 s (WAP lesson).
  uint32_t sta_timeout_ms = 30000;
  /// Scan cache freshness (a live sweep knocks the phone off the AP).
  uint32_t scan_ttl_ms = 300000;
  /// AP up this long with no station ever associated -> log the one likely
  /// fix (a phone auto-rejoining with a stale saved password).
  uint32_t stuck_hint_ms = 45000;
  /// While recovering (saved credentials exist), quietly retry them at this
  /// cadence whenever the portal is idle.
  uint32_t background_retry_ms = 60000;
};

/**
 * @brief Is the post-success AP linger over?
 *
 * True once the phone has acked and the ack beat has played, or once the
 * cap expires (covering the phone that never polls again — or a background
 * rejoin with no phone involved at all, where @p phone_acked stays false
 * and @p acked_at carries the success time so only the short beat runs).
 */
inline bool portal_teardown_due(uint32_t now_ms,
                                uint32_t success_at_ms,
                                bool phone_acked,
                                uint32_t acked_at_ms,
                                const SetupPortalTiming& t) {
  if (phone_acked &&
      (int32_t)(now_ms - acked_at_ms) > (int32_t)t.ap_linger_ack_ms) {
    return true;
  }
  return (int32_t)(now_ms - success_at_ms) > (int32_t)t.ap_linger_max_ms;
}

/**
 * @brief Should the idle portal quietly retry the saved network now?
 *
 * Only when there IS a saved network to retry (recovery, not first boot),
 * nobody is on the AP, no wizard join is in flight, and the cadence is due.
 * First eligibility is measured from the portal raise, so a router mid-boot
 * gets a full minute before the radio splits its attention.
 */
inline bool portal_background_retry_due(uint32_t now_ms,
                                        bool have_saved_credentials,
                                        int stations,
                                        bool join_in_flight,
                                        uint32_t last_try_ms,
                                        const SetupPortalTiming& t) {
  if (!have_saved_credentials) return false;
  if (stations > 0 || join_in_flight) return false;
  return (int32_t)(now_ms - last_try_ms) >= (int32_t)t.background_retry_ms;
}

/**
 * @brief Should the "forget this network on your phone" hint fire?
 *
 * Once, after the AP has been up @ref SetupPortalTiming::stuck_hint_ms with
 * no station EVER associated this session — the overwhelmingly likely cause
 * is a phone auto-rejoining with a password from an earlier session, which
 * this side can neither fix nor detect (a refused association leaves no
 * trace here), so the log names the one move that clears it.
 */
inline bool portal_stuck_hint_due(uint32_t now_ms,
                                  uint32_t waiting_since_ms,
                                  bool ever_saw_station,
                                  bool already_hinted,
                                  const SetupPortalTiming& t) {
  if (ever_saw_station || already_hinted) return false;
  return (int32_t)(now_ms - waiting_since_ms) > (int32_t)t.stuck_hint_ms;
}

/** @brief Is the scan cache too old to serve without a fresh sweep? */
inline bool portal_scan_stale(uint32_t now_ms,
                              uint32_t scan_at_ms,
                              const SetupPortalTiming& t) {
  if (scan_at_ms == 0) return true;  // never swept
  return (int32_t)(now_ms - scan_at_ms) > (int32_t)t.scan_ttl_ms;
}

}  // namespace net
}  // namespace canary
