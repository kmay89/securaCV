#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
//   RS485      A/B — Modbus RTU probe of a slave's holding register
//   CAN bus    H/L — CAN 2.0 / TWAI: transmit a test frame, count RX
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

struct Rs485State {
  bool     ready = false;     // Serial1 brought up on A/B
  uint32_t polls = 0;         // PROBE requests sent
  uint32_t replies = 0;       // valid Modbus responses parsed
  uint16_t last_val = 0;      // last holding-register value read
  bool     last_ok = false;   // last probe got a good reply
  // The last probe's Modbus exception code, -1 when it got none (a good
  // reply, or no reply). Wider than the code's byte because every byte is a
  // code on the wire: the station takes a CRC-good exception carrying 0 as
  // one ("slave=1 exception=0"), so 0 cannot also mean "none".
  int16_t  last_exc = -1;
};

struct CanState {
  bool     ready = false;     // TWAI installed + started on H/L
  uint32_t tx = 0;            // test frames transmitted
  uint32_t rx = 0;            // frames received
  uint32_t last_id = 0;       // id of the last received frame
};

struct PgState {
  DiChannel di0, di1;
  DoChannel do0, do1;
  LightState light;
  TofState   tof;
  PadState   pad;
  Rs485State rs485;
  CanState   can;
  I2cCensus  bus;
  bool       display_ok = false;
};

const PgState& state();

// ── UI -> driver actions (wired to on-glass buttons) ────────────────────
void action_do_pulse(int ch);         // 0/1: bounded 1.5 s pulse
void action_do_latch_toggle(int ch);  // 0/1: latch with 30 s safety auto-off
void action_pad_cycle_preset();       // contact -> 2 mm -> 4 mm -> max gain
void action_tof_cycle_trip();         // 50 -> 100 -> 200 -> 400 mm
void action_rs485_probe();            // Modbus RTU: read holding reg 0 of slave 1
void action_can_send();               // transmit one CAN test frame

const char* pad_preset_name(uint8_t idx);
const char* i2c_device_name(uint8_t addr);  // known-address lookup, "" unknown
bool i2c_addr_reserved(uint8_t addr);       // CH422G/GT911 command addresses

// ── The RS485 station's words on the glass (F317) ───────────────────────
// What the pin tracker's RS485 row and the station's tile say about the last
// probe, from the station state alone. Pure (no LVGL): playground_ui.cpp
// draws them, and tests_host/test_fieldbus_glue.cpp holds them, in all three
// builds, to the state each probe scenario leaves. A Modbus exception is
// named ("exc 2 • check register" on the row, "exception 2" on the tile): the
// slave answered and refused the register, and "no reply • check A/B" would
// send the bench to wiring that works. The row is narrow: its value column
// has about 145 px of Montserrat 12 before the tracker panel's edge (LVGL
// 8.4's glyph widths), where "exc 255 • check register" is 142 px and
// "exception 2 • check the register" 192, which the edge would cut short;
// the tile and its detail panel have room for the whole word.
enum Rs485Look : uint8_t { RS485_LOOK_IDLE, RS485_LOOK_OK, RS485_LOOK_WARN };

// The row: its text into `out`, and its look.
inline Rs485Look rs485_row_words(const Rs485State& s, char* out, size_t cap) {
  if (s.last_exc >= 0) {
    snprintf(out, cap, "exc %u • check register", (unsigned)s.last_exc);
    return RS485_LOOK_WARN;
  }
  if (s.replies) {
    snprintf(out, cap, "RTU • reg0=%u", (unsigned)s.last_val);
    return RS485_LOOK_OK;
  }
  snprintf(out, cap, "%s", s.polls ? "no reply • check A/B" : "Modbus • tap to probe");
  return s.polls ? RS485_LOOK_WARN : RS485_LOOK_IDLE;
}

// The tile: its value into `val`, its sub line into `sub`, and its look.
inline Rs485Look rs485_tile_words(const Rs485State& s, char* val, size_t val_cap,
                                  char* sub, size_t sub_cap) {
  if (s.last_ok) {
    snprintf(val, val_cap, "%u", (unsigned)s.last_val);
    snprintf(sub, sub_cap, "%s", "reg0 • slave 1");
    return RS485_LOOK_OK;
  }
  snprintf(val, val_cap, "%s", s.polls ? "?" : "-");
  if (s.last_exc >= 0) {
    snprintf(sub, sub_cap, "exception %u", (unsigned)s.last_exc);
  } else {
    snprintf(sub, sub_cap, "%s", s.polls ? "no reply" : "tap to probe");
  }
  return s.polls ? RS485_LOOK_WARN : RS485_LOOK_IDLE;
}

// ── Mode entry points (called from main.cpp under FEATURE_PLAYGROUND) ───
void playground_setup();
void playground_loop();

}  // namespace canary::playground
