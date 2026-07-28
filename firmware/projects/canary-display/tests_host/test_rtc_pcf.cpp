// Host test for the pure PCF8563 RTC core (include/canary/io/rtc_pcf.h).
//
// Builds standalone with g++ — no Arduino, no board, no Wire. Run in CI by the
// "PCF8563 RTC host test" step in .github/workflows/firmware.yml. Prints
// "ALL RTC TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_rtc_pcf.cpp -o t && ./t

#include "canary/io/rtc_pcf.h"

#include <cstdio>
#include <ctime>

namespace rtc = canary::io::rtc;

// The two parts share the block order but NOT the base register; the default
// build (no RTC_CHIP_PCF85063) must select the PCF8563 base. (The PCF85063A
// selection is exercised by test_rtc_pcf85063.cpp.)
static_assert(rtc::PCF8563_REG_SECS == 0x02, "PCF8563 seconds base is 0x02");
static_assert(rtc::PCF85063_REG_SECS == 0x04, "PCF85063A seconds base is 0x04");
static_assert(rtc::PCF_REG_SECS == rtc::PCF8563_REG_SECS,
              "default RTC base must be the PCF8563 (0x02)");

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                     \
    }                                                              \
  } while (0)

// A catalogued Unix instant: 1700000000 == 2023-11-14 22:13:20 UTC (Tue).
static void test_known_epoch_decomposes() {
  rtc::RtcTime t;
  rtc::from_epoch(1700000000LL, t);
  CHECK(t.year == 2023, "known epoch: year");
  CHECK(t.mon == 11, "known epoch: month");
  CHECK(t.day == 14, "known epoch: day");
  CHECK(t.hour == 22, "known epoch: hour");
  CHECK(t.min == 13, "known epoch: minute");
  CHECK(t.sec == 20, "known epoch: second");
  CHECK(t.wday == 2, "known epoch: weekday = Tuesday");
  CHECK(rtc::to_epoch(t) == 1700000000LL, "known epoch: round-trips");
}

// to_epoch/from_epoch must invert across a long sweep — cross-checked against
// libc timegm so any calendar-math slip shows up.
static void test_epoch_roundtrip_sweep() {
  bool ok = true;
  for (int64_t e = 946684800LL /*2000-01-01*/; e < 4102444800LL /*2100*/;
       e += 97001 /*coprime-ish stride, hits leap days/DST-free UTC*/) {
    rtc::RtcTime t;
    rtc::from_epoch(e, t);
    if (rtc::to_epoch(t) != e) { ok = false; break; }
    std::tm tm{};
    tm.tm_year = t.year - 1900; tm.tm_mon = t.mon - 1; tm.tm_mday = t.day;
    tm.tm_hour = t.hour; tm.tm_min = t.min; tm.tm_sec = t.sec;
    if (timegm(&tm) != e) { ok = false; break; }
  }
  CHECK(ok, "epoch<->civil inverts and matches timegm across 2000..2100");
}

// Encode to the 7 BCD registers and decode back: identity, VL cleared.
static void test_register_roundtrip() {
  rtc::RtcTime in;
  in.year = 2026; in.mon = 7; in.day = 21;
  in.hour = 14; in.min = 30; in.sec = 15;
  uint8_t regs[7];
  rtc::encode_regs(in, regs);
  // Spot-check BCD packing (0x02 seconds reg, VL clear).
  CHECK((regs[0] & rtc::PCF_VL_BIT) == 0, "encode clears VL");
  CHECK(regs[0] == 0x15, "seconds 15 -> BCD 0x15");
  CHECK(regs[2] == 0x14, "hours 14 -> BCD 0x14");
  CHECK(regs[5] == 0x07, "month 7 -> BCD 0x07 (century bit 0)");
  CHECK(regs[6] == 0x26, "year 2026 -> BCD 0x26");
  rtc::RtcTime out;
  CHECK(rtc::decode_regs(regs, out), "decode a good block succeeds");
  CHECK(out.year == 2026 && out.mon == 7 && out.day == 21 && out.hour == 14 &&
            out.min == 30 && out.sec == 15,
        "register round-trip preserves the instant");
  CHECK(out.wday == 2, "2026-07-21 is a Tuesday");
}

// The voltage-low flag means the oscillator stopped: decode must REFUSE so the
// runtime never seeds the clock from an unreliable RTC.
static void test_voltage_low_rejected() {
  rtc::RtcTime in;
  in.year = 2026; in.mon = 7; in.day = 21; in.hour = 1; in.min = 2; in.sec = 3;
  uint8_t regs[7];
  rtc::encode_regs(in, regs);
  regs[0] |= rtc::PCF_VL_BIT;  // simulate a battery-dead / just-powered RTC
  rtc::RtcTime out;
  CHECK(!rtc::decode_regs(regs, out), "VL set -> decode refuses (unreliable)");
}

// Out-of-range BCD (garbage on the bus) must not pass as a valid time.
static void test_garbage_rejected() {
  // hours reg 0x24 survives the &0x3f mask as BCD 24 -> hour 24, out of range.
  uint8_t regs[7] = {0x00, 0x00, 0x24 /*hour 24*/, 0x01, 0x00, 0x07, 0x26};
  rtc::RtcTime out;
  CHECK(!rtc::decode_regs(regs, out), "impossible hour -> rejected");
  CHECK(!rtc::valid(rtc::RtcTime{1999, 1, 1, 0, 0, 0, 0}), "pre-2000 -> invalid");
}

int main() {
  test_known_epoch_decomposes();
  test_epoch_roundtrip_sweep();
  test_register_roundtrip();
  test_voltage_low_rejected();
  test_garbage_rejected();

  if (g_fail == 0) {
    std::printf("ALL RTC TESTS PASSED\n");
    return 0;
  }
  std::printf("%d RTC TEST(S) FAILED\n", g_fail);
  return 1;
}
