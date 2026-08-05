/*
 * SecuraCV — Power-event lineage + outage log (pure, host-testable)
 *
 * The honest *decision* half of "harden against brownouts, power flickers, and
 * outages, and keep a correct log of when the power went out." A device that
 * has just lost power cannot record the instant it died — it is unpowered. So
 * this core does the one thing a device truthfully can: on the NEXT boot it
 * classifies how the *previous* power session ended, from signals the boot glue
 * collects, and appends a correctly-named event to a small durable ring.
 *
 * The vocabulary is deliberate (the difference matters to a user):
 *   - COLD BOOT        first-ever power-up; no prior session to compare against.
 *   - CLEAN REBOOT     the predecessor stopped on purpose — an OTA update, a
 *                      user reset, an intended power-off, a deep-sleep wake.
 *                      NOT a power incident.
 *   - BROWNOUT         the supply SAGGED below the chip's brownout detector and
 *                      reset it. A dip, not necessarily a full outage.
 *   - OUTAGE RESTORED  a power-on reset whose predecessor did NOT stop cleanly:
 *                      mains was lost while the device ran, and has now come
 *                      back. We log "power restored; last seen alive at T", and
 *                      an outage duration that is an explicit LOWER BOUND
 *                      (now - last_alive) — the device may have died any time
 *                      after T, so we never fabricate an exact loss instant.
 *   - FAULT            a watchdog/panic reset — a crash, surfaced here so the
 *                      ring shows it, but counted separately from power loss.
 *
 * Signals the glue supplies (all cheap, all already available in the tree):
 *   reset              esp_reset_reason() mapped onto ResetKind
 *   clean_shutdown     an NVS flag the last session set before an intended
 *                      stop (graceful shutdown / reboot intent); cleared at the
 *                      start of every normal run, so if it is still set the last
 *                      stop was deliberate.
 *   rtc_marker_present a magic value kept in RTC-domain NOINIT memory. It
 *                      survives a soft/brownout reset but not a true power loss,
 *                      so its ABSENCE corroborates a real outage. A secondary
 *                      hint only — it breaks the tie on an UNKNOWN-cause reset,
 *                      and never overrides an explicit reset cause. Boards
 *                      without the marker pass false.
 *   have_prior_session boot_count > 0 (or any persisted heartbeat) — false only
 *                      on a factory-fresh first boot.
 *   last_alive_epoch   the newest heartbeat timestamp the running device
 *                      persisted (wall clock from RTC/SNTP), 0 if no clock.
 *   now_epoch          wall clock at this boot, 0 if no clock yet.
 *
 * No Arduino/ESP-IDF dependencies and no hidden state: classify() is a pure
 * function of its Signals, and the Log is a trivially-copyable POD the glue can
 * persist as raw NVS bytes (exactly like power_history_t). test_power_events.cpp
 * (run in CI, -Wall -Wextra -Werror) pins the whole classification table, the
 * terminology, the ring/counter behavior, and the outage-bound arithmetic, so
 * the recovery behavior is proven, not reviewed.
 *
 * Consumed by every firmware via -I firmware/common as "power/power_events.h"
 * (canary base + the PlatformIO projects); the Arduino sketch trees stage a
 * byte-identical copy guarded by a sync check, the same contract power_logic.h
 * documents.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_POWER_EVENTS_H
#define SECURACV_POWER_EVENTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace powerevents {

// ── Reset cause, reduced to what the power story needs ──────────────────────
// The glue maps esp_reset_reason() onto this so the core needs no ESP headers.
// (ESP_RST_POWERON→PowerOn; ESP_RST_BROWNOUT→Brownout; ESP_RST_SW/EXT→Software;
//  ESP_RST_DEEPSLEEP→DeepSleepWake; ESP_RST_PANIC/INT_WDT/TASK_WDT/WDT→Fault;
//  ESP_RST_UNKNOWN and anything else→Unknown.)
enum class ResetKind : uint8_t {
  Unknown = 0,
  PowerOn,        // the chip powered up from fully unpowered
  Brownout,       // supply sagged below the brownout detector
  Software,       // esp_restart(): OTA, user reset, intended reboot
  DeepSleepWake,  // woke from an intended deep sleep
  Fault,          // watchdog or panic — a crash
};

// ── The classified lineage of THIS boot (what the ring records) ─────────────
enum class BootPower : uint8_t {
  Unknown = 0,
  ColdBoot,        // first-ever power-up, nothing to compare against
  CleanReboot,     // predecessor stopped on purpose — not a power incident
  OutageRestored,  // mains was lost while running, now restored
  Brownout,        // supply dipped below the detector and reset the chip
  Fault,           // watchdog/panic reset (a crash, surfaced but not "power")
};

// Human-readable, correctly-named — for console lines, telemetry, on-glass.
// Never returns NULL.
inline const char* boot_power_name(BootPower k) {
  switch (k) {
    case BootPower::ColdBoot:       return "cold boot";
    case BootPower::CleanReboot:    return "clean reboot";
    case BootPower::OutageRestored: return "power restored (outage)";
    case BootPower::Brownout:       return "brownout reset";
    case BootPower::Fault:          return "fault reset";
    case BootPower::Unknown:        return "unknown";
  }
  return "unknown";
}

// A short wire token for signed/telemetry payloads (stable, machine-readable).
inline const char* boot_power_wire(BootPower k) {
  switch (k) {
    case BootPower::ColdBoot:       return "cold_boot";
    case BootPower::CleanReboot:    return "clean_reboot";
    case BootPower::OutageRestored: return "power_restored";
    case BootPower::Brownout:       return "brownout";
    case BootPower::Fault:          return "fault_reset";
    case BootPower::Unknown:        return "unknown";
  }
  return "unknown";
}

// Is this boot's lineage a real power incident worth alerting/logging as such?
// Outage and brownout are; a clean reboot, a cold first boot, and a crash are
// not (a crash is a fault, tracked on its own axis).
inline bool is_power_incident(BootPower k) {
  return k == BootPower::OutageRestored || k == BootPower::Brownout;
}

// ── What to tell the person standing in front of it ─────────────────────────
//
// boot_power_name() above is the LOG name; these two are the OPERATOR's. The
// gap between them cost real time: a 7" Dash blanked and reset in a loop, and
// every observable symptom — dark screen, restart, repeat — was identical to a
// firmware crash. The board had classified the reset correctly the whole time
// and only ever said "brownout reset", which is not a remedy. The investigation
// went to panel timings and RGB bounce buffers; the answer was amperage.
//
// A brownout is the one cause where NO firmware change can help, so it must be
// unmistakable. An under-powered board is not a broken board, and telling
// someone to reflash it wastes their evening and convinces them the hardware
// is faulty.

// One sentence of plain explanation. Never implies a dead board — the device is
// running well enough to say this.
inline const char* boot_power_detail(BootPower k) {
  switch (k) {
    case BootPower::ColdBoot:       return "Started up normally.";
    case BootPower::CleanReboot:    return "Restarted on purpose — an update or a settings change.";
    case BootPower::OutageRestored: return "Mains power came back after an outage.";
    case BootPower::Brownout:       return "The supply dipped and the board reset itself to protect what it had saved.";
    case BootPower::Fault:          return "The last run hit a fault and restarted itself.";
    case BootPower::Unknown:        return "Restarted for a reason this board didn't recognize.";
  }
  return "Restarted for a reason this board didn't recognize.";
}

// What to actually DO. The brownout text is the whole point: it names a CURRENT
// rating, because "try another cable" sends people in circles when the answer is
// amps, and it names the port that usually can't deliver — the larger screens
// draw most of a laptop port's budget before WiFi has transmitted anything.
inline const char* boot_power_hint(BootPower k) {
  switch (k) {
    case BootPower::ColdBoot:
    case BootPower::CleanReboot:
    case BootPower::OutageRestored:
      return "Nothing to do.";
    case BootPower::Brownout:
      return "Use a 5V supply rated for at least 2A. A laptop or monitor USB "
             "port often can't deliver enough, especially on the larger screens "
             "once WiFi starts transmitting.";
    case BootPower::Fault:
    case BootPower::Unknown:
      return "If this keeps happening, reflash from the released firmware.";
  }
  return "If this keeps happening, reflash from the released firmware.";
}

// Whether to put this on the glass rather than only in the log.
//
// A brownout warns on the FIRST occurrence: unlike a crash it will not resolve
// itself, and every retry is another brownout — waiting for a third means two
// more resets and an operator who has already decided the board is dead. A
// fault waits, because one crash after a yanked cable is noise and three in a
// row is a report. Ordinary boots never warn: a device that shouts on every
// power-on trains people to ignore it, and the one message that mattered goes
// unread with the rest.
//
// `consecutive` counts resets of the same troubling kind since the last clean
// run.
inline bool boot_power_should_warn(BootPower k, uint32_t consecutive) {
  if (k == BootPower::Brownout) return true;
  if (k == BootPower::Fault)    return consecutive >= 3;
  return false;
}

// ── Signals in, lineage out ─────────────────────────────────────────────────
struct Signals {
  ResetKind reset             = ResetKind::Unknown;
  bool      clean_shutdown    = false;  // the last stop was deliberate
  bool      rtc_marker_present = false; // RTC-domain magic survived (power held)
  bool      have_prior_session = false; // not a factory-fresh first boot
};

// THE per-boot classification. Pure function of its Signals.
//
// Order of judgment:
//   · No prior session at all → ColdBoot (nothing to have lost).
//   · Brownout / Fault resets are explicit hardware causes → reported verbatim.
//   · DeepSleepWake is always an intended return → CleanReboot.
//   · A Software reset (esp_restart) is always our own code deciding to reboot —
//     OTA, a user reset, a config apply — never a power event. A true power loss
//     surfaces as PowerOn or Brownout, so Software is unconditionally CleanReboot.
//   · A PowerOn whose predecessor stopped cleanly (clean_shutdown latched by a
//     graceful power-off) → CleanReboot; otherwise → OutageRestored (ran, lost
//     mains, came back).
//   · An Unknown cause with no clean stop: only call it an outage if the RTC
//     marker was ALSO lost (a corroborated power loss); otherwise stay honest.
inline BootPower classify(const Signals& s) {
  if (!s.have_prior_session) return BootPower::ColdBoot;

  switch (s.reset) {
    case ResetKind::Brownout:      return BootPower::Brownout;
    case ResetKind::Fault:         return BootPower::Fault;
    case ResetKind::DeepSleepWake: return BootPower::CleanReboot;
    case ResetKind::Software:      return BootPower::CleanReboot;
    case ResetKind::PowerOn:
      return s.clean_shutdown ? BootPower::CleanReboot
                              : BootPower::OutageRestored;
    case ResetKind::Unknown:
    default:
      if (s.clean_shutdown) return BootPower::CleanReboot;
      return s.rtc_marker_present ? BootPower::Unknown
                                  : BootPower::OutageRestored;
  }
}

// Lower-bound outage seconds from the last heartbeat to now. Both must be real
// wall-clock (non-zero) and ordered; otherwise the duration is unknown (0).
// This is a floor, never an exact figure — the device may have died any time
// after last_alive_epoch.
inline uint32_t outage_bound_s(uint32_t last_alive_epoch, uint32_t now_epoch) {
  if (last_alive_epoch == 0 || now_epoch == 0) return 0;
  if (now_epoch <= last_alive_epoch) return 0;
  return now_epoch - last_alive_epoch;
}

// ── MQTT tamper egress (the securacv/<id>/tamper payload) ───────────────────
//
// The boot classification as the one JSON object every downstream consumer of
// the tamper topic parses today:
//   - the HA integration's per-type sensors key on "type" ("power_loss" /
//     "unexpected_reboot") and render "detail"/"severity" as attributes;
//   - its aggregate tamper sensor reads "type" and "detail";
//   - the host mqtt_sensor adapter's tamper route gates on the truthy "state"
//     and the "confidence" floor, and ignores the rest.
// Returns false for a benign boot (cold boot, clean reboot, unknown): those
// publish nothing — silence over noise. Confidence is 1.0 because the reset
// cause is hardware-reported, not inferred from a sensor reading. A restored
// outage carries its honest lower-bound duration when one is known.
inline bool ha_tamper_json(BootPower k, uint32_t outage_s, char* out,
                           size_t cap) {
  const char* type = nullptr;
  switch (k) {
    case BootPower::OutageRestored:
    case BootPower::Brownout:
      type = "power_loss";
      break;
    case BootPower::Fault:
      type = "unexpected_reboot";
      break;
    default:
      return false;
  }
  int n;
  if (k == BootPower::OutageRestored && outage_s > 0) {
    n = snprintf(out, cap,
                 "{\"state\":\"on\",\"confidence\":1.00,\"type\":\"%s\","
                 "\"severity\":\"warning\",\"detail\":\"%s (outage >= %u s)\"}",
                 type, boot_power_detail(k), (unsigned)outage_s);
  } else {
    n = snprintf(out, cap,
                 "{\"state\":\"on\",\"confidence\":1.00,\"type\":\"%s\","
                 "\"severity\":\"warning\",\"detail\":\"%s\"}",
                 type, boot_power_detail(k));
  }
  return n > 0 && (size_t)n < cap;
}

// ── The durable ring log (POD; persisted as raw NVS bytes) ──────────────────
inline constexpr uint32_t kLogMagic  = 0x50574556u; // 'PWEV'
inline constexpr uint8_t  kLogVersion = 1;
inline constexpr size_t   kRingCap    = 16; // last N power events kept

struct Event {
  uint8_t  kind;        // BootPower
  uint8_t  _pad[3];
  uint32_t at_epoch;    // wall clock of the boot that logged it (0 if no clock)
  uint32_t boot_index;  // boot counter at the event (monotonic fallback)
  uint32_t outage_s;    // lower-bound outage seconds (0 unless OutageRestored)
};

struct Log {
  uint32_t magic;
  uint8_t  version;
  uint8_t  count;   // events stored (<= kRingCap)
  uint8_t  head;    // next write slot
  uint8_t  _pad;
  uint32_t total_outages;
  uint32_t total_brownouts;
  uint32_t total_faults;
  uint32_t longest_outage_s;
  uint32_t last_incident_epoch;
  Event    ring[kRingCap];
};

inline void log_init(Log& L) {
  L = Log{};
  L.magic = kLogMagic;
  L.version = kLogVersion;
}

// Is a blob read back from NVS a well-formed log (vs uninitialized/foreign)?
inline bool log_valid(const Log& L) {
  return L.magic == kLogMagic && L.version <= kLogVersion &&
         L.count <= kRingCap && L.head < kRingCap;
}

inline Event make_event(BootPower k, uint32_t at_epoch, uint32_t boot_index,
                        uint32_t outage_s) {
  Event e{};
  e.kind = static_cast<uint8_t>(k);
  e.at_epoch = at_epoch;
  e.boot_index = boot_index;
  e.outage_s = (k == BootPower::OutageRestored) ? outage_s : 0;
  return e;
}

// Append an event and fold it into the aggregate counters. Circular: once the
// ring is full the oldest event is overwritten, but the counters are monotonic
// and outlive the ring.
inline void log_note(Log& L, const Event& e) {
  L.ring[L.head] = e;
  L.head = static_cast<uint8_t>((L.head + 1) % kRingCap);
  if (L.count < kRingCap) L.count++;

  switch (static_cast<BootPower>(e.kind)) {
    case BootPower::OutageRestored:
      L.total_outages++;
      L.last_incident_epoch = e.at_epoch;
      if (e.outage_s > L.longest_outage_s) L.longest_outage_s = e.outage_s;
      break;
    case BootPower::Brownout:
      L.total_brownouts++;
      L.last_incident_epoch = e.at_epoch;
      break;
    case BootPower::Fault:
      L.total_faults++;
      break;
    default:
      break;
  }
}

// Read the ring newest-first: i==0 is the most recent event. Returns false when
// i is past the number stored. `head-1-i` walked modulo the capacity.
inline bool log_at(const Log& L, size_t i, Event& out) {
  if (i >= L.count) return false;
  size_t idx = (static_cast<size_t>(L.head) + kRingCap - 1 - i) % kRingCap;
  out = L.ring[idx];
  return true;
}

inline bool log_latest(const Log& L, Event& out) { return log_at(L, 0, out); }

}  // namespace powerevents

#endif  // SECURACV_POWER_EVENTS_H
