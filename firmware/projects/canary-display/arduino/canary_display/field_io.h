// Field I/O — promote the 4.3B's isolated inputs/outputs into the dash runtime.
//
// Reads the two isolated digital inputs (a door/window contact on DI0, a tamper
// contact on DI1) and turns their edges into local fleet events; drives the
// isolated open-drain output DO0 as a siren/strobe while the fleet stands in an
// unacknowledged alert. The decision logic (debounce, bounded siren) is the
// host-tested pure core in canary/io/field_io_logic.h; this is the thin runtime
// that talks to the CH422G expander and the fleet model.
//
// IMPORTANT provenance rule: the dash has no signing identity, so a contact read
// is reported as an honestly UNSIGNED local event (signed_flag=false) — the same
// footing as the fleet model's on_chirp/on_beacon observations. It is never
// dressed up as a signed witness event.
//
// Active only on boards that declare HAS_ISOLATED_IO (Waveshare 4.3B). On any
// other board the implementation is an empty translation unit and every caller
// is compiled out, so nothing ships to the watch / plain-4.3 dash / emulator.

#pragma once

#include <cstdint>

namespace canary {
namespace io {

// Cache the reporter id (this dash's device_id) used to tag local contact
// events, and reset the input/siren state. Call once at boot.
void field_io_begin(const char* self_id);

// Poll the isolated inputs (debounced) into unsigned local fleet events and
// drive the siren output from the fleet's unacked-alert state. `now` is
// millis(); safe (and cheap) to call every loop pass — it self-throttles.
void field_io_loop(uint32_t now);

// Siren arming (opt-in). Disarmed by default: an unacked alert still shows on
// the glass and lands in the journal, but the isolated output (DO0) stays
// silent until the user arms it from Settings. This is deliberately safe — the
// DO sink polarity is VERIFY-tagged in pins.h, so a mis-wired output never
// drives until someone opts in on real hardware. The flag persists in NVS, so a
// reboot keeps the household's choice. `field_io_armed()` reflects the current
// state for the settings row; both are no-ops unless HAS_ISOLATED_IO.
void field_io_set_armed(bool armed);
bool field_io_armed();

}  // namespace io
}  // namespace canary
