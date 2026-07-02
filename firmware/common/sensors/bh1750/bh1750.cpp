#include "bh1750.h"

#include <Arduino.h>
#include <Wire.h>

namespace securacv::sensors {

namespace {
constexpr uint8_t OP_POWER_ON  = 0x01;
constexpr uint8_t OP_RESET     = 0x07;
constexpr uint8_t OP_CONT_HRES = 0x10;

bool write_op(TwoWire& wire, uint8_t addr, uint8_t op) {
    wire.beginTransmission(addr);
    wire.write(op);
    return wire.endTransmission() == 0;
}
}  // namespace

bool BH1750::begin(TwoWire& wire, uint8_t addr) {
    wire_ = &wire;
    addr_ = addr;

    // POWER_ON doubles as the presence probe: a missing sensor NAKs.
    if (!write_op(wire, addr, OP_POWER_ON)) {
        present_ = false;
        return false;
    }
    write_op(wire, addr, OP_RESET);  // clear the data register (best-effort)
    present_ = write_op(wire, addr, OP_CONT_HRES);
    return present_;
}

float BH1750::read_lux() {
    if (!present_ || wire_ == nullptr) return -1.0f;

    if (wire_->requestFrom((int)addr_, 2) != 2) {
        return -1.0f;
    }
    const uint16_t raw = ((uint16_t)wire_->read() << 8) | (uint16_t)wire_->read();
    // Typical sensitivity: 1.2 counts per lux (datasheet).
    return (float)raw / 1.2f;
}

}  // namespace securacv::sensors
