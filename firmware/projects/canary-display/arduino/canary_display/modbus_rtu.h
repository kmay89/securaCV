// Modbus RTU — pure, host-testable protocol core (no Arduino, no allocation).
//
// This is the honesty layer for the 4.3B's RS485 A/B terminal: every byte that
// goes on the wire is built and checked here, in plain C++17, so the framing +
// CRC can be exhaustively host-tested (tests_host/test_modbus_rtu.cpp) without a
// board. The thin transport that actually drives Serial1 lives in src/io/rs485.*
// behind FEATURE_RS485 — it owns timing and the UART; this file owns the format.
//
// Scope: the Modbus master (client) side of the three function codes a bring-up
// bench needs — read holding registers (0x03), read input registers (0x04), and
// write single register (0x06). Register values are big-endian on the wire (the
// Modbus convention); the CRC-16/MODBUS trailer is little-endian (low byte
// first), also per spec. Nothing here blocks, allocates, or logs.
//
// CRC-16/MODBUS: poly 0xA001 (reflected 0x8005), init 0xFFFF, no final xor. Its
// cataloged check value — CRC of the ASCII "123456789" — is 0x4B37, which the
// host test pins as an independent known-answer anchor.

#ifndef CANARY_IO_MODBUS_RTU_H
#define CANARY_IO_MODBUS_RTU_H

#include <cstddef>
#include <cstdint>

namespace canary {
namespace io {
namespace modbus {

// Function codes (master requests).
enum : uint8_t {
  FN_READ_HOLDING = 0x03,
  FN_READ_INPUT = 0x04,
  FN_WRITE_SINGLE = 0x06,
};

// The exception bit ORed into the function code of an error response.
constexpr uint8_t EXCEPTION_FLAG = 0x80;

// parse_registers() / decode_response() outcomes. Negative = failure; a
// non-negative return from parse_registers() is the register count decoded.
enum Status : int {
  ERR_SHORT = -1,       // fewer bytes than the frame needs
  ERR_CRC = -2,         // CRC-16 mismatch
  ERR_ADDR = -3,        // responder slave address != expected
  ERR_FUNC = -4,        // function code != expected (and not an exception)
  ERR_EXCEPTION = -5,   // a Modbus exception response (see decode_exception)
  ERR_BYTECOUNT = -6,   // declared byte count inconsistent with the frame/buffer
  ERR_CAPACITY = -7,    // caller's register buffer is too small
};

// Longest ADU this core builds/parses: 1 addr + 1 func + 1 bytecount + 253 data
// + 2 CRC. A Modbus RTU frame is capped at 256 bytes by the spec.
constexpr size_t MAX_ADU = 256;

// The Modbus spec caps a single read (0x03/0x04) at 125 registers (0x7D): the
// response is 5 + 2*count bytes, so 125 -> 255 bytes, the most that fits in one
// RTU frame. Bounding count here is what stops an over-long read from asking for
// a response the transport can never fully receive (a silent timeout).
constexpr uint16_t MAX_READ_REGS = 125;

// ── CRC-16/MODBUS ──────────────────────────────────────────────────────────
// Bitwise (table-free) so the header stays tiny and dependency-free; a bench
// poll is nowhere near hot enough to want a 512-byte table.
inline uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  if (data == nullptr) return crc;  // empty/absent buffer -> init value
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x0001) {
        crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

// Append the CRC-16 trailer (low byte first) after `len` payload bytes already
// in `buf`. Returns the new total length (len + 2). Caller guarantees capacity.
inline size_t append_crc(uint8_t* buf, size_t len) {
  const uint16_t crc = crc16(buf, len);
  buf[len] = static_cast<uint8_t>(crc & 0xFF);         // low byte first
  buf[len + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return len + 2;
}

// True if the last two bytes of `frame` are a valid CRC over the preceding
// bytes. `len` is the whole frame including the 2 CRC bytes.
inline bool crc_ok(const uint8_t* frame, size_t len) {
  if (len < 3) return false;
  const uint16_t want = crc16(frame, len - 2);
  const uint16_t got =
      static_cast<uint16_t>(frame[len - 2] | (frame[len - 1] << 8));
  return want == got;
}

// ── Request builders ───────────────────────────────────────────────────────
// Each writes a complete ADU (header + CRC) into `out` (needs >= 8 bytes) and
// returns its length, or 0 if `cap` is too small.

// Read `count` holding (0x03) or input (0x04) registers from `start`. Returns 0
// if `out` is null/undersized or `count` is outside the spec's 1..125 range —
// the latter guard is what keeps a response inside one RTU frame (MAX_ADU).
inline size_t build_read(uint8_t fn, uint8_t slave, uint16_t start,
                         uint16_t count, uint8_t* out, size_t cap) {
  if (out == nullptr || cap < 8) return 0;
  if (count == 0 || count > MAX_READ_REGS) return 0;
  out[0] = slave;
  out[1] = fn;
  out[2] = static_cast<uint8_t>(start >> 8);
  out[3] = static_cast<uint8_t>(start & 0xFF);
  out[4] = static_cast<uint8_t>(count >> 8);
  out[5] = static_cast<uint8_t>(count & 0xFF);
  return append_crc(out, 6);
}

inline size_t build_read_holding(uint8_t slave, uint16_t start, uint16_t count,
                                 uint8_t* out, size_t cap) {
  return build_read(FN_READ_HOLDING, slave, start, count, out, cap);
}

inline size_t build_read_input(uint8_t slave, uint16_t start, uint16_t count,
                               uint8_t* out, size_t cap) {
  return build_read(FN_READ_INPUT, slave, start, count, out, cap);
}

// Write a single holding register (0x06).
inline size_t build_write_single(uint8_t slave, uint16_t reg, uint16_t value,
                                  uint8_t* out, size_t cap) {
  if (out == nullptr || cap < 8) return 0;
  out[0] = slave;
  out[1] = FN_WRITE_SINGLE;
  out[2] = static_cast<uint8_t>(reg >> 8);
  out[3] = static_cast<uint8_t>(reg & 0xFF);
  out[4] = static_cast<uint8_t>(value >> 8);
  out[5] = static_cast<uint8_t>(value & 0xFF);
  return append_crc(out, 6);
}

// The exact number of response bytes a read of `count` registers will return,
// so the transport knows how long to wait: addr+func+bytecount + 2*count + CRC.
inline size_t read_response_len(uint16_t count) {
  return 5 + static_cast<size_t>(count) * 2;
}

// ── Response parsing ───────────────────────────────────────────────────────

// If `frame` is a Modbus exception response for `slave`, return its exception
// code (1..255); otherwise return 0. Assumes crc already validated by caller.
inline uint8_t decode_exception(const uint8_t* frame, size_t len) {
  if (len < 5) return 0;
  if ((frame[1] & EXCEPTION_FLAG) == 0) return 0;
  return frame[2];
}

// Parse a read-registers response (fn 0x03/0x04). On success returns the number
// of registers written into `regs` (== the responder's bytecount / 2); on
// failure returns a negative Status. Validates length, CRC, slave, function,
// the exception bit, the declared byte count, and the caller's capacity.
inline int parse_registers(const uint8_t* frame, size_t len, uint8_t expect_slave,
                           uint8_t expect_fn, uint16_t* regs, size_t regs_cap) {
  if (len < 5) return ERR_SHORT;
  if (!crc_ok(frame, len)) return ERR_CRC;
  if (frame[0] != expect_slave) return ERR_ADDR;
  if (frame[1] == (expect_fn | EXCEPTION_FLAG)) return ERR_EXCEPTION;
  if (frame[1] != expect_fn) return ERR_FUNC;

  const uint8_t byte_count = frame[2];
  if (byte_count & 0x01) return ERR_BYTECOUNT;          // registers are 2 bytes
  // header(3) + data(byte_count) + crc(2) must equal the frame length exactly.
  if (static_cast<size_t>(byte_count) + 5 != len) return ERR_BYTECOUNT;

  const size_t n = byte_count / 2;
  if (n > regs_cap) return ERR_CAPACITY;
  if (n > 0 && regs == nullptr) return ERR_CAPACITY;  // caller buffer contract
  for (size_t i = 0; i < n; i++) {
    regs[i] = static_cast<uint16_t>((frame[3 + i * 2] << 8) | frame[3 + i * 2 + 1]);
  }
  return static_cast<int>(n);
}

}  // namespace modbus
}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_MODBUS_RTU_H
