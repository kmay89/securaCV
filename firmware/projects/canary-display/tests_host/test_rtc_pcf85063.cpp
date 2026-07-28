// Host test for the PCF85063A register-base selection (RTC_CHIP_PCF85063).
//
// The PCF85063A's 7-byte time block starts at register 0x04, NOT the PCF8563
// base 0x02 (its 0x02/0x03 are the offset and RAM-byte registers). Reading the
// wrong base would feed shifted garbage into a signed timestamp. This TU
// compiles the pure core WITH the chip declared and verifies the base flips to
// 0x04 while the BCD codec still round-trips and the (PCF8563-only) century bit
// is masked off. Prints "ALL RTC PCF85063 TESTS PASSED" on success.
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include \
//     firmware/projects/canary-display/tests_host/test_rtc_pcf85063.cpp -o t && ./t

#define RTC_CHIP_PCF85063 1
#include "canary/io/rtc_pcf.h"

#include <cstdint>
#include <cstdio>

namespace rtc = canary::io::rtc;

// The whole point of this file: with the chip declared, the block base is 0x04.
static_assert(rtc::PCF_REG_SECS == 0x04,
              "PCF85063A seconds block must start at register 0x04");

int main() {
  int fail = 0;

  // The BCD codec is chip-agnostic — a full instant must still round-trip.
  rtc::RtcTime in;
  in.year = 2026; in.mon = 7; in.day = 21; in.hour = 14; in.min = 30; in.sec = 15;
  uint8_t regs[7];
  rtc::encode_regs(in, regs);
  rtc::RtcTime out;
  if (!rtc::decode_regs(regs, out) || out.year != 2026 || out.mon != 7 ||
      out.day != 21 || out.hour != 14 || out.min != 30 || out.sec != 15) {
    std::printf("  FAIL: PCF85063 register round-trip\n");
    fail++;
  }

  // On the PCF85063A the months register has no century bit (reserved/0). Even
  // if bit7 is set on the wire, decode must mask it and still read month 7.
  regs[5] |= 0x80;
  rtc::RtcTime out2;
  if (!rtc::decode_regs(regs, out2) || out2.mon != 7) {
    std::printf("  FAIL: months century/reserved bit not masked\n");
    fail++;
  }

  if (fail == 0) {
    std::printf("ALL RTC PCF85063 TESTS PASSED\n");
    return 0;
  }
  std::printf("%d RTC PCF85063 TEST(S) FAILED\n", fail);
  return 1;
}
