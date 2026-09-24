/*
 * SecuraCV Canary WAP — the three-tier health summary (host-testable)
 *
 * Arduino-free: stdint.h only. The headline dashboard's strip says one of
 * three things — Good / Needs attention / Action required — in plain words,
 * instead of making a nontechnical owner read crypto_healthy / sd_state /
 * last_reset flags (ENTERPRISE_READINESS_TODO §2 "Simple status language").
 * GET /api/status carries the verdict as `status_tier` + `status_reason`;
 * the dashboard maps the reason CODE to its words in COPY.tier (so the copy
 * stays in the one microcopy bank the lint reads, and the firmware never
 * ships prose). Pinned by tests_host/test_status_tier_logic.cpp.
 *
 * Ranking, worst first — the first matching row wins, and the reason is the
 * single thing the owner should look at:
 *   ACTION_REQUIRED
 *     signing      the device cannot sign records (crypto self-test failed)
 *     verify       a stored record failed its signature/hash check
 *     safe_mode    booted in safe mode (optional hardware switched off)
 *   NEEDS_ATTENTION
 *     sd_card      a card is present but erroring (records stay on the device)
 *     restarted    the last reset looked like a crash
 *     low_memory   free heap fell below the floor at some point
 *     notes        log entries are waiting to be acknowledged
 *   GOOD
 *     ok
 *
 * A MISSING card is not a problem — plenty of installs run without one —
 * only a card that is there and failing is. The wording decision for the
 * three labels is the maintainer's (backlog F21 decision 1); the codes are
 * stable, so rewording is a COPY edit, never a firmware one.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_STATUS_TIER_LOGIC_H
#define SECURACV_STATUS_TIER_LOGIC_H

#include <stdint.h>

namespace status_tier_logic {

enum class Tier : uint8_t {
  GOOD = 0,
  NEEDS_ATTENTION = 1,
  ACTION_REQUIRED = 2,
};

// Snapshot of the health facts the firmware already tracks (g_health / g_hw).
struct Inputs {
  bool     crypto_healthy;     // signing self-test passed
  uint32_t verify_failures;    // records that failed verification
  bool     safe_mode;          // hardware_state safe mode
  bool     sd_card_erroring;   // card present, state machine says ERROR
  bool     last_reset_crash;   // the reset reason looked like a crash
  uint32_t min_free_heap;      // lowest free heap seen (bytes)
  uint32_t low_heap_floor;     // below this, memory is worth a look
  uint32_t logs_unacked;       // warnings/errors not yet acknowledged
};

struct Verdict {
  Tier        tier;
  const char* tier_code;    // "good" | "needs_attention" | "action_required"
  const char* reason_code;  // one of the codes in the header comment
};

inline const char* tier_code(Tier t) {
  switch (t) {
    case Tier::ACTION_REQUIRED: return "action_required";
    case Tier::NEEDS_ATTENTION: return "needs_attention";
    case Tier::GOOD:            break;
  }
  return "good";
}

inline Verdict make(Tier t, const char* reason) {
  Verdict v;
  v.tier = t;
  v.tier_code = tier_code(t);
  v.reason_code = reason;
  return v;
}

inline Verdict evaluate(const Inputs& in) {
  if (!in.crypto_healthy)       return make(Tier::ACTION_REQUIRED, "signing");
  if (in.verify_failures > 0)   return make(Tier::ACTION_REQUIRED, "verify");
  if (in.safe_mode)             return make(Tier::ACTION_REQUIRED, "safe_mode");
  if (in.sd_card_erroring)      return make(Tier::NEEDS_ATTENTION, "sd_card");
  if (in.last_reset_crash)      return make(Tier::NEEDS_ATTENTION, "restarted");
  if (in.low_heap_floor > 0 && in.min_free_heap > 0 &&
      in.min_free_heap < in.low_heap_floor)
                                return make(Tier::NEEDS_ATTENTION, "low_memory");
  if (in.logs_unacked > 0)      return make(Tier::NEEDS_ATTENTION, "notes");
  return make(Tier::GOOD, "ok");
}

}  // namespace status_tier_logic

#endif  // SECURACV_STATUS_TIER_LOGIC_H
