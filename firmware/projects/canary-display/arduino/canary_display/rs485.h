// RS485 / Modbus RTU master transport for the 4.3B A/B terminal.
//
// Thin device layer over the pure core (canary/io/modbus_rtu.h): it owns
// Serial1 on the RS485 pins and the request→response timing; the core owns the
// framing + CRC. Auto-direction transceiver on this board — there is no
// driver-enable pin to toggle (see pins.h "RS485 — auto direction control").
//
// Gated by FEATURE_RS485 (default 0). It shares GPIO43/44 with the CH343
// USB-UART console, so a build that enables it must keep logging on the native
// USB CDC. Compiled only in the canary-display-dash-rs485 env; the default and
// emulator builds see an empty translation unit, so the wasm stays byte-for-byte
// identical. Bench-pending: TX/RX orientation and bus timing are VERIFY-tagged
// in pins.h until validated on hardware.

#pragma once

#include <cstddef>
#include <cstdint>

namespace canary {
namespace io {

// Transport-level outcomes (kept distinct from modbus::Status, which is used
// for framing/CRC faults). rs485_read_holding() returns a modbus::Status
// (negative) or a register count (>= 0) on a completed exchange, and one of
// these only when the exchange itself couldn't complete.
enum : int {
  RS485_ERR_TIMEOUT = -20,   // response didn't arrive within the budget
  RS485_ERR_NOT_READY = -21, // rs485_begin() hasn't run
  RS485_ERR_ARG = -22,       // count is 0 or exceeds the register buffer
};

// Bring Serial1 up on the RS485 terminal at `baud` (default RS485_BAUD_DEFAULT).
// Idempotent. Returns false if the UART couldn't start.
bool rs485_begin(uint32_t baud);

// True once rs485_begin() has succeeded.
bool rs485_ready();

// Read `count` holding registers (fn 0x03) from `slave` starting at `start`,
// into `regs` (capacity `cap`), waiting up to `timeout_ms` for the reply.
// Returns the register count on success, or a negative modbus::Status /
// RS485_ERR_* on failure. Non-blocking beyond the bounded wait; never allocates.
int rs485_read_holding(uint8_t slave, uint16_t start, uint16_t count,
                       uint16_t* regs, size_t cap, uint32_t timeout_ms);

// Optional bench heartbeat: periodically polls a probe register and logs the
// result. Provided for a future setup()/loop() wiring once the bus is
// bench-validated (kept out of the shipping loop for now). No-op until
// rs485_begin() runs. `now` is millis(); safe to call every loop.
void rs485_loop(uint32_t now);

}  // namespace io
}  // namespace canary
