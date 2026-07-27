/*
 * SecuraCV canary base — power-event boot glue.
 *
 * Feeds the shared, host-tested decision core
 * (firmware/common/power/power_events.h) the real signals this tree can collect
 * — esp_reset_reason(), an RTC-domain survival marker, the boot counter, and a
 * liveness heartbeat — then persists the outage log to NVS ("securacv") and
 * signs a witness record for a real power incident. The classification and log
 * arithmetic live in the pure core (proven by test_power_events.cpp); this file
 * is only the ESP-specific plumbing. See docs/design/power_events.md.
 *
 * Contract with the core: a Software reset is always intentional (esp_restart),
 * so only a power-on with no deliberate-shutdown flag is a restored outage. On
 * this tree nothing sets the clean-shutdown flag yet, so every power-on is
 * logged as a power loss — which is the honest record the user asked for (the
 * device cannot tell an outage from an unplug; both lost power).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef CANARY_POWER_EVENTS_H
#define CANARY_POWER_EVENTS_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

#include "power/power_events.h"  // the pure core (-I firmware/common)
#include "securacv_crypto.h"     // CborWriter, nvs_load_u32 (declared here)
#include "securacv_witness.h"    // witness_create_record, RecordType, WitnessRecord
#include "canary_config.h"       // NVS_KEY_BOOTS, NVS_MAIN_NS

namespace canary_pe {

inline constexpr uint32_t kRtcMarker  = 0x50574B31u;  // 'PWK1'
inline constexpr uint32_t kClockFloor = 1700000000u;  // time() below this = unset
inline constexpr uint32_t kHeartbeatMs = 300000u;     // 5 min (wear vs resolution)

// RTC-domain marker: survives a soft/brownout reset, lost on a true power loss.
// Namespace scope so it lands in the RTC no-init segment; no initializer.
inline RTC_NOINIT_ATTR uint32_t g_rtc_marker;

// Shared between on_boot() and witness_incident().
inline powerevents::BootPower g_boot = powerevents::BootPower::Unknown;
inline uint32_t g_outage_s = 0;
inline uint32_t g_boot_index = 0;

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

// Call ONCE, early in setup() (before risky init), so an outage is recorded
// even if a later init stage faults. Classifies the previous session's ending,
// appends it to the durable ring, persists, and prints a console line.
inline void on_boot() {
  const bool marker_present = (g_rtc_marker == kRtcMarker);
  g_rtc_marker = kRtcMarker;  // re-seed for the next boot

  Preferences pe;
  powerevents::Log log;
  bool had_log = false;
  bool clean = false;
  uint32_t last_alive = 0;
  if (pe.begin(NVS_MAIN_NS, /*readOnly=*/true)) {
    if (pe.getBytes("pe_log", &log, sizeof(log)) == sizeof(log) &&
        powerevents::log_valid(log)) {
      had_log = true;
    }
    clean = pe.getBool("pe_clean", false);
    last_alive = pe.getUInt("pe_alive", 0);
    pe.end();
  }
  if (!had_log) powerevents::log_init(log);

  const time_t nowt = time(nullptr);
  const uint32_t now_epoch = (nowt >= (time_t)kClockFloor) ? (uint32_t)nowt : 0;

  powerevents::Signals s;
  s.reset = reset_kind(esp_reset_reason());
  s.clean_shutdown = clean;
  s.rtc_marker_present = marker_present;
  s.have_prior_session = had_log;  // our own log is the prior-session witness
  g_boot = powerevents::classify(s);

  g_boot_index = nvs_load_u32(NVS_KEY_BOOTS, 0);  // best-effort label
  g_outage_s = powerevents::outage_bound_s(last_alive, now_epoch);

  powerevents::log_note(
      log, powerevents::make_event(g_boot, now_epoch, g_boot_index, g_outage_s));

  if (pe.begin(NVS_MAIN_NS, /*readOnly=*/false)) {
    pe.putBytes("pe_log", &log, sizeof(log));
    if (clean) pe.putBool("pe_clean", false);  // consume the deliberate-stop flag
    pe.end();
  }

  Serial.printf("[..] Power lineage: %s", powerevents::boot_power_name(g_boot));
  if (g_boot == powerevents::BootPower::OutageRestored && g_outage_s) {
    Serial.printf(" (outage >= %u s)", (unsigned)g_outage_s);
  }
  Serial.println();
}

// Call ONCE after witness provisioning: sign a record for a real incident
// (a restored outage or a brownout), so "the power went out" is verifiable.
inline void witness_incident() {
  if (!powerevents::is_power_incident(g_boot)) return;
  uint8_t payload[64];
  CborWriter cbor(payload, sizeof(payload));
  cbor.write_map(3);
  cbor.write_text("type");     cbor.write_text(powerevents::boot_power_wire(g_boot));
  cbor.write_text("boot");     cbor.write_uint(g_boot_index);
  cbor.write_text("outage_s"); cbor.write_uint(g_outage_s);
  WitnessRecord rec;
  witness_create_record(payload, cbor.size(), RECORD_STATE_CHANGE, &rec);
}

// Call every loop: persists the liveness heartbeat on a cadence — and only when
// the wall clock is actually set, so a board with no clock never writes a
// meaningless heartbeat. The heartbeat bounds the NEXT outage's lower-bound
// duration (now - last-seen-alive).
inline void heartbeat(uint32_t now_ms) {
  static uint32_t s_last_ms = 0;
  if ((int32_t)(now_ms - s_last_ms) < (int32_t)kHeartbeatMs) return;
  s_last_ms = now_ms;
  const time_t nowt = time(nullptr);
  if (nowt < (time_t)kClockFloor) return;  // no clock -> nothing to record
  Preferences pe;
  if (pe.begin(NVS_MAIN_NS, /*readOnly=*/false)) {
    pe.putUInt("pe_alive", (uint32_t)nowt);
    pe.end();
  }
}

}  // namespace canary_pe

#endif  // CANARY_POWER_EVENTS_H
