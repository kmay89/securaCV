// src/care/bird_glue.cpp — living-canary wiring. See bird_glue.h.
#include <config.h>

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <Preferences.h>

#include "canary/care/bird_glue.h"
#include "canary/care/bird_mood.h"
#include "canary/fleet/fleet_instance.h"
#include "canary/net/wifi_mgr.h"
#include "canary/net/mqtt_mgr.h"

namespace canary::care {

namespace {

BirdMood s_mood;
bool s_loaded = false;
uint32_t s_last_minute_ms = 0;
int s_last_yday = -1;

// Hub-flap tracking: link drops observed inside the rolling hour.
uint8_t s_drops = 0;
uint32_t s_drop_window_ms = 0;
bool s_was_connected = false;

// When the current Warn+ condition started standing unacknowledged.
uint32_t s_warn_since_ms = 0;

void load() {
  s_loaded = true;
  Preferences p;
  if (!p.begin("scv-bird", /*readOnly=*/true)) return;
  s_mood.trust_days = p.getUShort("trust", 0);
  s_last_yday = p.getInt("yday", -1);
  p.end();
}

void persist(int yday) {
  Preferences p;
  if (!p.begin("scv-bird", /*readOnly=*/false)) return;
  p.putUShort("trust", s_mood.trust_days);
  p.putInt("yday", yday);
  p.end();
}

}  // namespace

canary::ui::CanaryMood bird_mood_tick(uint32_t now_ms, bool night,
                                      bool time_valid, int yday) {
  using canary::fleet::Sev;
  if (!s_loaded) load();
  auto& fleet = canary::fleet::the_fleet();

  // Hub-link flap counter: more than one drop in the hour = flapping.
  const bool hub_up = canary::net::mqtt_connected();
  // Unsigned elapsed math (review catch): these are since-timestamps that
  // can legitimately go weeks between resets, and a signed cast flips
  // negative past 24.8 days and freezes the window forever.
  if (s_drop_window_ms == 0 || now_ms - s_drop_window_ms >= 3600000UL) {
    s_drop_window_ms = now_ms;
    s_drops = 0;
  }
  if (s_was_connected && !hub_up && s_drops < 255) s_drops++;
  s_was_connected = hub_up;

  BirdInputs in = {};
  const int n = fleet.count();
  for (int i = 0; i < n; i++) {
    const auto* w = fleet.at(i);
    if (!w) continue;
    const Sev sv = fleet.witness_sev(*w, now_ms);
    if (sv == Sev::Warn && in.stale_witnesses < 255) {
      in.stale_witnesses++;
    } else if (sv >= Sev::Alert && in.lost_witnesses < 255) {
      in.lost_witnesses++;
    }
  }
  const Sev worst = fleet.worst(now_ms);
  const bool acked = fleet.ack_active(now_ms);
  in.links_down = !canary::net::wifi_connected() || !hub_up;
  in.hub_flapping = s_drops >= 2;
  // Trouble nobody has answered for over 12 h (seconds domain).
  if (worst >= Sev::Warn && !acked) {
    if (s_warn_since_ms == 0) s_warn_since_ms = now_ms;
    in.unacked_old =
        (now_ms - s_warn_since_ms) / 1000 >= 12UL * 3600UL ? 1 : 0;
  } else {
    s_warn_since_ms = 0;
  }
  in.all_verified = n > 0 && worst == Sev::Ok && fleet.all_verified() &&
                    !in.links_down;
  in.night = night;
  in.alarm_unacked = worst >= Sev::Alert && !acked;

  // One engine minute per real minute; the local-day rollover advances the
  // trust ladder. Trust persists ONLY at rollover (write-light — and a
  // reboot deliberately forgives a dirty morning rather than burning a
  // flash write per incident).
  if (s_last_minute_ms == 0 ||
      now_ms - s_last_minute_ms >= 60000UL) {
    s_last_minute_ms = now_ms;
    bird_mood_minute(s_mood, in);
    if (time_valid && yday >= 0 && yday != s_last_yday) {
      if (s_last_yday >= 0) bird_mood_rollover(s_mood);
      s_last_yday = yday;
      persist(yday);
    }
  }

  canary::ui::canary_mark_trust(s_mood.trust_days);
  switch (bird_face(s_mood, in)) {
    case BirdFace::Hidden:     return canary::ui::CanaryMood::Hidden;
    case BirdFace::Asleep:     return canary::ui::CanaryMood::Asleep;
    case BirdFace::Worried:    return canary::ui::CanaryMood::Worried;
    case BirdFace::Distressed: return canary::ui::CanaryMood::Distressed;
    default:                   return canary::ui::CanaryMood::Idle;
  }
}

}  // namespace canary::care
