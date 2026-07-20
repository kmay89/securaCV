#pragma once
#include <stdint.h>

// Dev Playground (FEATURE_PLAYGROUND, docs/hardware/dev_playground_43b.md):
// a guided, safe peripheral bench mode for the Waveshare 4.3B — the
// industrial-IO dash SKU whose terminal block (isolated DI/DO, I2C sensor
// header, RS485, CAN) is the only external wiring surface. The playground
// boots INSTEAD of the fleet face: no WiFi, no MQTT, no OTA — glass,
// terminal block, and the USB serial console only. main.cpp branches into
// playground_setup()/playground_loop() and never initializes the network
// stack, so a bench unit can't join the fleet by accident.
//
// Stations (one per bench test) map onto the terminal block:
//   Doorbell   DI0 — isolated input; press events + the DO0 "ding" link
//   Intrusion  DI1 — PIR / reed / laser break-beam receiver; gap timing
//   Chime      DO0 — isolated open-drain out; pulse / latch, auto-off
//   Strobe     DO1 — isolated open-drain out; pulse / latch, auto-off
//   Light      I2C — VEML7700 (0x10) or BH1750@0x5C; live lux
//   ToF        I2C — VL53L0X (0x29); mm ranging + trip-threshold counter
//   Cap touch  I2C — MPR121 (0x5A); 12-electrode bitmap + sensitivity
//                    presets for the printed shell-thickness test piece
//   I2C census — live bus scan with reserved-address warnings
//
// Serial protocol (PG1 — see the playground doc §Comms):
//   PG1 HELLO board=<id> fw=<ver>
//   PG1 <ms> EVT <station> <k>=<v>...      on every edge/trip/action
//   PG1 <ms> SNAP <k>=<v>...               1 Hz full snapshot

namespace canary::playground {

// ── Live model (driver writes, UI reads) ────────────────────────────────

struct DiChannel {
  bool     raw = false;       // raw expander bit (as read)
  bool     active = false;    // interpreted: input energized
  uint32_t events = 0;        // activation edges since boot
  uint32_t last_edge_ms = 0;  // millis() of the last edge (0 = never)
  uint32_t active_ms = 0;     // length of the current/last active phase
};

struct DoChannel {
  bool     sinking = false;   // conducting right now
  bool     latched = false;   // latched on (safety auto-off still runs)
  uint32_t auto_off_at_ms = 0;
  uint32_t pulses = 0;
};

struct LightState {
  bool    found = false;
  uint8_t addr = 0;           // 0x10 VEML7700 or 0x5C BH1750
  float   lux = 0.0f;
};

struct TofState {
  bool     found = false;
  bool     valid = false;     // last read in range
  uint16_t mm = 0;
  uint16_t trip_mm = 100;     // beam-gap threshold (cycled from the UI)
  uint32_t trips = 0;
  bool     tripped = false;
};

struct PadState {
  bool     found = false;
  uint16_t touched_bits = 0;  // MPR121 electrodes 0..11
  uint8_t  preset = 0;        // sensitivity preset index
  uint32_t events = 0;        // touch-down edges (any electrode)
};

struct I2cCensus {
  uint8_t count = 0;
  uint8_t addrs[16] = {0};
};

struct PgState {
  DiChannel di0, di1;
  DoChannel do0, do1;
  LightState light;
  TofState   tof;
  PadState   pad;
  I2cCensus  bus;
  bool       display_ok = false;
};

const PgState& state();

// ── UI -> driver actions (wired to on-glass buttons) ────────────────────
void action_do_pulse(int ch);         // 0/1: bounded 1.5 s pulse
void action_do_latch_toggle(int ch);  // 0/1: latch with 30 s safety auto-off
void action_pad_cycle_preset();       // contact -> 2 mm -> 4 mm -> max gain
void action_tof_cycle_trip();         // 50 -> 100 -> 200 -> 400 mm

const char* pad_preset_name(uint8_t idx);
const char* i2c_device_name(uint8_t addr);  // known-address lookup, "" unknown
bool i2c_addr_reserved(uint8_t addr);       // CH422G/GT911 command addresses

// ── Mode entry points (called from main.cpp under FEATURE_PLAYGROUND) ───
void playground_setup();
void playground_loop();

}  // namespace canary::playground
