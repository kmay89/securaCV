// PCF8563 real-time-clock core — pure, host-testable (no Arduino, no Wire).
//
// Trusted time for a cryptographic witness: SNTP-only time is a weakness (block
// or spoof NTP and every signed timestamp is suspect). If a PCF8563/PCF85063 is
// populated on the shared I2C bus, this core turns its register block into an
// epoch and back, so the runtime (src/io/rtc.cpp, gated on FEATURE_RTC) can seed
// the clock from the RTC when the network can't, and write NTP back to it when
// it can. Everything decision-worthy — BCD, the civil<->days calendar math, the
// voltage-low validity flag, range checks — lives here where a host test pins it
// without a board (tests_host/test_rtc_pcf.cpp).
//
// Register block is the 7 bytes starting at PCF8563 reg 0x02 (VL_seconds):
//   [0] 0x02 seconds  bit7 = VL (1 => oscillator stopped, time UNRELIABLE)
//   [1] 0x03 minutes
//   [2] 0x04 hours    (24h)
//   [3] 0x05 days
//   [4] 0x06 weekdays (0..6; derived, not trusted on read)
//   [5] 0x07 months   bit7 = century (0 => 20xx, this core's supported range)
//   [6] 0x08 years    (00..99, base 2000)
// PCF8563 and PCF85063A both answer at I2C 0x51 and share this seconds-first
// BCD layout, so this core drives either.

#ifndef CANARY_IO_RTC_PCF_H
#define CANARY_IO_RTC_PCF_H

#include <cstdint>

namespace canary {
namespace io {
namespace rtc {

constexpr uint8_t PCF_ADDR      = 0x51;  // PCF8563 / PCF85063A
constexpr uint8_t PCF_REG_SECS  = 0x02;  // first register of the 7-byte block
constexpr uint8_t PCF_VL_BIT    = 0x80;  // seconds reg bit7: voltage-low flag

// A wall-clock instant, UTC. `year` is the full year (e.g. 2026); `mon` is
// 1..12; `wday` is 0..6 (Sun=0), derived on encode and ignored on decode.
struct RtcTime {
  uint16_t year = 2000;
  uint8_t  mon = 1, day = 1, hour = 0, min = 0, sec = 0, wday = 0;
};

// ── BCD (the RTC speaks packed binary-coded decimal) ─────────────────────
inline uint8_t bcd2bin(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0f)); }
inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// ── Proleptic-Gregorian calendar math (Howard Hinnant's algorithms) ──────
// Days since the Unix epoch (1970-01-01) for a y/m/d, and its inverse. Pure
// integer, valid for the whole range we care about — no libc timegm needed,
// so the conversion is identical on-device and in the host test.
inline int64_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

inline void civil_from_days(int64_t z, int32_t* y, uint32_t* m, uint32_t* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t yy = (int32_t)yoe + (int32_t)(era * 400);
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = yy + (*m <= 2);
}

// Day of week for a civil date, Sun=0 (matches the PCF8563 weekday reg).
inline uint8_t weekday_from_civil(int32_t y, uint32_t m, uint32_t d) {
  const int64_t z = days_from_civil(y, m, d);
  return (uint8_t)((z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6));
}

// ── Whole-instant conversion, UTC ────────────────────────────────────────
inline int64_t to_epoch(const RtcTime& t) {
  return days_from_civil((int32_t)t.year, t.mon, t.day) * 86400LL +
         (int64_t)t.hour * 3600 + (int64_t)t.min * 60 + t.sec;
}

inline void from_epoch(int64_t epoch, RtcTime& t) {
  int64_t days = epoch / 86400;
  int64_t rem = epoch % 86400;
  if (rem < 0) { rem += 86400; days -= 1; }
  int32_t y; uint32_t m, d;
  civil_from_days(days, &y, &m, &d);
  t.year = (uint16_t)y; t.mon = (uint8_t)m; t.day = (uint8_t)d;
  t.hour = (uint8_t)(rem / 3600);
  t.min = (uint8_t)((rem % 3600) / 60);
  t.sec = (uint8_t)(rem % 60);
  t.wday = weekday_from_civil(y, m, d);
}

// ── Sanity: a plausible 21st-century instant this core supports ──────────
inline bool valid(const RtcTime& t) {
  return t.year >= 2000 && t.year <= 2099 && t.mon >= 1 && t.mon <= 12 &&
         t.day >= 1 && t.day <= 31 && t.hour <= 23 && t.min <= 59 &&
         t.sec <= 59;
}

// ── Register block <-> RtcTime ───────────────────────────────────────────
// Encode always clears VL (we are writing a known-good time) and stamps the
// weekday + century=0 (20xx). `out` is the 7 bytes for regs 0x02..0x08.
inline void encode_regs(const RtcTime& t, uint8_t out[7]) {
  out[0] = bin2bcd(t.sec) & 0x7f;             // VL cleared
  out[1] = bin2bcd(t.min) & 0x7f;
  out[2] = bin2bcd(t.hour) & 0x3f;
  out[3] = bin2bcd(t.day) & 0x3f;
  out[4] = weekday_from_civil((int32_t)t.year, t.mon, t.day) & 0x07;
  out[5] = bin2bcd(t.mon) & 0x1f;             // century bit 0 => 20xx
  out[6] = bin2bcd((uint8_t)(t.year - 2000));
}

// Decode the 7-byte block. Returns false if the VL flag is set (oscillator
// stopped since the last write -> time is not trustworthy) or the decoded
// instant is out of range. On false, *t is left indeterminate: callers must
// fall back to SNTP, never seed the clock from an unreliable RTC.
inline bool decode_regs(const uint8_t in[7], RtcTime& t) {
  if (in[0] & PCF_VL_BIT) return false;       // voltage-low: unreliable
  t.sec = bcd2bin(in[0] & 0x7f);
  t.min = bcd2bin(in[1] & 0x7f);
  t.hour = bcd2bin(in[2] & 0x3f);
  t.day = bcd2bin(in[3] & 0x3f);
  t.wday = (uint8_t)(in[4] & 0x07);
  t.mon = bcd2bin(in[5] & 0x1f);
  t.year = (uint16_t)(2000 + bcd2bin(in[6]));
  return valid(t);
}

}  // namespace rtc
}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_RTC_PCF_H
