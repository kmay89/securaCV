/**
 * @file wifi_join_policy.h
 * @brief One answer, fleet-wide, to "the uplink didn't come up — now what?"
 *
 * Every networked Canary has to decide the same three things when Wi-Fi will
 * not associate: **what to tell the human**, **whether a reboot could possibly
 * help**, and **how long to wait before trying again**. Those decisions were
 * copy-pasted into `canary-display`, `canary-sense` and `canary-vision`, plus a
 * fourth copy of the reason strings inside the display's onboarding portal —
 * and they had already drifted apart in exactly the way FLEET_PARITY.md warns
 * about:
 *
 *   - the display had learned not to reboot a link that never worked, but had
 *     lost the retry jitter;
 *   - sense and vision had the jitter, and still rebooted forever;
 *   - the display's boot message and its portal's message for the *same*
 *     failure said different things, under a comment asking a future reader to
 *     "keep the two in step" by hand.
 *
 * So it lives here once, as pure data-in/data-out with no Arduino and no heap,
 * and `firmware/tests_host/test_wifi_join_policy.cpp` pins the behaviour.
 *
 * ## The rule that matters
 *
 * **Never reboot a link that has never worked.** A device that reboots because
 * the boot-time join timed out re-runs that identical join, against the
 * identical network, with the identical credentials — so a wrong password, a
 * renamed SSID, or a 5 GHz-only AP this radio cannot see becomes a silent
 * ~30-second reboot cycle with nothing on screen and no way out. That is the
 * "setup loop" a real operator hit on the 4-inch display: it never finished
 * booting, so the wizard that could have fixed the password never appeared.
 *
 * A reboot is only plausible recovery for a link that **was** associated and
 * then dropped, where the radio or the DHCP lease may genuinely be wedged.
 * Hence [`WifiRetry::ever_online`] gates every reboot decision here.
 *
 * The uplink is also not allowed to hold the rest of the device hostage. A
 * Canary that cannot reach the router can still show its screen, keep its
 * touch surface alive, talk to the fleet over ESP-NOW, and serve its own setup
 * portal. Boot proceeds; retry is the loop's job, not boot's.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace canary {
namespace net {

/**
 * @brief Why a join attempt failed, in terms every board shares.
 *
 * Deliberately not `wl_status_t`: that type belongs to Arduino, and this header
 * must compile on a host with no ESP32 toolchain. Each board maps its own
 * radio's status into this enum — a two-line switch — and everything downstream
 * is shared.
 */
enum class JoinFailure : uint8_t {
  /// The SSID was not seen at all. Overwhelmingly the 5 GHz-only case.
  NotFound = 0,
  /// Associated far enough to be rejected: the passphrase is wrong.
  BadPassword = 1,
  /// Associated, but no usable address (DHCP silent, or the AP dropped us).
  NoAddress = 2,
  /// Anything else, including "still trying when the clock ran out".
  Unknown = 3,
};

/**
 * @brief A short label for a status line or a log prefix. No trailing period.
 *
 * This is the text the onboarding portal and the glass both show, so it is kept
 * to a few words. [`join_failure_detail`] carries the sentence, and
 * [`join_failure_hint`] the fix.
 */
inline const char* join_failure_label(JoinFailure f) {
  switch (f) {
    case JoinFailure::NotFound:    return "Network not found";
    case JoinFailure::BadPassword: return "Wrong password";
    case JoinFailure::NoAddress:   return "No address from the router";
    case JoinFailure::Unknown:     break;
  }
  return "Couldn't connect";
}

/**
 * @brief The full sentence for a serial log or a portal banner.
 *
 * Says what happened AND that the device stays up, because the previous
 * behaviour was to vanish into a reboot loop and the difference is the whole
 * point of this change.
 */
inline const char* join_failure_detail(JoinFailure f) {
  switch (f) {
    case JoinFailure::NotFound:
      return "Couldn't find that network. Staying up and retrying in the "
             "background. (2.4GHz only — a 5GHz-only or band-steered SSID "
             "won't be visible to this radio.)";
    case JoinFailure::BadPassword:
      return "Wrong Wi-Fi password. Staying up and retrying in the background.";
    case JoinFailure::NoAddress:
      return "Joined the network but the router never gave out an address. "
             "Staying up and retrying in the background.";
    case JoinFailure::Unknown:
      break;
  }
  return "Couldn't join Wi-Fi yet. Staying up and retrying in the background.";
}

/**
 * @brief The single most likely fix, in the user's words, for a small screen.
 *
 * Lower case and un-punctuated on purpose: these render under a status line on
 * a 1.47-inch panel as well as in the portal, and sentence case there reads as
 * a second error rather than a suggestion.
 */
inline const char* join_failure_hint(JoinFailure f) {
  switch (f) {
    case JoinFailure::NotFound:    return "it only sees 2.4 GHz wifi - not 5";
    case JoinFailure::BadPassword: return "passwords are case-sensitive";
    case JoinFailure::NoAddress:   return "your router may be out of addresses";
    case JoinFailure::Unknown:     break;
  }
  return "try moving it closer to the router";
}

/**
 * @brief Whether a human could plausibly fix this by re-entering credentials.
 *
 * A wrong password or a missing SSID is a **setup** problem: the right response
 * is to raise the setup portal so someone can correct it. A weak signal or a
 * silent DHCP server is an **environment** problem, where reopening setup only
 * takes the device off the air while it retries. Boards use this to decide
 * whether to fall back to the SoftAP wizard.
 */
inline bool join_failure_is_fixable_by_setup(JoinFailure f) {
  return f == JoinFailure::NotFound || f == JoinFailure::BadPassword;
}

/** Tunables. Defaults match what the three boards independently converged on. */
struct WifiRetryPolicy {
  /// First backoff step; doubles per consecutive failure.
  uint32_t base_ms = 2000;
  /// Ceiling for the doubling.
  uint32_t max_ms = 30000;
  /// How long a *previously working* link may stay down before a reboot.
  uint32_t outage_reboot_ms = 300000;
  /// Cap on the doubling exponent, so `base << n` cannot overflow or explode.
  uint8_t max_shift = 5;
};

/** Live retry state. The board owns the storage; this header owns the rules. */
struct WifiRetry {
  /// Currently associated.
  bool online = false;
  /// Associated at least once since power-on. Gates every reboot — see above.
  bool ever_online = false;
  /// `millis()` when the current outage began.
  uint32_t lost_since_ms = 0;
  /// `millis()` of the most recent attempt.
  uint32_t last_attempt_ms = 0;
  /// Consecutive failed attempts in this outage; a success resets it.
  uint32_t attempts = 0;
};

/** What the caller should do on this pass of its loop. */
enum class WifiAction : uint8_t {
  /// Nothing to do yet — keep waiting out the backoff.
  Wait = 0,
  /// Start another association attempt now.
  Retry = 1,
  /// A previously-working link has been down too long; reboot as last resort.
  Reboot = 2,
};

/**
 * @brief Backoff for the Nth consecutive failure: base << (n-1), capped.
 *
 * `attempts == 0` yields `base_ms` rather than something clever, so the first
 * retry after a drop is prompt.
 */
inline uint32_t wifi_backoff_ms(const WifiRetryPolicy& p, uint32_t attempts) {
  uint32_t shift = attempts > 0 ? attempts - 1 : 0;
  if (shift > p.max_shift) shift = p.max_shift;
  uint32_t ms = p.base_ms << shift;
  if (ms > p.max_ms || ms < p.base_ms /* overflow guard */) ms = p.max_ms;
  return ms;
}

/**
 * @brief Spread a fleet's reconnects so N devices don't retry in lockstep.
 *
 * After a router reboot every Canary in the house comes back at the same
 * instant and hammers the AP together. `jitter` is caller-supplied randomness
 * (`esp_random()` on device, a fixed value in tests) and is folded in as up to
 * a quarter of the backoff, so the schedule is still bounded and testable.
 */
inline uint32_t wifi_backoff_with_jitter_ms(const WifiRetryPolicy& p,
                                            uint32_t attempts,
                                            uint32_t jitter) {
  const uint32_t ms = wifi_backoff_ms(p, attempts);
  return ms + (jitter % (ms / 4 + 1));
}

/**
 * @brief Decide what to do while disconnected.
 *
 * Wrap-safe signed-delta time math throughout, per the firmware idiom for
 * `millis()` arithmetic — a device that has been up for 49 days must not
 * suddenly decide every deadline has passed.
 *
 * @param now_ms  current `millis()`
 * @param jitter  caller randomness; pass 0 for a deterministic schedule
 */
inline WifiAction wifi_next_action(const WifiRetryPolicy& p,
                                   const WifiRetry& s,
                                   uint32_t now_ms,
                                   uint32_t jitter) {
  // The rule this whole header exists for: a link that has never associated is
  // not a wedged link, it is a wrong one. Rebooting re-runs the same failed
  // join forever and hides the setup portal that could have fixed it.
  if (s.ever_online &&
      (int32_t)(now_ms - s.lost_since_ms) >= (int32_t)p.outage_reboot_ms) {
    return WifiAction::Reboot;
  }
  const uint32_t backoff = wifi_backoff_with_jitter_ms(p, s.attempts, jitter);
  if ((int32_t)(now_ms - s.last_attempt_ms) >= (int32_t)backoff) {
    return WifiAction::Retry;
  }
  return WifiAction::Wait;
}

/**
 * @brief Should the board raise its setup portal instead of retrying quietly?
 *
 * True once a **setup-fixable** failure has survived `attempts_before_setup`
 * tries without the device ever having been online. The two conditions matter
 * together:
 *
 *   - `ever_online` false, because a device that worked this morning and lost
 *     the AP should not throw away a good configuration and start advertising
 *     an open setup network to the street;
 *   - a fixable reason, because no amount of re-typing a correct password
 *     fixes a router that is simply out of range.
 *
 * The delay exists so a slow-booting AP doesn't trip setup mode; by the default
 * schedule three attempts is roughly 15 seconds.
 */
inline bool wifi_should_open_setup(const WifiRetry& s,
                                   JoinFailure f,
                                   uint32_t attempts_before_setup) {
  if (s.ever_online) return false;
  if (!join_failure_is_fixable_by_setup(f)) return false;
  return s.attempts >= attempts_before_setup;
}

}  // namespace net
}  // namespace canary
