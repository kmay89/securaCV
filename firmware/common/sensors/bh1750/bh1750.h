/**
 * @file bh1750.h
 * @brief Minimal driver for the ROHM BH1750 ambient-light sensor (I2C).
 *
 * Continuous high-resolution mode (1 lx resolution, ~120 ms conversion),
 * which is all the tamper-corroboration use case needs ("lights-out while
 * presence persists"). Board-agnostic per firmware/ARCHITECTURE.md: the
 * caller owns Wire.begin() with its board's pins and passes the bus in —
 * no pin numbers or feature flags here.
 *
 * Datasheet: ROHM BH1750FVI. Opcode subset used:
 *   0x01 POWER_ON, 0x07 RESET, 0x10 CONTINUOUS_HIGH_RES_MODE.
 * Raw-to-lux conversion: lux = raw / 1.2 (typical measurement accuracy).
 */

#pragma once

#include <stdint.h>

class TwoWire;

namespace securacv::sensors {

class BH1750 {
public:
    // Probe the sensor and start continuous high-resolution sampling.
    // `wire` must already be initialized (Wire.begin with board pins).
    // Returns false — and stays inert — when nothing ACKs at `addr`.
    bool begin(TwoWire& wire, uint8_t addr);

    // Latest reading in lux, or a negative value when the sensor is absent
    // or the read failed. Non-blocking beyond the I2C transaction; safe to
    // call at any cadence >= the ~120 ms conversion time.
    float read_lux();

    bool present() const { return present_; }

private:
    TwoWire* wire_    = nullptr;
    uint8_t  addr_    = 0;
    bool     present_ = false;
};

}  // namespace securacv::sensors
