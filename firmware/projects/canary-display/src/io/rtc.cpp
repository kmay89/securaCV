// PCF8563 RTC runtime — see canary/io/rtc.h.
//
// The whole translation unit is behind FEATURE_RTC so the default and emulator
// builds compile it to nothing (byte-neutral wasm). No stubs are needed: every
// caller in main.cpp is under the same gate. The register/calendar logic lives
// in the host-tested pure core (canary/io/rtc_pcf.h); this file is only the I2C
// transport and the seed/mirror policy.

#include <config.h>

#if defined(FEATURE_RTC) && FEATURE_RTC

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>

#include "canary/io/rtc.h"
#include "canary/io/rtc_pcf.h"
#include "canary/log.h"

namespace canary {
namespace io {

namespace {
namespace r = canary::io::rtc;

bool s_present = false;
uint32_t s_next_check_ms = 0;

// Once the system clock reads at/after this instant we treat it as "real" (SNTP
// stepped it): ~2023-11-14. Below it, the clock is the boot default and there is
// nothing worth mirroring to the RTC.
constexpr time_t TIME_VALID_EPOCH = 1700000000;
constexpr uint32_t MIRROR_EVERY_MS = 3600000;  // 1 h once the clock is valid
constexpr uint32_t RECHECK_MS = 5000;          // poll while waiting for SNTP

// Read the 7-byte time block (regs 0x02..0x08). Returns false on a bus fault or
// if the core rejects the block (VL set / out of range).
bool read_rtc(r::RtcTime& t) {
  Wire.beginTransmission(r::PCF_ADDR);
  Wire.write(r::PCF_REG_SECS);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)r::PCF_ADDR, 7) != 7) return false;
  uint8_t regs[7];
  for (int i = 0; i < 7; i++) regs[i] = (uint8_t)Wire.read();
  return r::decode_regs(regs, t);
}

bool write_rtc(const r::RtcTime& t) {
  uint8_t regs[7];
  r::encode_regs(t, regs);
  Wire.beginTransmission(r::PCF_ADDR);
  Wire.write(r::PCF_REG_SECS);
  Wire.write(regs, 7);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool rtc_present() { return s_present; }

bool rtc_begin() {
  // Presence = a bare-address ACK on the shared bus.
  Wire.beginTransmission(r::PCF_ADDR);
  s_present = (Wire.endTransmission() == 0);
  if (!s_present) {
    log_line("RTC", "no PCF8563 on the bus - SNTP is the time source");
    return false;
  }
  // Seed the clock from the RTC only if the system time isn't already real and
  // the RTC's time is reliable (decode_regs refuses a VL/garbage block).
  r::RtcTime t;
  if (time(nullptr) < TIME_VALID_EPOCH) {
    if (read_rtc(t)) {
      struct timeval tv = {(time_t)r::to_epoch(t), 0};
      settimeofday(&tv, nullptr);
      log_line("RTC", "seeded the clock from the RTC (offline-trusted time)");
    } else {
      log_line("RTC", "present but time unreliable (VL) - waiting for SNTP");
    }
  }
  s_next_check_ms = 0;  // mirror back as soon as the clock is valid
  return true;
}

void rtc_loop(uint32_t now) {
  if (!s_present) return;
  if ((int32_t)(now - s_next_check_ms) < 0) return;
  const time_t sys = time(nullptr);
  if (sys < TIME_VALID_EPOCH) {
    s_next_check_ms = now + RECHECK_MS;  // keep watching for the SNTP step
    return;
  }
  s_next_check_ms = now + MIRROR_EVERY_MS;
  r::RtcTime t;
  r::from_epoch((int64_t)sys, t);
  if (write_rtc(t)) log_line("RTC", "mirrored NTP time to the RTC");
}

}  // namespace io
}  // namespace canary

#endif  // FEATURE_RTC
