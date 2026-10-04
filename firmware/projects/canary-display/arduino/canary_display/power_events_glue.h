/*
 * SecuraCV canary-display — power-event boot glue.
 *
 * Feeds the shared, host-tested decision core
 * (firmware/common/power/power_events.h) the signals this tree can collect —
 * esp_reset_reason(), an RTC-domain survival marker, and a liveness
 * heartbeat — then persists the outage log to NVS ("securacv"). The display
 * witnesses nothing itself (no signing kernel in this tree), so the base
 * tree's witness_incident() call has no counterpart here: the record is the
 * durable ring plus the health flags Home Assistant parses.
 *
 * Differences from firmware/canary/include/canary_power_events.h, on purpose:
 *  - no witness record (this tree is verify-only; see src/main.cpp's banner);
 *  - the boot index is a local "pe_boots" counter (this tree has no
 *    NVS_KEY_BOOTS);
 *  - pe_clean is stored via getUChar/putUChar — the display's Preferences
 *    shim documents that getBool/putBool may be absent (src/hal/chime.cpp).
 *
 * Wall-clock honesty: heartbeat() only persists when time(nullptr) is set.
 * On flavors without FEATURE_RTC or SNTP the classifier still works from
 * boot lineage alone; outage durations read "unknown", never invented.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef CANARY_DISPLAY_POWER_EVENTS_GLUE_H
#define CANARY_DISPLAY_POWER_EVENTS_GLUE_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

#include "power_events.h"  // the pure core (-I firmware/common)

namespace cd_pe {

inline constexpr uint32_t kRtcMarker = 0x50574B31u;   // 'PWK1'
inline constexpr uint32_t kClockFloor = 1700000000u;  // time() below this = unset
inline constexpr uint32_t kHeartbeatMs = 300000u;     // 5 min (wear vs resolution)
inline constexpr uint32_t kIncidentHoldMs = 3600000u; // 1 h health-flag hold

// RTC-domain marker: survives a soft/brownout reset, lost on a true power
// loss. Namespace scope so it lands in the RTC no-init segment.
inline RTC_NOINIT_ATTR uint32_t g_rtc_marker;

inline powerevents::BootPower g_boot = powerevents::BootPower::Unknown;
inline uint32_t g_outage_s = 0;

inline powerevents::ResetKind reset_kind(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return powerevents::ResetKind::PowerOn;
    case ESP_RST_BROWNOUT:  return powerevents::ResetKind::Brownout;
    case ESP_RST_DEEPSLEEP: return powerevents::ResetKind::DeepSleepWake;
    case ESP_RST_SW:        return powerevents::ResetKind::Software;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return powerevents::ResetKind::Fault;
    default:                return powerevents::ResetKind::Unknown;
  }
}

// Call ONCE, early in setup() — before the mode-latch early-return, so a
// bench/demo boot still records its lineage. Classifies the previous
// session's ending, appends it to the durable ring, persists.
inline void on_boot() {
  const bool marker_present = (g_rtc_marker == kRtcMarker);
  g_rtc_marker = kRtcMarker;  // re-seed for the next boot

  Preferences pe;
  powerevents::Log log;
  bool had_log = false;
  bool clean = false;
  uint32_t last_alive = 0;
  uint32_t boots = 0;
  if (pe.begin("securacv", /*readOnly=*/true)) {
    if (pe.getBytes("pe_log", &log, sizeof(log)) == sizeof(log) &&
        powerevents::log_valid(log)) {
      had_log = true;
    }
    clean = pe.getUChar("pe_clean", 0) != 0;
    last_alive = pe.getUInt("pe_alive", 0);
    boots = pe.getUInt("pe_boots", 0);
    pe.end();
  }
  if (!had_log) powerevents::log_init(log);

  const time_t nowt = time(nullptr);
  const uint32_t now_epoch = (nowt >= (time_t)kClockFloor) ? (uint32_t)nowt : 0;

  powerevents::Signals s;
  s.reset = reset_kind(esp_reset_reason());
  s.clean_shutdown = clean;
  s.rtc_marker_present = marker_present;
  s.have_prior_session = had_log;
  g_boot = powerevents::classify(s);
  g_outage_s = powerevents::outage_bound_s(last_alive, now_epoch);

  powerevents::log_note(
      log, powerevents::make_event(g_boot, now_epoch, boots + 1, g_outage_s));

  if (pe.begin("securacv", /*readOnly=*/false)) {
    pe.putBytes("pe_log", &log, sizeof(log));
    pe.putUInt("pe_boots", boots + 1);
    if (clean) pe.putUChar("pe_clean", 0);  // consume the deliberate-stop flag
    pe.end();
  }

  Serial.printf("[..] Power lineage: %s", powerevents::boot_power_name(g_boot));
  if (g_boot == powerevents::BootPower::OutageRestored && g_outage_s) {
    Serial.printf(" (outage >= %u s)", (unsigned)g_outage_s);
  }
  Serial.println();
}

// The hold, latched: once lapsed it never re-arms, so the 32-bit millis()
// wrap (~49.7 days) cannot resurrect an hour of stale power flags.
inline bool hold_active(uint32_t now_ms) {
  static bool s_expired = false;
  if (s_expired) return false;
  if (now_ms >= kIncidentHoldMs) {
    s_expired = true;
    return false;
  }
  return true;
}

// True while the retained health payload should carry power_loss_detected.
// Health repeats on a cadence, so a hub that boots after this display still
// sees the incident; HA's health parse clears the sensor when the hold ends.
inline bool health_power_flag(uint32_t now_ms) {
  return powerevents::is_power_incident(g_boot) && hold_active(now_ms);
}

// True while it should carry unexpected_reboot (a fault-reset lineage).
inline bool health_fault_flag(uint32_t now_ms) {
  return g_boot == powerevents::BootPower::Fault && hold_active(now_ms);
}

// Call every loop pass, above the mode-latch early-return: persists the
// liveness heartbeat on a cadence, and only when the wall clock is actually
// set — a clock-less flavor never writes a meaningless heartbeat.
inline void heartbeat(uint32_t now_ms) {
  static uint32_t s_last_ms = 0;
  if ((int32_t)(now_ms - s_last_ms) < (int32_t)kHeartbeatMs) return;
  s_last_ms = now_ms;
  const time_t nowt = time(nullptr);
  if (nowt < (time_t)kClockFloor) return;  // no clock -> nothing to record
  Preferences pe;
  if (pe.begin("securacv", /*readOnly=*/false)) {
    pe.putUInt("pe_alive", (uint32_t)nowt);
    pe.end();
  }
}

}  // namespace cd_pe

#endif  // CANARY_DISPLAY_POWER_EVENTS_GLUE_H
