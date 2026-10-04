// CAN / TWAI transport for the 4.3B H/L terminal.
//
// Thin device layer over the pure frame core (canary/io/can_frame.h): it owns
// the ESP32-S3 TWAI controller on the CAN pins and the bounded tx/rx timing; the
// core owns frame validity, filtering, and formatting. The dedicated CAN
// transceiver on this board coexists with native USB (unlike the plain 4.3),
// and an on-board 120 Ohm terminator is jumper-selectable (OFF by default).
//
// Gated by FEATURE_CAN (default 0). Compiled only in the canary-display-dash-can
// env; the default and emulator builds see an empty translation unit, so the
// wasm stays byte-for-byte identical. Bench-pending: TX/RX orientation, bit
// timing, and the terminator jumper are VERIFY-tagged in pins.h until validated
// on hardware.

#pragma once

#include <cstdint>

#include "can_frame.h"

namespace canary {
namespace io {

// Transport-level outcomes. can_transmit()/can_receive() return 0 on success or
// one of these (all negative) on failure.
enum : int {
  CAN_OK = 0,
  CAN_ERR_TIMEOUT = -20,    // no tx slot / no frame within the budget
  CAN_ERR_NOT_READY = -21,  // can_begin() hasn't run
  CAN_ERR_ARG = -22,        // malformed frame (bad id/dlc) or bad bitrate
  CAN_ERR_BUS = -23,        // driver/bus fault (bus-off, queue error)
};

// Install + start the TWAI driver on the CAN terminal at `bitrate` bit/s (one of
// can::BITRATE_125K/250K/500K/1M). Accept-all filter, normal mode. Idempotent.
// Returns false on a bad bitrate or if the driver won't start.
bool can_begin(uint32_t bitrate);

// True once can_begin() has succeeded.
bool can_ready();

// Queue `f` for transmission, waiting up to `timeout_ms` for a free slot.
// Validates the frame first (id width + dlc). Returns CAN_OK or a negative code.
int can_transmit(const can::Frame& f, uint32_t timeout_ms);

// Receive one frame into `*out`, waiting up to `timeout_ms`. Returns CAN_OK (and
// fills *out), CAN_ERR_TIMEOUT if the bus was quiet, or another negative code.
int can_receive(can::Frame* out, uint32_t timeout_ms);

// Optional bench heartbeat: drains any received frames and logs them via
// can::format_frame. Provided for a future setup()/loop() wiring once the bus is
// bench-validated (kept out of the shipping loop for now). No-op until
// can_begin() runs. `now` is millis(); safe to call every loop.
void can_loop(uint32_t now);

}  // namespace io
}  // namespace canary
