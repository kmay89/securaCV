// Field I/O runtime — see canary/io/field_io.h.
//
// Gated on HAS_ISOLATED_IO (the 4.3B board map). On any other board this is an
// empty translation unit; every caller in main.cpp is under the same gate, so no
// stubs are needed and nothing links on the watch / plain-4.3 dash / emulator.

#include "pins.h"  // HAS_ISOLATED_IO, ISO_IN_BIT_*, ISO_OUT_BIT_* (board -I path)

#if defined(HAS_ISOLATED_IO) && HAS_ISOLATED_IO

#include <Arduino.h>
#include <Preferences.h>
#include <config.h>
#include <string.h>

#include "canary/io/field_io.h"
#include "canary/io/field_io_logic.h"
#include "canary/hal/display.h"
#include "canary/fleet/fleet_instance.h"
#include "canary/log.h"

namespace canary {
namespace io {

namespace {
namespace fio = canary::io::field;
using canary::fleet::Sev;
using canary::fleet::the_fleet;

// Poll cadence: gentle (20 Hz) — each read briefly flips the CH422G to input
// mode, so a bounded rate keeps the panel control lines from flickering
// (pins.h VERIFY note). 2-sample debounce rejects contact bounce.
constexpr uint32_t POLL_MS = 50;
constexpr uint8_t DEBOUNCE_N = 2;
// A driven siren self-releases after this long so a standing alert can't blare
// forever (see SirenController); re-arms once the alert clears or is acked.
constexpr uint32_t SIREN_MAX_ON_MS = 300000;  // 5 min

// Default contact meanings for the two isolated inputs. The severity follows
// the event name through classify_event(): "tamper_contact" -> Sev::Tamper (an
// alert that can sound the siren), "door_contact" -> Sev::Notice (informational).
constexpr const char* DI0_EVENT = "door_contact";
constexpr const char* DI1_EVENT = "tamper_contact";

char s_self_id[48] = {0};
bool s_ready = false;
uint32_t s_next_poll_ms = 0;
fio::Debounce s_di0, s_di1;
fio::SirenController s_siren;
bool s_siren_driving = false;
bool s_armed = false;  // opt-in: the siren stays silent until the user arms it

// Siren-arm persistence. Its own NVS namespace/key (not the glass settings
// blob) keeps this field-I/O concern self-contained on the 4.3B — no versioned
// blob migration, and nothing leaks into the watch/plain-dash/emulator builds
// that don't compile this TU.
constexpr const char* ARM_NS  = "scv-field";
constexpr const char* ARM_KEY = "siren_arm";

// A contact just went active -> report it as an UNSIGNED local event.
void report_contact(const char* event_name, uint32_t now) {
  the_fleet().on_event(s_self_id, event_name, /*signed_flag=*/false, now);
  log_line("FIELD", event_name);
}
}  // namespace

void field_io_begin(const char* self_id) {
  strlcpy(s_self_id, self_id ? self_id : "", sizeof(s_self_id));
  s_di0 = fio::Debounce{};
  s_di1 = fio::Debounce{};
  s_siren = fio::SirenController{};
  s_siren_driving = false;
  // Restore the household's arm choice (default disarmed if never set / no NVS).
  Preferences p;
  if (p.begin(ARM_NS, /*readOnly=*/true)) {
    s_armed = p.getBool(ARM_KEY, false);
    p.end();
  } else {
    s_armed = false;
  }
  s_next_poll_ms = 0;
  s_ready = true;
}

bool field_io_armed() { return s_armed; }

void field_io_set_armed(bool armed) {
  s_armed = armed;
  Preferences p;
  if (p.begin(ARM_NS, /*readOnly=*/false)) {
    p.putBool(ARM_KEY, armed);
    p.end();
  }
  log_line("FIELD", armed ? "siren armed" : "siren disarmed");
}

void field_io_loop(uint32_t now) {
  if (!s_ready) return;
  if ((int32_t)(now - s_next_poll_ms) < 0) return;
  s_next_poll_ms = now + POLL_MS;

  // ── Inputs: debounce, then a rising (contact-active) edge -> local event.
  uint8_t bits = 0;
  if (canary::hal::expander_read_inputs(&bits)) {
    // Isolated inputs are active-LOW through the optocoupler: the EXIO bit
    // reads 0 when the contact is energized/closed.
    const bool a0 = (bits & ISO_IN_BIT_DI0) == 0;
    const bool a1 = (bits & ISO_IN_BIT_DI1) == 0;
    if (s_di0.update(a0, DEBOUNCE_N) == fio::Edge::Rising) report_contact(DI0_EVENT, now);
    if (s_di1.update(a1, DEBOUNCE_N) == fio::Edge::Rising) report_contact(DI1_EVENT, now);
  }
  // A failed read reports nothing (expander_read_inputs is fail-closed) — a bus
  // glitch must never manufacture a contact edge.

  // ── Output: drive DO0 as a siren while the fleet stands in an unacked alert
  // AND the user has armed it. Disarmed, the alert still shows/journals; only
  // the physical output stays silent (field_io_logic treats !armed as resolved).
  const bool alerting = (int)the_fleet().worst(now) >= (int)Sev::Alert;
  const bool acked = the_fleet().ack_active(now);
  const bool drive =
      s_siren.update(now, alerting, acked, SIREN_MAX_ON_MS, s_armed);
  if (drive != s_siren_driving) {  // only touch the bus on a change
    canary::hal::expander_od_set(ISO_OUT_BIT_DO0, /*sink=*/drive);
    s_siren_driving = drive;
    log_line("FIELD", drive ? "siren on (unacked alert)" : "siren off");
  }
}

}  // namespace io
}  // namespace canary

#endif  // HAS_ISOLATED_IO
