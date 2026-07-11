#pragma once
#include <stdint.h>
#include <cstddef>

#include "canary/fleet/fleet_model.h"

// Attention policy — who gets to make noise, when, and what happens to the
// noise that wasn't allowed (display_care_wave.md §2).
//
// The single most hated behavior across every comparable product line —
// baby monitors, security panels, wellness hubs — is the 2 a.m. maintenance
// chirp: a low battery or a lost link waking the house with no snooze
// granularity. The policy here is the fix, stated once and enforced in one
// place:
//
//   - Tier 1 (unacked Alert/Tamper) is the ONLY sound allowed during quiet
//     hours, and it re-voices until acknowledged, ramping from soft to full
//     (wake gently, escalate honestly — no product ships this).
//   - Tier 2 (Warn: stale witness, low battery) sounds by day, and at night
//     is SUPPRESSED into the overnight ledger instead — the glass stays
//     visually honest all night, and the morning summary says what happened
//     while you slept. Nothing is lost, nothing nags.
//   - All-clear only ever sounds by day, once, on resolution.
//
// Pure C++ (time injected, no Arduino) so every rule above is host-tested.

namespace canary::care {

using canary::fleet::Sev;

enum class Sound : uint8_t { None = 0, Tier1, Tier2, AllClear };

struct SoundDecision {
  Sound sound = Sound::None;
  uint8_t ramp = 0;  // 0 soft, 1 mid, 2 full — chime duty ladder
};

class AttentionPolicy {
 public:
  // Call once per loop pass with the fleet's current truth. Transition
  // edges (the "did something change" part) live in here.
  SoundDecision decide(Sev worst, bool acked, bool quiet, uint32_t now,
                       uint32_t revoice_ms) {
    SoundDecision d;
    const bool alarm = worst >= Sev::Alert && !acked;
    if (alarm) {
      if (!in_episode_) { in_episode_ = true; voices_ = 0; }
      if (prev_worst_ < Sev::Alert || !prev_alarm_ ||
          (int32_t)(now - last_voice_ms_) >= (int32_t)revoice_ms) {
        d.sound = Sound::Tier1;
        d.ramp = voices_ >= 2 ? 2 : voices_;  // soft -> mid -> full -> full…
        if (voices_ < 255) voices_++;
        last_voice_ms_ = now;
      }
    } else {
      in_episode_ = false;
      if (worst == Sev::Warn && prev_worst_ < Sev::Warn) {
        if (!quiet) {
          d.sound = Sound::Tier2;
          d.ramp = 2;
          last_voice_ms_ = now;
        }
        // At night the Warn edge makes NO sound — the caller's PER-WITNESS
        // ledger scan records who/why (a fleet-wide flag here would miss a
        // second witness going Warn while the first still holds the level;
        // review catch).
      } else if (worst <= Sev::Notice && prev_worst_ >= Sev::Warn && !quiet) {
        d.sound = Sound::AllClear;
        d.ramp = 1;
      }
    }
    prev_worst_ = worst;
    prev_alarm_ = alarm;
    return d;
  }

  // Escalation-on-no-ack (display_care_wave.md §5): true exactly once per
  // alarm episode when it has run unacknowledged past the deadline AND the
  // caller can actually deliver it (`can_send`: broker up, clock valid).
  // The deadline latches only on a deliverable pass — a Tier-1 that crosses
  // the deadline while the broker is down escalates the moment the link
  // returns, instead of being silently consumed (review catch). Ack or
  // resolution resets the episode.
  bool escalation_due(Sev worst, bool acked, uint32_t now,
                      uint32_t deadline_ms, bool can_send) {
    const bool alarm = worst >= Sev::Alert && !acked;
    if (!alarm) {
      alarm_clocked_ = false;
      escalated_ = false;
      return false;
    }
    if (!alarm_clocked_) {
      alarm_clocked_ = true;
      alarm_since_ms_ = now;
    }
    if (!escalated_ && can_send &&
        (int32_t)(now - alarm_since_ms_) >= (int32_t)deadline_ms) {
      escalated_ = true;
      return true;
    }
    return false;
  }

 private:
  Sev prev_worst_ = Sev::Ok;
  bool prev_alarm_ = false;
  bool in_episode_ = false;
  uint8_t voices_ = 0;
  uint32_t last_voice_ms_ = 0;
  bool alarm_clocked_ = false;
  bool escalated_ = false;
  uint32_t alarm_since_ms_ = 0;
};

// ── Overnight ledger ─────────────────────────────────────────────────────
// What quiet hours silenced, so the morning summary can say it. Small,
// deduplicated by (who, what) — a witness that flaps all night is one row,
// not thirty. Cleared by the household acknowledge (reading the summary IS
// the ack) or by the next night starting fresh.

struct NightNotice {
  char who[24] = {0};
  char what[24] = {0};
  uint32_t epoch = 0;  // first occurrence, 0 = clock was unknown
};

class NightLedger {
 public:
  static constexpr int CAP = 6;

  void note(const char* who, const char* what, uint32_t epoch) {
    if (!who || !what) return;
    for (int i = 0; i < count_; i++) {
      if (str_eq(rows_[i].who, who) && str_eq(rows_[i].what, what)) return;
    }
    if (count_ >= CAP) { overflow_ = true; return; }
    copy_str(rows_[count_].who, sizeof(rows_[count_].who), who);
    copy_str(rows_[count_].what, sizeof(rows_[count_].what), what);
    rows_[count_].epoch = epoch;
    count_++;
  }

  int count() const { return count_; }
  bool overflowed() const { return overflow_; }
  const NightNotice* at(int i) const {
    return (i >= 0 && i < count_) ? &rows_[i] : nullptr;
  }

  void clear() {
    count_ = 0;
    overflow_ = false;
  }

  // One-line morning summary: "While you slept: 2 notices" (buf always
  // NUL-terminated; empty when there is nothing to say).
  int summary(char* buf, size_t cap) const {
    if (!buf || cap == 0) return 0;
    if (count_ == 0) { buf[0] = '\0'; return 0; }
    int n = snprintf_shim(buf, cap, count_, overflow_);
    return n;
  }

 private:
  // Kept out-of-line-ish so this header stays <stdio.h>-free for hosts that
  // care; a hand-rolled formatter for one fixed sentence.
  static int snprintf_shim(char* buf, size_t cap, int count, bool overflow) {
    static const char PRE[] = "While you slept: ";
    size_t o = 0;
    for (size_t i = 0; PRE[i] && o + 1 < cap; i++) buf[o++] = PRE[i];
    if (count > 9) count = 9;
    if (o + 1 < cap) buf[o++] = (char)('0' + count);
    const char* tail = count == 1 ? " notice" : " notices";
    for (size_t i = 0; tail[i] && o + 1 < cap; i++) buf[o++] = tail[i];
    if (overflow && o + 1 < cap) buf[o++] = '+';
    buf[o] = '\0';
    return (int)o;
  }

  static bool str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
  }
  static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
  }

  NightNotice rows_[CAP] = {};
  int count_ = 0;
  bool overflow_ = false;
};

// Why is this witness at Warn? One honest phrase for the ledger/summary.
inline const char* warn_reason(const canary::fleet::Witness& w) {
  using canary::fleet::Link;
  if (w.link == Link::Lost || w.link == Link::Stale ||
      w.link == Link::Offline) {
    return "went quiet";
  }
  if (w.battery_present && w.battery_pct >= 0 && w.battery_pct < 10) {
    return "battery low";
  }
  return "needs a look";
}

}  // namespace canary::care
