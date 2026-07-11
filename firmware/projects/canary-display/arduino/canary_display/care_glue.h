#pragma once
#include <stdint.h>
#include <cstddef>

// Firmware glue for the care wave (display_care_wave.md): owns the
// AttentionPolicy / NightLedger / RhythmModel instances, drives the chime
// from the policy, harvests suppressed notices, fires escalation, applies
// persisted mutes once the clock is valid, and persists the rhythm
// baseline. main.cpp calls care_loop() once per pass; the UI reads the
// accessors. All decisions live in the pure headers — this file only
// wires them to time, NVS, the chime pin, and the broker.

#include "attention.h"

namespace canary::care {

void care_begin();

// Once per loop pass. `quiet` = quiet hours active; hh/mm/doy only valid
// when time_valid (SNTP synced).
void care_loop(uint32_t now, bool quiet, bool broker_up, bool time_valid,
               int hh, int mm, int day_of_year);

// ── UI surface ───────────────────────────────────────────────────────────

// Overnight ledger (morning summary). Cleared by the household ack or when
// the next night begins.
const NightLedger& night_ledger();

// Rhythm line for the glass; returns length (0 = nothing worth saying).
int rhythm_line(char* buf, size_t cap);

// Compute the "until morning" mute deadline: next CD_QUIET_END_HOUR as an
// epoch (0 when the clock isn't valid — caller falls back to a millis-only
// mute). Exposed for main's long-press routing.
uint32_t mute_until_morning_epoch();

}  // namespace canary::care
