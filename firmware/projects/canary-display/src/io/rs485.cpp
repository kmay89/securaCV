// RS485 / Modbus RTU master transport — see canary/io/rs485.h.
//
// The whole translation unit is behind FEATURE_RS485 so the default and
// emulator builds compile it to nothing (byte-neutral wasm). No stubs are
// needed: every caller in main.cpp is under the same gate.

#include <config.h>

#if defined(FEATURE_RS485) && FEATURE_RS485

#include <Arduino.h>

#include "canary/io/rs485.h"
#include "canary/io/modbus_rtu.h"
#include "canary/log.h"

namespace canary {
namespace io {

namespace mb = canary::io::modbus;

namespace {
bool s_ready = false;
uint32_t s_next_probe_ms = 0;

// Bench heartbeat cadence + probe target. Deliberately gentle — this is a
// bring-up sanity poll, not a data path.
constexpr uint32_t PROBE_EVERY_MS = 5000;
constexpr uint8_t PROBE_SLAVE = 1;
constexpr uint16_t PROBE_REG = 0;
constexpr uint32_t PROBE_TIMEOUT_MS = 500;

// Drain any stale bytes left on the line before a fresh request, so a late
// reply from a previous poll can't be mistaken for this one's.
void flush_input() {
  while (Serial1.available()) Serial1.read();
}
}  // namespace

bool rs485_begin(uint32_t baud) {
  if (s_ready) return true;
  // Auto-direction transceiver: no DE/RE pin. 8N1 is the Modbus RTU default.
  Serial1.begin(baud ? baud : RS485_BAUD_DEFAULT, SERIAL_8N1, RS485_PIN_RX,
                RS485_PIN_TX);
  s_ready = true;
  char msg[64];
  snprintf(msg, sizeof(msg), "up on A/B (TX=%d RX=%d) @ %lu 8N1", RS485_PIN_TX,
           RS485_PIN_RX, (unsigned long)(baud ? baud : RS485_BAUD_DEFAULT));
  log_line("RS485", msg);
  return true;
}

bool rs485_ready() { return s_ready; }

int rs485_read_holding(uint8_t slave, uint16_t start, uint16_t count,
                       uint16_t* regs, size_t cap, uint32_t timeout_ms) {
  if (!s_ready) return RS485_ERR_NOT_READY;
  if (count == 0 || count > cap) return RS485_ERR_ARG;

  uint8_t req[mb::MAX_ADU];
  const size_t req_len =
      mb::build_read_holding(slave, start, count, req, sizeof(req));
  if (req_len == 0) return RS485_ERR_ARG;

  flush_input();
  Serial1.write(req, req_len);
  Serial1.flush();  // block until the request has fully shifted out

  const size_t want = mb::read_response_len(count);
  uint8_t resp[mb::MAX_ADU];
  size_t got = 0;
  const uint32_t deadline = millis() + timeout_ms;
  while (got < want && (int32_t)(deadline - millis()) > 0) {
    while (Serial1.available() && got < sizeof(resp)) {
      resp[got++] = (uint8_t)Serial1.read();
    }
  }
  if (got < want) return RS485_ERR_TIMEOUT;

  return mb::parse_registers(resp, got, slave, mb::FN_READ_HOLDING, regs, cap);
}

void rs485_loop(uint32_t now) {
  if (!s_ready) return;
  if ((int32_t)(now - s_next_probe_ms) < 0) return;
  s_next_probe_ms = now + PROBE_EVERY_MS;

  uint16_t reg = 0;
  const int r =
      rs485_read_holding(PROBE_SLAVE, PROBE_REG, 1, &reg, 1, PROBE_TIMEOUT_MS);
  char msg[80];
  if (r == 1) {
    snprintf(msg, sizeof(msg), "probe slave %u reg %u = %u (0x%04X)", PROBE_SLAVE,
             PROBE_REG, reg, reg);
  } else if (r == RS485_ERR_TIMEOUT) {
    snprintf(msg, sizeof(msg), "probe slave %u: no reply (nothing on the bus?)",
             PROBE_SLAVE);
  } else {
    snprintf(msg, sizeof(msg), "probe slave %u: status %d", PROBE_SLAVE, r);
  }
  log_line("RS485", msg);
}

}  // namespace io
}  // namespace canary

#endif  // FEATURE_RS485
