/*
 * Host test for common/power/power_events.h — the power-lineage classifier and
 * the durable outage-log ring. Pins the full classification table (so a change
 * to how a boot's power story is named breaks CI, not a review), the correct
 * terminology, the honest lower-bound outage arithmetic, and the ring/counter
 * behavior the firmware persists.
 *
 * Build: see firmware/tests_host/Makefile (POWEREVT_BIN), -I ../common,
 *        -std=c++17 -Wall -Wextra -Werror.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "power/power_events.h"

using namespace powerevents;

static int g_checks = 0;
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    ++g_checks;                                                            \
    if (!(cond)) {                                                          \
      std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);        \
      std::exit(1);                                                         \
    }                                                                       \
  } while (0)

static Signals sig(ResetKind r, bool clean, bool rtc, bool prior = true) {
  Signals s;
  s.reset = r;
  s.clean_shutdown = clean;
  s.rtc_marker_present = rtc;
  s.have_prior_session = prior;
  return s;
}

static void test_cold_boot_wins_when_no_prior_session() {
  // A factory-fresh first boot can never be an "outage" — there is nothing to
  // have lost. This holds for EVERY reset cause.
  const ResetKind all[] = {ResetKind::Unknown, ResetKind::PowerOn,
                           ResetKind::Brownout, ResetKind::Software,
                           ResetKind::DeepSleepWake, ResetKind::Fault};
  for (ResetKind r : all) {
    CHECK(classify(sig(r, false, false, /*prior=*/false)) == BootPower::ColdBoot,
          "no prior session => cold boot regardless of reset cause");
  }
}

static void test_explicit_hardware_causes_reported_verbatim() {
  // Brownout and fault are unambiguous hardware causes — reported as-is even if
  // a clean-shutdown flag happens to be latched.
  CHECK(classify(sig(ResetKind::Brownout, false, false)) == BootPower::Brownout,
        "brownout reset => Brownout");
  CHECK(classify(sig(ResetKind::Brownout, true, true)) == BootPower::Brownout,
        "brownout outranks a stale clean flag");
  CHECK(classify(sig(ResetKind::Fault, false, true)) == BootPower::Fault,
        "watchdog/panic => Fault");
  CHECK(classify(sig(ResetKind::Fault, true, false)) == BootPower::Fault,
        "fault outranks a stale clean flag");
}

static void test_deep_sleep_is_always_a_clean_return() {
  CHECK(classify(sig(ResetKind::DeepSleepWake, false, false)) ==
            BootPower::CleanReboot,
        "an intended deep-sleep wake is never an outage");
}

static void test_power_on_needs_a_clean_flag_to_not_be_an_outage() {
  // The heart of the feature: a power-on reset with no clean stop is a restored
  // outage; with a clean stop (user powered off, then on) it is a clean reboot.
  CHECK(classify(sig(ResetKind::PowerOn, false, false)) ==
            BootPower::OutageRestored,
        "power-on with no clean stop => power restored (outage)");
  CHECK(classify(sig(ResetKind::PowerOn, true, false)) == BootPower::CleanReboot,
        "power-on after a deliberate power-off => clean reboot");
  // The RTC marker can't be present across a true power-on, but a caller that
  // sets it wrongly must not turn an unflagged outage into a reboot.
  CHECK(classify(sig(ResetKind::PowerOn, false, true)) ==
            BootPower::OutageRestored,
        "power-on: the clean flag, not the RTC hint, decides");
}

static void test_software_reset_is_always_intentional() {
  // esp_restart() is always our own code rebooting (OTA, user reset, config
  // apply) — never a power event. A real loss surfaces as PowerOn/Brownout.
  // So a Software reset is a clean reboot regardless of the flags.
  CHECK(classify(sig(ResetKind::Software, true, false)) ==
            BootPower::CleanReboot,
        "software reset with a clean flag => clean reboot");
  CHECK(classify(sig(ResetKind::Software, false, true)) ==
            BootPower::CleanReboot,
        "software reset, no flag, power held => clean reboot");
  CHECK(classify(sig(ResetKind::Software, false, false)) ==
            BootPower::CleanReboot,
        "software reset is intentional even with no flag and no marker "
        "(so an OTA/reboot is never mislabeled an outage)");
}

static void test_unknown_reset_stays_honest() {
  CHECK(classify(sig(ResetKind::Unknown, true, false)) == BootPower::CleanReboot,
        "unknown reset but clean flag => reboot");
  CHECK(classify(sig(ResetKind::Unknown, false, true)) == BootPower::Unknown,
        "unknown reset, power held, no clean flag => unknown (not asserted)");
  CHECK(classify(sig(ResetKind::Unknown, false, false)) ==
            BootPower::OutageRestored,
        "unknown reset with the RTC marker also lost => corroborated outage");
}

static void test_terminology_is_correct_and_total() {
  // Every enum value has a human name and a stable wire token, and never NULL.
  const BootPower all[] = {BootPower::Unknown, BootPower::ColdBoot,
                           BootPower::CleanReboot, BootPower::OutageRestored,
                           BootPower::Brownout, BootPower::Fault};
  for (BootPower k : all) {
    CHECK(boot_power_name(k) != nullptr && boot_power_name(k)[0] != '\0',
          "every lineage has a human name");
    CHECK(boot_power_wire(k) != nullptr && boot_power_wire(k)[0] != '\0',
          "every lineage has a wire token");
  }
  CHECK(std::strcmp(boot_power_name(BootPower::OutageRestored),
                    "power restored (outage)") == 0,
        "the outage name is stated the correct way");
  CHECK(std::strcmp(boot_power_name(BootPower::Brownout), "brownout reset") == 0,
        "a brownout is named a brownout, not an outage");
  CHECK(std::strcmp(boot_power_wire(BootPower::OutageRestored),
                    "power_restored") == 0,
        "outage wire token");

  // A power incident is an outage or a brownout — not a clean reboot or a crash.
  CHECK(is_power_incident(BootPower::OutageRestored), "outage is an incident");
  CHECK(is_power_incident(BootPower::Brownout), "brownout is an incident");
  CHECK(!is_power_incident(BootPower::CleanReboot), "clean reboot is not");
  CHECK(!is_power_incident(BootPower::ColdBoot), "cold boot is not");
  CHECK(!is_power_incident(BootPower::Fault), "a crash is a fault, not a loss");
}

static void test_outage_bound_is_an_honest_lower_bound() {
  CHECK(outage_bound_s(0, 100) == 0, "no last-alive clock => unknown");
  CHECK(outage_bound_s(100, 0) == 0, "no now clock => unknown");
  CHECK(outage_bound_s(100, 90) == 0, "now before last-alive => unknown, not negative");
  CHECK(outage_bound_s(90, 90) == 0, "same instant => zero");
  CHECK(outage_bound_s(1000, 1600) == 600, "600 s floor between heartbeat and boot");
}

static void test_make_event_only_carries_outage_for_an_outage() {
  Event e = make_event(BootPower::OutageRestored, 1600, 7, 600);
  CHECK(e.kind == (uint8_t)BootPower::OutageRestored && e.outage_s == 600,
        "an outage event keeps its bound");
  Event b = make_event(BootPower::Brownout, 1600, 7, 600);
  CHECK(b.outage_s == 0, "a brownout carries no outage duration");
}

static void test_log_init_and_validity() {
  Log L;
  log_init(L);
  CHECK(log_valid(L), "a freshly initialized log is valid");
  CHECK(L.count == 0 && L.total_outages == 0 && L.longest_outage_s == 0,
        "fresh log is empty with zero counters");

  Log bad{};  // all-zero: magic wrong -> rejected (uninitialized NVS blob)
  CHECK(!log_valid(bad), "an uninitialized blob is rejected");
  Log hi = L;
  hi.version = kLogVersion + 1;
  CHECK(!log_valid(hi), "a future version is rejected");
  Log oob = L;
  oob.head = kRingCap;
  CHECK(!log_valid(oob), "an out-of-range head is rejected");
}

static void test_ring_counts_wraps_and_reads_newest_first() {
  Log L;
  log_init(L);
  // Push kRingCap + 3 outages with rising epochs; the oldest 3 fall off.
  const size_t pushed = kRingCap + 3;
  for (size_t i = 0; i < pushed; i++) {
    log_note(L, make_event(BootPower::OutageRestored,
                           /*at_epoch=*/1000 + (uint32_t)i,
                           /*boot=*/(uint32_t)i, /*outage_s=*/10 + (uint32_t)i));
  }
  CHECK(L.count == kRingCap, "the ring holds exactly its capacity");
  CHECK(log_valid(L), "still valid after wrap");
  CHECK(L.total_outages == pushed, "counters are monotonic across the wrap");

  Event newest;
  CHECK(log_latest(L, newest), "latest is readable");
  CHECK(newest.at_epoch == 1000 + (uint32_t)(pushed - 1),
        "latest is the most recently noted event");

  Event oldest;
  CHECK(log_at(L, kRingCap - 1, oldest), "the oldest surviving event is readable");
  CHECK(oldest.at_epoch == 1000 + 3, "the first 3 events were overwritten");

  Event past_end;
  CHECK(!log_at(L, kRingCap, past_end), "reading past the ring returns false");
}

static void test_counters_track_the_right_axes() {
  Log L;
  log_init(L);
  log_note(L, make_event(BootPower::OutageRestored, 100, 1, 30));
  log_note(L, make_event(BootPower::OutageRestored, 500, 2, 900));  // longest
  log_note(L, make_event(BootPower::OutageRestored, 900, 3, 120));
  log_note(L, make_event(BootPower::Brownout, 950, 4, 0));
  log_note(L, make_event(BootPower::Fault, 960, 5, 0));
  log_note(L, make_event(BootPower::CleanReboot, 970, 6, 0));
  log_note(L, make_event(BootPower::ColdBoot, 0, 0, 0));

  CHECK(L.total_outages == 3, "three outages counted");
  CHECK(L.total_brownouts == 1, "one brownout counted");
  CHECK(L.total_faults == 1, "one fault counted");
  CHECK(L.longest_outage_s == 900, "longest outage tracked across events");
  CHECK(L.last_incident_epoch == 950,
        "last incident epoch is the brownout (latest outage-or-brownout)");
  // Clean reboots and cold boots move neither the incident counters nor epoch.
  CHECK(L.count == 7, "every event is stored in the ring regardless of kind");
}

// ── the words an operator actually acts on ──────────────────────────────────
//
// The 7" Dash blanked and reset in a loop. Dark screen, restart, repeat — from
// the outside, indistinguishable from a firmware crash. The board had already
// classified it correctly and only ever said "brownout reset", which is a name,
// not a remedy, so the hunt went to panel timings and RGB bounce buffers when
// the answer was amperage. These pin the difference.

static void test_a_brownout_names_the_power_supply() {
  // The hint IS the remedy. It has to carry a current rating — "try another
  // cable" is exactly what sends people in circles — and name the port that
  // usually can't deliver it.
  const std::string hint = boot_power_hint(BootPower::Brownout);
  CHECK(hint.find("5V") != std::string::npos, "hint must name the voltage");
  CHECK(hint.find("2A") != std::string::npos, "hint must name a current rating");
  CHECK(hint.find("USB") != std::string::npos, "hint must name the usual culprit");
  // No firmware change can fix an under-powered board, so this hint must never
  // send someone down the reflash path a crash would deserve.
  CHECK(hint.find("reflash") == std::string::npos,
        "never tell someone to reflash their way out of a power problem");
  CHECK(std::string(boot_power_hint(BootPower::Fault)).find("reflash") != std::string::npos,
        "a fault, by contrast, IS worth a reflash");
}

static void test_a_power_fault_warns_the_first_time() {
  // A crash might be a one-off; a brownout recurs on the very next transmit.
  CHECK(boot_power_should_warn(BootPower::Brownout, 0),
        "the FIRST brownout must warn — it will not resolve itself");
  CHECK(!boot_power_should_warn(BootPower::Fault, 0), "one crash is not a pattern");
  CHECK(!boot_power_should_warn(BootPower::Fault, 2), "two is still not a pattern");
  CHECK(boot_power_should_warn(BootPower::Fault, 3), "three consecutive faults is a report");
}

static void test_an_ordinary_boot_stays_quiet() {
  // Warn on every power-on and nobody reads the one that mattered.
  for (uint32_t n = 0; n < 50; ++n) {
    CHECK(!boot_power_should_warn(BootPower::ColdBoot, n), "cold boot never warns");
    CHECK(!boot_power_should_warn(BootPower::CleanReboot, n), "clean reboot never warns");
    CHECK(!boot_power_should_warn(BootPower::OutageRestored, n),
          "a restored outage is logged, not shouted — the power is back");
  }
}

static void test_operator_text_is_total_and_never_says_dead() {
  const BootPower all[] = {BootPower::ColdBoot, BootPower::CleanReboot,
                           BootPower::OutageRestored, BootPower::Brownout,
                           BootPower::Fault, BootPower::Unknown};
  for (BootPower k : all) {
    // A value that falls through a switch reaches the glass as a blank line,
    // which reads as a hung device — the very failure this text prevents.
    CHECK(boot_power_detail(k) && *boot_power_detail(k), "detail present");
    CHECK(boot_power_hint(k) && *boot_power_hint(k), "hint present");
    // The device is running well enough to display this. Copy implying a
    // corpse gets a working board thrown away.
    const std::string all_text = std::string(boot_power_detail(k)) + " " + boot_power_hint(k);
    CHECK(all_text.find("dead") == std::string::npos, "never calls the board dead");
    CHECK(all_text.find("broken") == std::string::npos, "never calls the board broken");
  }
}

static void test_ha_tamper_json_exact_payloads() {
  char buf[256];
  // A restored outage with a known floor carries it, verbatim.
  CHECK(ha_tamper_json(BootPower::OutageRestored, 120, buf, sizeof(buf)),
        "outage builds a payload");
  CHECK(std::string(buf) ==
            "{\"state\":\"on\",\"confidence\":1.00,\"type\":\"power_loss\","
            "\"severity\":\"warning\",\"detail\":\"Mains power came back after "
            "an outage. (outage >= 120 s)\"}",
        "outage payload is exact");
  // No clock, no floor: the duration claim is omitted, never invented.
  CHECK(ha_tamper_json(BootPower::OutageRestored, 0, buf, sizeof(buf)),
        "outage without a known floor still publishes");
  CHECK(std::string(buf).find("outage >=") == std::string::npos,
        "unknown duration is omitted, not fabricated");
  // Brownout and fault map to the HA tamper types the integration parses.
  CHECK(ha_tamper_json(BootPower::Brownout, 0, buf, sizeof(buf)),
        "brownout builds a payload");
  CHECK(std::string(buf).find("\"type\":\"power_loss\"") != std::string::npos,
        "brownout is a power_loss tamper");
  CHECK(ha_tamper_json(BootPower::Fault, 0, buf, sizeof(buf)),
        "fault builds a payload");
  CHECK(std::string(buf).find("\"type\":\"unexpected_reboot\"") != std::string::npos,
        "fault is an unexpected_reboot tamper");
  // The adapter route's gates need the truthy state and the confidence.
  CHECK(std::string(buf).find("\"state\":\"on\"") != std::string::npos,
        "payload passes the adapter's truthy gate");
  CHECK(std::string(buf).find("\"confidence\":1.00") != std::string::npos,
        "payload passes the adapter's confidence floor");
}

static void test_ha_tamper_json_stays_quiet_on_benign_boots() {
  char buf[256];
  // Every ordinary lineage publishes nothing — an unplug/replug during setup
  // is the OutageRestored case and DOES publish; a first boot does not.
  CHECK(!ha_tamper_json(BootPower::ColdBoot, 0, buf, sizeof(buf)),
        "cold boot publishes nothing");
  CHECK(!ha_tamper_json(BootPower::CleanReboot, 0, buf, sizeof(buf)),
        "clean reboot publishes nothing");
  CHECK(!ha_tamper_json(BootPower::Unknown, 0, buf, sizeof(buf)),
        "unknown lineage publishes nothing");
}

static void test_ha_tamper_json_refuses_truncation() {
  // A truncated JSON payload is worse than none: it fails to parse everywhere
  // and looks like corruption. The builder must say no, not emit half.
  char tiny[24];
  CHECK(!ha_tamper_json(BootPower::Brownout, 0, tiny, sizeof(tiny)),
        "too-small buffer returns false");
  char nothing[1];
  CHECK(!ha_tamper_json(BootPower::OutageRestored, 3600, nothing, sizeof(nothing)),
        "one-byte buffer returns false");
}

int main() {
  test_a_brownout_names_the_power_supply();
  test_a_power_fault_warns_the_first_time();
  test_an_ordinary_boot_stays_quiet();
  test_operator_text_is_total_and_never_says_dead();
  test_cold_boot_wins_when_no_prior_session();
  test_explicit_hardware_causes_reported_verbatim();
  test_deep_sleep_is_always_a_clean_return();
  test_power_on_needs_a_clean_flag_to_not_be_an_outage();
  test_software_reset_is_always_intentional();
  test_unknown_reset_stays_honest();
  test_terminology_is_correct_and_total();
  test_outage_bound_is_an_honest_lower_bound();
  test_make_event_only_carries_outage_for_an_outage();
  test_log_init_and_validity();
  test_ring_counts_wraps_and_reads_newest_first();
  test_counters_track_the_right_axes();
  test_ha_tamper_json_exact_payloads();
  test_ha_tamper_json_stays_quiet_on_benign_boots();
  test_ha_tamper_json_refuses_truncation();

  std::printf("ALL POWER-EVENTS TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
