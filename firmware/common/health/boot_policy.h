/*
 * SecuraCV — boot crash-loop / safe-mode policy (host-testable, no Arduino).
 *
 * The pure *decision* half of "a bad firmware image must not be able to brick
 * trust" (docs/design/self_star_roadmap.md, TODO 2 — Boot safe-mode + A/B
 * auto-rollback). All the state lives in one NVS counter and all the judgement
 * lives in one pure function, so CI proves the recovery behaviour instead of
 * trusting a review of boot-path glue.
 *
 * WHAT THIS IS, AND ISN'T
 *   This layer decides, very early in boot and before any risky init, whether
 *   to proceed normally or fall back to a minimal safe-mode console. It is the
 *   LAST-RESORT net, complementary to — not a replacement for — the OTA A/B
 *   rollback that already ships in firmware/common/ota/securacv_ota.*:
 *
 *     · A/B rollback (securacv_ota_boot_self_test) handles "the *new* image is
 *       bad": while an image is PENDING_VERIFY, a failed post-flash self-test —
 *       or any crash/hang/brownout before the image confirms itself — makes the
 *       ESP bootloader revert to the last-known-good image on the next boot.
 *       There is a good image to go back to, so reverting is strictly better
 *       than a stripped-down console.
 *
 *     · Safe-mode (this policy) handles "the *confirmed* image can't come up":
 *       once an image has marked itself valid there is no A/B image to roll back
 *       to, yet a corrupt config, failing peripheral, or bad SD state can still
 *       crash-loop it. Rather than a dead device, degrade after N consecutive
 *       boots that never reach "healthy" to a recoverable safe mode that still
 *       prints the trust card + a recovery URL and accepts the read-only Tier
 *       ::Diag console (see console_scenes.h / test_console.h).
 *
 *   Because of that split, safe mode is entered ONLY for a confirmed image:
 *   while an image is unconfirmed this layer stands aside and lets A/B rollback
 *   own recovery.
 *
 * THE COUNTER'S LIFECYCLE (the boot-path wiring, landed separately + hardware-
 * validated, calls into the pure functions below):
 *   1. Very early in boot, before the risky init: load the persisted count,
 *      call decide(), and PERSIST decision.persist_count *before* proceeding —
 *      so a hang / watchdog reset / brownout during init is still counted.
 *   2. If decision.mode == BootMode::SafeMode, enter the safe-mode console and
 *      never fall through to normal init.
 *   3. Once the app reaches "healthy" (setup finished, post-boot self-test
 *      passed, main loop running — the same gate that confirms the OTA image),
 *      persist kHealthyReset. A good image reaching healthy on its first real
 *      boot zeroes the counter, so it never accumulates toward the threshold.
 *
 * ESCAPING SAFE MODE (safe mode is never a permanent brick — there is always a
 * reset path, preserving the "you can't brick it" promise):
 *   · a healthy boot of a good image             -> kHealthyReset
 *   · a fresh image installed over OTA           -> kFreshImageReset (a new
 *     image deserves a clean N attempts; the OTA engine resets on install)
 *   · an operator "clear & retry" from safe mode or the browser flasher's
 *     health tool                                -> kOperatorClearReset
 *   A full re-flash recovers via the operator clear (or an NVS erase).
 *
 * This file must compile hosted for tests_host/test_boot_policy.cpp with
 * -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BOOT_POLICY_H
#define SECURACV_BOOT_POLICY_H

#include <stdint.h>

namespace bootpolicy {

// ════════════════════════════════════════════════════════════════════════════
// Tunables
// ════════════════════════════════════════════════════════════════════════════

// Consecutive boots that never reach "healthy" before falling back to safe
// mode. Deliberately headroomed above transient churn: a single brownout, or
// the one extra boot an A/B rollback adds while the last-known-good image comes
// back up, must not trip it. A genuinely good image reaches healthy on its
// first real boot and resets the counter to zero, so reaching this count takes
// that many boots *in a row* where none reached healthy — a real crash loop.
inline constexpr uint16_t kDefaultSafeModeThreshold = 4;

// The persisted counter saturates here so repeated safe-mode boots neither
// overflow the uint16_t nor churn NVS past the point the decision is settled.
// Must stay comfortably above the threshold so the >= comparison is stable.
inline constexpr uint16_t kBootAttemptCap = 32;

// The value to persist when the counter should be cleared. Zero means "no
// unhealthy boots pending." Three named aliases record *why* a reset happened
// at the call site (they are intentionally the same value — a reset is a reset).
inline constexpr uint16_t kHealthyReset      = 0;  // app reached healthy
inline constexpr uint16_t kFreshImageReset   = 0;  // new image installed (OTA)
inline constexpr uint16_t kOperatorClearReset = 0; // operator "clear & retry"

// A sane threshold needs headroom above the +1 an A/B rollback can add before
// a good image resets the counter, and the cap must sit above the threshold so
// a saturated counter still compares >= threshold.
static_assert(kDefaultSafeModeThreshold >= 3,
              "safe-mode threshold needs headroom above rollback/brownout churn");
static_assert(kBootAttemptCap > kDefaultSafeModeThreshold,
              "counter cap must exceed the threshold so saturation still trips");

// ════════════════════════════════════════════════════════════════════════════
// Decision
// ════════════════════════════════════════════════════════════════════════════

enum class BootMode : uint8_t {
  Normal = 0,  // proceed with full init
  SafeMode,    // minimal recovery console only; do not fall through to init
};

struct Decision {
  uint16_t persist_count;  // persist this (write only when it differs from prev)
  BootMode mode;           // what this boot must do
};

// Saturating increment — never overflows, never exceeds kBootAttemptCap.
inline constexpr uint16_t saturating_inc(uint16_t n) {
  return n >= kBootAttemptCap ? kBootAttemptCap : static_cast<uint16_t>(n + 1);
}

// THE per-boot decision. Call ONCE, very early in boot, then persist
// Decision::persist_count before doing anything that can hang or crash.
//
//   prev            the persisted consecutive-unhealthy-boot count (0 on a
//                   first-ever boot / after any reset)
//   image_confirmed false while the running OTA image is still PENDING_VERIFY
//                   (A/B rollback owns recovery); true once it has confirmed
//                   itself valid, or for a factory / non-OTA image
//   threshold       kDefaultSafeModeThreshold unless a build overrides it
//
// The count is incremented (saturating) unconditionally so a genuine crash loop
// is always measured; safe mode is gated on image_confirmed so this layer never
// pre-empts an A/B rollback that would restore a known-good image instead.
inline constexpr Decision decide(uint16_t prev, bool image_confirmed,
                                 uint16_t threshold = kDefaultSafeModeThreshold) {
  const uint16_t next = saturating_inc(prev);
  const bool safe = image_confirmed && next >= threshold;
  return Decision{next, safe ? BootMode::SafeMode : BootMode::Normal};
}

// Would a boot at this persisted count enter safe mode? A pure predicate for
// callers (e.g. the safe-mode console deciding whether it is itself the active
// mode, or a diagnostic wanting to report "N boots from safe mode") that must
// not mutate the counter. Mirrors decide()'s gate without the increment.
inline constexpr bool in_safe_mode(uint16_t count, bool image_confirmed,
                                   uint16_t threshold = kDefaultSafeModeThreshold) {
  return image_confirmed && count >= threshold;
}

}  // namespace bootpolicy

#endif  // SECURACV_BOOT_POLICY_H
