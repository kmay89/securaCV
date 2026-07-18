// src/care/wake_glue.cpp — wake alarm wiring: NVS persistence, MQTT config,
// two-phase chime, gamma backlight sunrise. See wake_glue.h / wake_alarm.h.
#include "flavor_config.h"

// LDF lesson: bundled-library includes stay ABOVE feature gates.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

#if defined(FEATURE_WAKE_ALARM) && FEATURE_WAKE_ALARM

#include "wake_glue.h"
#include "wake_alarm.h"
#include "chime.h"
#include "log.h"

namespace canary::care {

namespace {
WakeAlarm s_alarm;
uint32_t s_last_chirp_ms = 0;
// The `at` of the last dismissed firing, persisted: a reboot or a broker
// reconnect REPLAYS the retained config mid-window, and a dismissed alarm
// must stay dismissed (review catch). A new `at` value re-arms normally.
int64_t s_dismissed_at = 0;
constexpr const char* NVS_NS = "scv-wake";

int64_t epoch_now() {
  const time_t t = time(nullptr);
  return t > 1700000000 ? (int64_t)t : 0;  // no valid clock, no alarm
}

void persist() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putLong64("at", s_alarm.at());
  p.putInt("ramp", s_alarm.ramp_min());
  p.putInt("p2", s_alarm.phase2_after_s());
  p.putLong64("dis", s_dismissed_at);
  p.end();
}

void arm(int64_t at, int ramp_min, int p2_after_s) {
  if (at != 0 && at == s_dismissed_at) {
    // Same firing we already dismissed: stay quiet.
    s_alarm.clear();
    return;
  }
  s_alarm.set(at, ramp_min, p2_after_s);
}
}  // namespace

void wake_alarm_init() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;
  const int64_t at = p.getLong64("at", 0);
  const int ramp = p.getInt("ramp", 20);
  const int p2 = p.getInt("p2", 420);
  s_dismissed_at = p.getLong64("dis", 0);
  p.end();
  if (at != 0 && at != s_dismissed_at) {
    s_alarm.set(at, ramp, p2);
    canary::log_line("WAKE", "Alarm restored from storage.");
  }
}

void wake_alarm_on_config(const char* payload, unsigned len) {
  if (len == 0) {  // retained clear = alarm off
    s_alarm.clear();
    persist();
    canary::log_line("WAKE", "Alarm cleared.");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;
  const int64_t at = doc["at"] | (int64_t)0;
  if (at == 0) {
    s_alarm.clear();
  } else {
    arm(at, doc["ramp_min"] | 20, doc["phase2_after_s"] | 420);
  }
  persist();
  canary::log_line("WAKE", at ? "Alarm set." : "Alarm cleared.");
}

void wake_alarm_loop(uint32_t now_ms) {
  const int64_t now = epoch_now();
  if (now == 0) return;
  switch (s_alarm.phase(now)) {
    case WakePhase::Phase1:
      // Soft ascending chirp every ~25 s — wake the brain, not the house.
      if ((int32_t)(now_ms - s_last_chirp_ms) >= 25000) {
        canary::hal::chime_play(canary::hal::Chime::Sunrise, /*ramp=*/0);
        s_last_chirp_ms = now_ms;
      }
      break;
    case WakePhase::Phase2:
      // Insistent: mid-loudness pulses every 4 s. Never the Tier-1 alarm
      // grammar — a morning must not sound like an intruder.
      if ((int32_t)(now_ms - s_last_chirp_ms) >= 4000) {
        canary::hal::chime_play(canary::hal::Chime::Tier2Warn, /*ramp=*/2);
        s_last_chirp_ms = now_ms;
      }
      break;
    default:
      break;
  }
}

int wake_alarm_backlight() {
  const int64_t now = epoch_now();
  if (now == 0) return -1;
  const WakePhase p = s_alarm.phase(now);
  if (p != WakePhase::Ramp && p != WakePhase::Phase1 && p != WakePhase::Gap &&
      p != WakePhase::Phase2)
    return -1;
  // Gamma 2.2 keeps the dawn perceptually linear: a linear duty ramp looks
  // like a light switch in the first minutes and a plateau after.
  const float t = s_alarm.ramp_pct(now) / 100.0f;
  const int level = (int)(255.0f * powf(t, 2.2f));
  return level < 4 ? 4 : level;  // floor: visibly "beginning" from the start
}

bool wake_alarm_tap() {
  const int64_t now = epoch_now();
  if (now == 0) return false;
  if (!s_alarm.tap(now)) return false;
  s_dismissed_at = s_alarm.at();
  persist();  // dismissed survives reboot AND retained-config replay
  canary::log_line("WAKE", "Alarm dismissed by tap.");
  return true;
}

}  // namespace canary::care

#endif  // FEATURE_WAKE_ALARM
