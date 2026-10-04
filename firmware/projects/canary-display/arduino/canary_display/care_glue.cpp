// src/care/care_glue.cpp — care-wave wiring (see header).
//
// Bundled-library includes sit ABOVE the feature gate on purpose: PlatformIO's
// lib_ldf_mode=deep+ evaluates preprocessor conditionals from build flags
// only, and FEATURE_CARE lives in the flavor config HEADER (via -I), so an
// include hidden behind the gate would vanish from dependency resolution
// (the #857 LittleFS / #860 WebServer lesson, third verse).
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

#include "flavor_config.h"
#if defined(FEATURE_CARE) && FEATURE_CARE

#include "care_glue.h"
#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
#include "rhythm.h"
#endif
#include "config.h"
#include "glass_settings.h"
#include "log.h"
#include "fleet_instance.h"
#include "mute_store.h"
#include "chime.h"
#include "mqtt_mgr.h"

namespace canary::care {

namespace {

AttentionPolicy s_policy;
NightLedger s_ledger;
#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
RhythmModel s_rhythm;
int s_rhythm_saved_doy = -1;
uint32_t s_last_event_seen_ms = 0;
#endif

bool s_prev_quiet = false;
bool s_prev_acked = false;
bool s_mutes_applied = false;

// Per-witness Warn-edge scan for the overnight ledger. Runs at 1 Hz during
// quiet hours only. Tracking is per slot, NOT fleet-wide: a second witness
// going stale while the first still holds the fleet's worst at Warn must
// still land in the morning summary (review catch). The mask self-heals on
// slot reuse — worst case is a duplicate note, which the ledger dedups.
uint32_t s_warn_mask = 0;
uint32_t s_last_scan_ms = 0;

void ledger_scan_warns(uint32_t now) {
  using canary::fleet::Sev;
  if ((int32_t)(now - s_last_scan_ms) < 1000) return;
  s_last_scan_ms = now;
  auto& fleet = canary::fleet::the_fleet();
  const time_t epoch = time(nullptr);
  const uint32_t e = epoch > 1700000000 ? (uint32_t)epoch : 0;
  const int n = fleet.count();
  for (int i = 0; i < n && i < 32; i++) {
    const auto* w = fleet.at(i);
    if (!w) continue;
    const uint32_t bit = 1UL << i;
    if (fleet.witness_sev(*w, now) == Sev::Warn) {
      if (!(s_warn_mask & bit)) {
        s_warn_mask |= bit;
        s_ledger.note(canary::fleet::Fleet::display_name(*w), warn_reason(*w),
                      e);
      }
    } else {
      s_warn_mask &= ~bit;
    }
  }
}

#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
void rhythm_persist_if_due(int day_of_year) {
  if (day_of_year < 0 || day_of_year == s_rhythm_saved_doy) return;
  s_rhythm_saved_doy = day_of_year;
  const RhythmModel::Persist p = s_rhythm.save();
  Preferences prefs;
  if (prefs.begin("scv-care", /*readOnly=*/false)) {
    prefs.putBytes("rhythm", &p, sizeof(p));
    prefs.end();
  }
}
#endif

}  // namespace

void care_begin() {
#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
  Preferences prefs;
  if (prefs.begin("scv-care", /*readOnly=*/true)) {
    RhythmModel::Persist p{};
    if (prefs.getBytes("rhythm", &p, sizeof(p)) == sizeof(p)) {
      if (s_rhythm.load(p)) {
        log_header("CARE");
        canary::dbg_serial().printf(
            "Rhythm baseline restored: %d day(s), first stir ~%02d:%02d\n",
            s_rhythm.days_learned(),
            s_rhythm.expected_first_stir() / 60,
            s_rhythm.expected_first_stir() % 60);
      }
    }
    prefs.end();
  }
#endif
}

void care_loop(uint32_t now, bool quiet, bool broker_up, bool time_valid,
               int hh, int mm, int day_of_year) {
  using canary::fleet::Sev;
  auto& fleet = canary::fleet::the_fleet();
  const Sev worst = fleet.worst(now);
  const bool acked = fleet.ack_active(now);

  // ── One-time: re-apply persisted mutes once both clocks exist ──
  if (!s_mutes_applied && time_valid) {
    s_mutes_applied = true;
    const time_t epoch = time(nullptr);
    canary::fleet::mute_store_apply(now, (uint32_t)epoch);
  }

  // ── Sound: the attention policy owns every edge (spec §2) ──
  {
    const SoundDecision d =
        s_policy.decide(worst, acked, quiet, now, CD_CHIME_REVOICE_MS);
    switch (d.sound) {
      case Sound::Tier1:
        canary::hal::chime_play(canary::hal::Chime::Tier1Alarm, d.ramp);
        break;
      case Sound::Tier2:
        canary::hal::chime_play(canary::hal::Chime::Tier2Warn, d.ramp);
        break;
      case Sound::AllClear:
        canary::hal::chime_play(canary::hal::Chime::AllClear, d.ramp);
        break;
      case Sound::None:
      default:
        break;
    }
  }
  canary::hal::chime_loop(now);

  // ── Ledger lifecycle: night begins fresh; the household ack clears it
  //    (reading the morning summary IS the acknowledgment). During quiet
  //    hours a per-witness scan records every Warn edge the policy
  //    silenced. ──
  if (quiet && !s_prev_quiet) {
    s_ledger.clear();
    s_warn_mask = 0;  // witnesses already at Warn when night falls count too
  }
  if (acked && !s_prev_acked) s_ledger.clear();
  s_prev_quiet = quiet;
  s_prev_acked = acked;
  if (quiet) ledger_scan_warns(now);

  // ── Escalation-on-no-ack (spec §5). Deliverability gates the latch: a
  //    deadline crossed while the broker is down (or the clock invalid)
  //    escalates the moment sending becomes possible, instead of being
  //    silently consumed (review catch). ──
  const time_t esc_epoch = time(nullptr);
  const bool can_send = broker_up && esc_epoch > 1700000000;
  if (s_policy.escalation_due(worst, acked, now, ESCALATE_UNACKED_MS,
                              can_send)) {
    // Name the witness that is worst right now — the escalation names a
    // place, not a payload.
    const char* who = "";
    const int n = fleet.count();
    for (int i = 0; i < n; i++) {
      const auto* w = fleet.at(i);
      if (w && fleet.witness_sev(*w, now) == worst) {
        who = canary::fleet::Fleet::display_name(*w);
        break;
      }
    }
    canary::net::publish_fleet_escalation(
        (uint32_t)esc_epoch, canary::fleet::sev_name(worst), who);
    log_line("CARE", "Escalation published: Tier-1 ran unacked past deadline.");
  }

#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
  // ── Rhythm: clock keeper + feed Notice-class activity edges (spec §4) ──
  if (time_valid) {
    const int mod = hh * 60 + mm;
    s_rhythm.tick(day_of_year, mod);
    const auto* e = fleet.event_at(0);
    if (e && e->at_ms != s_last_event_seen_ms) {
      s_last_event_seen_ms = e->at_ms;
      if (e->sev == Sev::Notice) s_rhythm.on_activity(day_of_year, mod);
    }
    rhythm_persist_if_due(day_of_year);
  }
#else
  (void)hh; (void)mm; (void)day_of_year; (void)time_valid;
#endif
}

const NightLedger& night_ledger() { return s_ledger; }

int rhythm_line(char* buf, size_t cap) {
#if defined(FEATURE_RHYTHM) && FEATURE_RHYTHM
  time_t t = time(nullptr);
  if (t < 1700000000) { if (buf && cap) buf[0] = '\0'; return 0; }
  struct tm lt;
  localtime_r(&t, &lt);
  return s_rhythm.line(buf, cap, lt.tm_hour * 60 + lt.tm_min);
#else
  if (buf && cap) buf[0] = '\0';
  return 0;
#endif
}

uint32_t mute_until_morning_epoch() {
  time_t t = time(nullptr);
  if (t < 1700000000) return 0;
  struct tm lt;
  localtime_r(&t, &lt);
  struct tm morning = lt;
  // "Morning" is the RUNTIME night-end hour (settings wave) — a mute made
  // under edited night hours must expire with them, not with the compiled
  // 07:00 seed (review catch: the display would re-alert mid-night or stay
  // muted into the user's configured day).
  morning.tm_hour = canary::glass::settings().night_end_hh;
  morning.tm_min = 0;
  morning.tm_sec = 0;
  time_t m = mktime(&morning);
  if (m <= t) m += 24 * 3600;  // that hour already passed — tomorrow's
  return (uint32_t)m;
}

}  // namespace canary::care

#endif  // FEATURE_CARE
