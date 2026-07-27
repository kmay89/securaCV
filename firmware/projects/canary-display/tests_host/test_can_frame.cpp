// Host test for the pure CAN 2.0 frame core (include/canary/io/can_frame.h).
//
// Builds standalone with g++ — no Arduino, no board. Run in CI by the
// "CAN frame host test" step in .github/workflows/firmware.yml. Prints
// "ALL CAN FRAME TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_can_frame.cpp -o t && ./t

#include "canary/io/can_frame.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace can = canary::io::can;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                     \
    }                                                              \
  } while (0)

// ── Identifier validity (ISO 11898-1 addressing) ────────────────────────────
static void test_valid_id() {
  CHECK(can::valid_id(0x000, false), "std id 0x000 valid");
  CHECK(can::valid_id(0x7FF, false), "std id 0x7FF (max 11-bit) valid");
  CHECK(!can::valid_id(0x800, false), "std id 0x800 rejected (needs extended)");
  CHECK(can::valid_id(0x1FFFFFFF, true), "ext id 0x1FFFFFFF (max 29-bit) valid");
  CHECK(!can::valid_id(0x20000000, true), "ext id 0x20000000 rejected (>29-bit)");
  // A 29-bit-range id is not valid as a standard frame.
  CHECK(!can::valid_id(0x1234, false), "0x1234 rejected as standard id");
  CHECK(can::valid_id(0x1234, true), "0x1234 valid as extended id");
}

static void test_valid_dlc_and_frame() {
  CHECK(can::valid_dlc(0), "dlc 0 valid");
  CHECK(can::valid_dlc(8), "dlc 8 valid");
  CHECK(!can::valid_dlc(9), "dlc 9 rejected (classic CAN caps at 8)");

  can::Frame f{};
  f.id = 0x123;
  f.extended = false;
  f.dlc = 8;
  CHECK(can::valid_frame(f), "well-formed standard frame valid");
  f.dlc = 9;
  CHECK(!can::valid_frame(f), "frame with dlc 9 invalid");
  f.dlc = 2;
  f.id = 0x800;  // out of 11-bit range for a standard frame
  CHECK(!can::valid_frame(f), "standard frame with 0x800 id invalid");
  f.extended = true;
  CHECK(can::valid_frame(f), "same id valid once marked extended");
}

static void test_valid_bitrate() {
  CHECK(can::valid_bitrate(125000), "125k valid");
  CHECK(can::valid_bitrate(250000), "250k valid");
  CHECK(can::valid_bitrate(500000), "500k valid");
  CHECK(can::valid_bitrate(1000000), "1M valid");
  CHECK(!can::valid_bitrate(800000), "800k rejected (no TWAI preset)");
  CHECK(!can::valid_bitrate(0), "0 rejected");
}

// ── SocketCAN-style acceptance filtering ────────────────────────────────────
static void test_filter() {
  // mask 0 accepts everything.
  CHECK(can::filter_accepts(0x123, 0x456, 0x000), "mask 0 accepts any id");
  // full mask matches exactly one id.
  CHECK(can::filter_accepts(0x123, 0x123, 0x7FF), "full mask exact match");
  CHECK(!can::filter_accepts(0x124, 0x123, 0x7FF), "full mask rejects a near id");
  // partial mask: high nibble must match, low bits don't care.
  CHECK(can::filter_accepts(0x1AB, 0x100, 0x700), "0x1AB accepted (top bits match)");
  CHECK(!can::filter_accepts(0x2AB, 0x100, 0x700), "0x2AB rejected (top bits differ)");
}

// ── ASCII log formatter ─────────────────────────────────────────────────────
static void test_format_data_frame() {
  can::Frame f{};
  f.id = 0x0AB;
  f.extended = false;
  f.rtr = false;
  f.dlc = 3;
  f.data[0] = 0xDE;
  f.data[1] = 0xAD;
  f.data[2] = 0xBE;
  char buf[64];
  size_t n = can::format_frame(f, buf, sizeof(buf));
  CHECK(n == std::strlen(buf), "format returns the written length");
  CHECK(std::strcmp(buf, "id=0x0AB ext=0 rtr=0 dlc=3 data=DE AD BE") == 0,
        "standard data frame formatted exactly");
}

static void test_format_extended_and_remote() {
  can::Frame f{};
  f.id = 0x18FF50E5;  // a J1939-style 29-bit id
  f.extended = true;
  f.rtr = true;
  f.dlc = 8;  // remote frames carry a DLC but no data
  char buf[64];
  can::format_frame(f, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "id=0x18FF50E5 ext=1 rtr=1 dlc=8 remote") == 0,
        "extended remote frame formatted exactly");

  // Zero-length data frame prints an empty payload cleanly.
  can::Frame z{};
  z.id = 0x001;
  z.dlc = 0;
  can::format_frame(z, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "id=0x001 ext=0 rtr=0 dlc=0 data=") == 0,
        "zero-length data frame formatted");
}

static void test_format_raw_id() {
  // An out-of-range id must be logged RAW (not masked to a plausible value), so
  // a bug that produces e.g. 0xFFF in a "standard" frame is visible in the log.
  can::Frame f{};
  f.id = 0xFFF;  // > 11-bit max; a real bug if seen on a standard frame
  f.extended = false;
  f.dlc = 0;
  char buf[64];
  can::format_frame(f, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "id=0xFFF ext=0 rtr=0 dlc=0 data=") == 0,
        "out-of-range std id shown raw, not masked");
}

static void test_format_guards() {
  can::Frame f{};
  f.id = 0x123;
  f.dlc = 8;
  CHECK(can::format_frame(f, nullptr, 10) == 0, "null buffer returns 0");
  char tiny[4];
  CHECK(can::format_frame(f, tiny, sizeof(tiny)) == 0,
        "undersized buffer returns 0, no overflow");
}

int main() {
  test_valid_id();
  test_valid_dlc_and_frame();
  test_valid_bitrate();
  test_filter();
  test_format_data_frame();
  test_format_extended_and_remote();
  test_format_raw_id();
  test_format_guards();

  if (g_fail == 0) {
    std::printf("ALL CAN FRAME TESTS PASSED\n");
    return 0;
  }
  std::printf("%d CAN FRAME TEST(S) FAILED\n", g_fail);
  return 1;
}
