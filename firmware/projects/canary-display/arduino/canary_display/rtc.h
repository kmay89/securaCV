// PCF8563 real-time-clock runtime — trusted time for the witness dash.
//
// Thin device layer over the pure core (canary/io/rtc_pcf.h): it owns the I2C
// exchange on the shared Wire bus (the same GPIO8/9 bus the panel expander and
// touch ride), the core owns the BCD + calendar math. Runtime-probing by
// design — it does not trust the board map (both 4.3B pin headers declare
// HAS_RTC 0 pending a bench check), it just asks 0x51 if it is there and uses it
// only if it ACKs and holds a reliable time. Otherwise the dash falls back to
// SNTP exactly as before.
//
// Why it matters: SNTP-only time is a weakness for a cryptographic witness —
// block or spoof NTP and every signed timestamp is suspect. A populated RTC lets
// the dash carry trustworthy time across a router outage and seed the clock at
// boot before the network is up.
//
// Gated by FEATURE_RTC (default 0). Compiled only in the canary-display-dash-rtc
// env; the default and emulator builds see an empty translation unit, so the
// wasm stays byte-for-byte identical. No stubs are needed — every caller in
// main.cpp is under the same gate.

#pragma once

#include <cstdint>

namespace canary {
namespace io {

// Probe the PCF8563 on the shared I2C bus once (call after the display HAL has
// brought Wire up). If it answers and holds a reliable time AND the system clock
// isn't set yet, seed the clock from it (settimeofday) so the dash has trustable
// time before — or entirely without — SNTP. Returns true if a working RTC
// answered. No-op returning false unless FEATURE_RTC.
bool rtc_begin();

// True if the last probe found a responding RTC.
bool rtc_present();

// Periodic upkeep: once the system clock is valid (SNTP has synced), mirror it
// back to the RTC so the RTC tracks NTP and survives the next outage. Cheap and
// self-throttling; safe to call every loop pass.
void rtc_loop(uint32_t now);

}  // namespace io
}  // namespace canary
