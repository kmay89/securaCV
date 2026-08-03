// Host test for the pure Modbus RTU core (include/canary/io/modbus_rtu.h).
//
// Builds standalone with g++ — no Arduino, no board. Run in CI by the
// "Modbus RTU host test" job in .github/workflows/firmware.yml. Prints
// "ALL MODBUS RTU TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_modbus_rtu.cpp -o t && ./t

#include "canary/io/modbus_rtu.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mb = canary::io::modbus;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                     \
    }                                                              \
  } while (0)

// ── CRC-16/MODBUS ───────────────────────────────────────────────────────────
static void test_crc_known_answer() {
  // The catalogd CRC-16/MODBUS check value: CRC of ASCII "123456789" = 0x4B37.
  // This is an authoritative, implementation-independent anchor.
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(mb::crc16(check, sizeof(check)) == 0x4B37, "CRC check value 0x4B37");

  // Empty input is the init value.
  CHECK(mb::crc16(nullptr, 0) == 0xFFFF, "CRC of empty == init 0xFFFF");
}

static void test_crc_trailer_order() {
  // append_crc writes the low byte first, then the high byte (Modbus RTU order).
  uint8_t buf[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
  const size_t n = mb::append_crc(buf, 6);
  CHECK(n == 8, "append_crc returns len+2");
  const uint16_t crc = mb::crc16(buf, 6);
  CHECK(buf[6] == (crc & 0xFF), "trailer low byte first");
  CHECK(buf[7] == ((crc >> 8) & 0xFF), "trailer high byte second");
  CHECK(mb::crc_ok(buf, n), "crc_ok accepts a freshly built frame");

  buf[3] ^= 0xFF;  // corrupt a payload byte
  CHECK(!mb::crc_ok(buf, n), "crc_ok rejects a corrupted frame");
}

// ── Request builders ────────────────────────────────────────────────────────
static void test_build_read_holding() {
  uint8_t out[mb::MAX_ADU];
  const size_t n = mb::build_read_holding(0x11, 0x006B, 0x0003, out, sizeof(out));
  CHECK(n == 8, "read-holding ADU is 8 bytes");
  CHECK(out[0] == 0x11, "slave addr");
  CHECK(out[1] == mb::FN_READ_HOLDING, "func 0x03");
  CHECK(out[2] == 0x00 && out[3] == 0x6B, "start reg big-endian 0x006B");
  CHECK(out[4] == 0x00 && out[5] == 0x03, "count big-endian 0x0003");
  CHECK(mb::crc_ok(out, n), "built request carries a valid CRC");
  // The appended CRC must equal a CRC over exactly the 6 header bytes.
  const uint16_t c = mb::crc16(out, 6);
  CHECK(out[6] == (c & 0xFF) && out[7] == (c >> 8), "CRC covers the header only");
}

static void test_build_read_input_and_write() {
  uint8_t out[mb::MAX_ADU];
  size_t n = mb::build_read_input(0x02, 0x0008, 0x0001, out, sizeof(out));
  CHECK(n == 8 && out[1] == mb::FN_READ_INPUT, "read-input func 0x04");

  n = mb::build_write_single(0x0A, 0x0001, 0xABCD, out, sizeof(out));
  CHECK(n == 8, "write-single ADU is 8 bytes");
  CHECK(out[1] == mb::FN_WRITE_SINGLE, "func 0x06");
  CHECK(out[2] == 0x00 && out[3] == 0x01, "reg 0x0001");
  CHECK(out[4] == 0xAB && out[5] == 0xCD, "value big-endian 0xABCD");
  CHECK(mb::crc_ok(out, n), "write-single CRC valid");
}

static void test_build_capacity_guard() {
  uint8_t tiny[4];
  CHECK(mb::build_read_holding(1, 0, 1, tiny, sizeof(tiny)) == 0,
        "builder refuses an undersized buffer");
  uint8_t out[mb::MAX_ADU];
  CHECK(mb::build_read_holding(1, 0, 1, nullptr, sizeof(out)) == 0,
        "builder refuses a null buffer");
}

static void test_build_count_bounds() {
  uint8_t out[mb::MAX_ADU];
  CHECK(mb::build_read_holding(1, 0, 0, out, sizeof(out)) == 0,
        "count 0 rejected");
  CHECK(mb::build_read_holding(1, 0, mb::MAX_READ_REGS, out, sizeof(out)) == 8,
        "count == MAX_READ_REGS (125) accepted");
  CHECK(mb::build_read_holding(1, 0, mb::MAX_READ_REGS + 1, out, sizeof(out)) == 0,
        "count > 125 rejected (would overflow one RTU frame)");
  // The largest allowed read still fits in one ADU.
  CHECK(mb::read_response_len(mb::MAX_READ_REGS) <= mb::MAX_ADU,
        "max read response fits in MAX_ADU");
}

static void test_response_len() {
  CHECK(mb::read_response_len(1) == 7, "1 reg -> 7 response bytes");
  CHECK(mb::read_response_len(3) == 11, "3 regs -> 11 response bytes");
}

// ── Response parsing ────────────────────────────────────────────────────────

// Helper: assemble a valid read-response frame with a fresh CRC.
static size_t make_read_response(uint8_t slave, uint8_t fn, const uint16_t* regs,
                                 uint8_t count, uint8_t* out) {
  out[0] = slave;
  out[1] = fn;
  out[2] = static_cast<uint8_t>(count * 2);
  for (uint8_t i = 0; i < count; i++) {
    out[3 + i * 2] = static_cast<uint8_t>(regs[i] >> 8);
    out[3 + i * 2 + 1] = static_cast<uint8_t>(regs[i] & 0xFF);
  }
  return mb::append_crc(out, 3 + static_cast<size_t>(count) * 2);
}

static void test_parse_valid() {
  const uint16_t src[3] = {0xAE41, 0x5652, 0x4340};
  uint8_t frame[mb::MAX_ADU];
  const size_t n = make_read_response(0x11, mb::FN_READ_HOLDING, src, 3, frame);

  uint16_t regs[8];
  const int got = mb::parse_registers(frame, n, 0x11, mb::FN_READ_HOLDING, regs, 8);
  CHECK(got == 3, "parsed 3 registers");
  CHECK(regs[0] == 0xAE41 && regs[1] == 0x5652 && regs[2] == 0x4340,
        "register values decoded big-endian");
}

static void test_parse_rejects() {
  const uint16_t src[1] = {0x1234};
  uint8_t frame[mb::MAX_ADU];
  const size_t n = make_read_response(0x11, mb::FN_READ_HOLDING, src, 1, frame);
  uint16_t regs[4];

  CHECK(mb::parse_registers(frame, 3, 0x11, mb::FN_READ_HOLDING, regs, 4) ==
            mb::ERR_SHORT,
        "short frame rejected");

  uint8_t bad = frame[n - 1] ^ 0xFF;  // corrupt CRC high byte
  uint8_t corrupt[mb::MAX_ADU];
  std::memcpy(corrupt, frame, n);
  corrupt[n - 1] = bad;
  CHECK(mb::parse_registers(corrupt, n, 0x11, mb::FN_READ_HOLDING, regs, 4) ==
            mb::ERR_CRC,
        "bad CRC rejected");

  CHECK(mb::parse_registers(frame, n, 0x22, mb::FN_READ_HOLDING, regs, 4) ==
            mb::ERR_ADDR,
        "wrong slave rejected");
  CHECK(mb::parse_registers(frame, n, 0x11, mb::FN_READ_INPUT, regs, 4) ==
            mb::ERR_FUNC,
        "wrong function rejected");
  CHECK(mb::parse_registers(frame, n, 0x11, mb::FN_READ_HOLDING, regs, 0) ==
            mb::ERR_CAPACITY,
        "insufficient register capacity rejected");
}

static void test_parse_exception() {
  // Slave 0x11 returns exception 0x02 (illegal data address) for a 0x03 read.
  uint8_t frame[8];
  frame[0] = 0x11;
  frame[1] = mb::FN_READ_HOLDING | mb::EXCEPTION_FLAG;  // 0x83
  frame[2] = 0x02;
  const size_t n = mb::append_crc(frame, 3);

  uint16_t regs[4];
  CHECK(mb::parse_registers(frame, n, 0x11, mb::FN_READ_HOLDING, regs, 4) ==
            mb::ERR_EXCEPTION,
        "exception response classified as ERR_EXCEPTION");
  CHECK(mb::decode_exception(frame, n) == 0x02, "exception code 0x02 decoded");
}

int main() {
  test_crc_known_answer();
  test_crc_trailer_order();
  test_build_read_holding();
  test_build_read_input_and_write();
  test_build_capacity_guard();
  test_build_count_bounds();
  test_response_len();
  test_parse_valid();
  test_parse_rejects();
  test_parse_exception();

  if (g_fail == 0) {
    std::printf("ALL MODBUS RTU TESTS PASSED\n");
    return 0;
  }
  std::printf("%d MODBUS RTU TEST(S) FAILED\n", g_fail);
  return 1;
}
