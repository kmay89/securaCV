// include/canary/care/wake_alarm.h — two-phase gentle wake (nightstand wave).
//
// The Loftie lesson, twice over: (1) a soft melodic phase followed by an
// insistent phase measurably reduces sleep inertia; (2) an alarm that can
// fail is worse than no alarm — so this whole machine runs on-device from
// a persisted schedule. The hub CONFIGURES the alarm (retained topic); it
// is never in the firing path.
//
// Timeline for at = T:
//   T-ramp .. T      Ramp    backlight sunrise (gamma-corrected, caller)
//   T .. T+60s       Phase1  soft ascending chirps every ~25 s
//   T+60s .. T+p2    Gap     silence (the built-in snooze)
//   T+p2 .. T+p2+10m Phase2  insistent pulses until dismissed
//   after that       Done    it NEVER beeps forever into an empty room
// A tap during any live phase dismisses.
//
// Pure logic, epoch-second time — host-tested.
#pragma once
#include <stdint.h>

namespace canary::care {

enum class WakePhase : uint8_t { Idle, Ramp, Phase1, Gap, Phase2, Done };

class WakeAlarm {
 public:
  void set(int64_t at_epoch, int ramp_min, int phase2_after_s) {
    at_ = at_epoch;
    ramp_s_ = ramp_min > 0 ? ramp_min * 60 : 0;
    if (ramp_s_ > 3600) ramp_s_ = 3600;
    p2_after_s_ = phase2_after_s >= 60 ? phase2_after_s : 420;
    dismissed_ = false;
  }
  void clear() { at_ = 0; dismissed_ = false; }
  bool armed() const { return at_ != 0; }
  int64_t at() const { return at_; }
  int ramp_min() const { return ramp_s_ / 60; }
  int phase2_after_s() const { return p2_after_s_; }

  WakePhase phase(int64_t now) const {
    if (at_ == 0 || now < at_ - ramp_s_) return WakePhase::Idle;
    if (dismissed_ || now >= at_ + p2_after_s_ + PHASE2_MAX_S)
      return WakePhase::Done;
    if (now < at_) return WakePhase::Ramp;
    if (now < at_ + PHASE1_S) return WakePhase::Phase1;
    if (now < at_ + p2_after_s_) return WakePhase::Gap;
    return WakePhase::Phase2;
  }

  // 0..100 through the ramp window (100 from alarm time on).
  int ramp_pct(int64_t now) const {
    if (at_ == 0) return 0;
    // No-ramp alarm: light snaps to full at T (review catch: returning 0
    // here left the whole alarm at the dim floor).
    if (ramp_s_ == 0) return now >= at_ ? 100 : 0;
    const int64_t into = now - (at_ - ramp_s_);
    if (into <= 0) return 0;
    if (into >= ramp_s_) return 100;
    return (int)(into * 100 / ramp_s_);
  }

  // A tap during any live phase dismisses; returns true when consumed
  // (the caller should swallow the tap instead of navigating).
  bool tap(int64_t now) {
    const WakePhase p = phase(now);
    if (p == WakePhase::Idle || p == WakePhase::Done) return false;
    dismissed_ = true;
    return true;
  }

  static constexpr int PHASE1_S = 60;
  static constexpr int PHASE2_MAX_S = 600;

 private:
  int64_t at_ = 0;
  int ramp_s_ = 1200;      // 20 min default sunrise
  int p2_after_s_ = 420;   // Loftie-style ~7 min gentle-to-insistent gap
  bool dismissed_ = false;
};

}  // namespace canary::care
