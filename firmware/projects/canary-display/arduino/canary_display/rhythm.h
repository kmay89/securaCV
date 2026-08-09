#pragma once
#include <stdint.h>
#include <cstddef>
#include <stdio.h>

// Whole-home daily rhythm — the display learns when this home usually
// stirs in the morning and says, calmly, whether today looks like every
// other day (display_care_wave.md §4).
//
// The pattern is Alarm.com Wellness' green/yellow/red-versus-own-baseline
// (the strongest reassurance UX in the aging-in-place category), built the
// SecuraCV way: computed on this device from events the canaries already
// publish, persisted locally, never uploaded, never an alarm. The verdict
// is a wellbeing SURFACE — one line on the glass — because a display
// witnesses nothing and must not start diagnosing people.
//
//   Learning  first LEARN_DAYS mornings — says so, claims nothing
//   Routine   the home moved this morning, or nothing is due yet
//   Notable   quiet past the usual wake window ("worth a glance")
//   Unusual   quiet WELL past it ("worth a call") — the "long lie" case
//             every elder-care product exists to catch
//
// v1 learns one signal: FIRST STIR — the first Notice-class activity
// between NIGHT_END (03:00) and NOON, folded into an EWMA across days.
// Activity at 1 a.m. is late night, not morning; days with zero events
// (nobody home) are skipped, not learned as "this home never wakes".
//
// Pure C++, time injected as (day_of_year, minute_of_day) — host-tested.

namespace canary::care {

enum class Rhythm : uint8_t { Learning = 0, Routine, Notable, Unusual };

class RhythmModel {
 public:
  static constexpr int LEARN_DAYS = 3;
  static constexpr int NIGHT_END_MIN = 3 * 60;    // stirs before 03:00 are night
  static constexpr int MORNING_END_MIN = 12 * 60; // and after noon aren't "waking"
  static constexpr int NOTABLE_AFTER_MIN = 45;    // usual + 45 min -> Notable
  static constexpr int UNUSUAL_AFTER_MIN = 120;   // usual + 2 h  -> Unusual

  // Clock keeper: call every pass with local wall time (skip when the clock
  // is not yet valid). Handles day rollover — including folding yesterday's
  // first stir into the baseline.
  void tick(int day_of_year, int minute_of_day) {
    (void)minute_of_day;
    if (day_of_year < 0) return;
    if (today_doy_ == -1) {
      today_doy_ = day_of_year;
      return;
    }
    if (day_of_year != today_doy_) {
      // Fold the finished day. A day with no stir teaches nothing (empty
      // house != a home that never wakes).
      if (today_first_min_ >= 0) fold(today_first_min_);
      today_doy_ = day_of_year;
      today_first_min_ = -1;
    }
  }

  // Feed every Notice-class fleet activity event (presence/occupancy) with
  // local wall time. Cheap; call on event edges only.
  void on_activity(int day_of_year, int minute_of_day) {
    tick(day_of_year, minute_of_day);
    if (minute_of_day < NIGHT_END_MIN || minute_of_day >= MORNING_END_MIN)
      return;
    if (today_first_min_ < 0) today_first_min_ = minute_of_day;
  }

  Rhythm verdict(int minute_of_day) const {
    if (days_ < LEARN_DAYS) return Rhythm::Learning;
    if (today_first_min_ >= 0) return Rhythm::Routine;  // the home moved
    const int expected = expected_first_stir();
    if (expected < 0) return Rhythm::Learning;
    // Nothing yet today. Before the window opens there is nothing to say;
    // past it, escalate wording (never sound) with distance.
    if (minute_of_day < NIGHT_END_MIN) return Rhythm::Routine;  // small hours
    if (minute_of_day <= expected + NOTABLE_AFTER_MIN) return Rhythm::Routine;
    if (minute_of_day <= expected + UNUSUAL_AFTER_MIN) return Rhythm::Notable;
    return Rhythm::Unusual;
  }

  // Minute-of-day the home usually first stirs; -1 while unlearned.
  int expected_first_stir() const { return days_ > 0 ? (int)ewma_min_ : -1; }
  int today_first_stir() const { return today_first_min_; }
  int days_learned() const { return days_; }

  // The line the glass renders. Empty when there is nothing worth saying
  // (still learning day 0, or clock never valid).
  int line(char* buf, size_t cap, int minute_of_day) const {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    const Rhythm v = verdict(minute_of_day);
    const int exp = expected_first_stir();
    switch (v) {
      case Rhythm::Learning:
        if (days_ == 0 && today_first_min_ < 0) return 0;
        return snprintf(buf, cap, "Learning this home's rhythm • day %d of %d",
                        days_ + 1, (int)LEARN_DAYS);
      case Rhythm::Routine:
        if (today_first_min_ >= 0) {
          // U+F00C, the check LVGL bakes into its fonts (what LV_SYMBOL_OK
          // expands to) — spelled as bytes because this is a care header and
          // has no business including lvgl.h. It is NOT U+2713 CHECK MARK,
          // which lives outside the font's range and drew a hollow box here.
          return snprintf(buf, cap, "Morning rhythm \xEF\x80\x8C • first stir %02d:%02d",
                          today_first_min_ / 60, today_first_min_ % 60);
        }
        return 0;  // nothing due yet — silence is the calm answer
      case Rhythm::Notable:
        return snprintf(buf, cap, "Quiet past the usual wake (%02d:%02d)",
                        exp / 60, exp % 60);
      case Rhythm::Unusual:
      default:
        return snprintf(buf, cap, "Still quiet - well past usual (%02d:%02d)",
                        exp / 60, exp % 60);
    }
  }

  // ── NVS persistence (the baseline must survive a reboot) ──────────────
  struct Persist {
    uint32_t magic = MAGIC;
    uint16_t ewma_min = 0;
    uint8_t days = 0;
    uint8_t reserved = 0;
  };
  static constexpr uint32_t MAGIC = 0x314D5952;  // "RYM1"

  Persist save() const {
    Persist p;
    p.ewma_min = (uint16_t)(ewma_min_ < 0 ? 0 : ewma_min_);
    p.days = (uint8_t)(days_ > 200 ? 200 : days_);
    return p;
  }

  bool load(const Persist& p) {
    if (p.magic != MAGIC) return false;
    if (p.ewma_min >= 24 * 60) return false;
    ewma_min_ = p.days > 0 ? (int)p.ewma_min : -1;
    days_ = p.days;
    return true;
  }

 private:
  void fold(int first_min) {
    if (days_ == 0 || ewma_min_ < 0) {
      ewma_min_ = first_min;
    } else {
      // EWMA alpha 0.3 in integer math: new = old + 0.3*(sample-old).
      ewma_min_ += (3 * (first_min - ewma_min_)) / 10;
      if (ewma_min_ < 0) ewma_min_ = 0;
      if (ewma_min_ >= 24 * 60) ewma_min_ = 24 * 60 - 1;
    }
    if (days_ < 200) days_++;
  }

  int ewma_min_ = -1;
  int days_ = 0;
  int today_doy_ = -1;
  int today_first_min_ = -1;
};

}  // namespace canary::care
