// include/canary/net/tz_auto.h — learn the house's timezone from the web.
//
// The clock stack: SNTP (pool.ntp.org + time.nist.gov, cross-checking
// sources) delivers correct UTC — but a wall display must speak WALL time,
// daylight saving included, and asking a household to type a POSIX TZ
// string is not calm technology. So once WiFi is up, this module asks a
// geolocation service which IANA zone the display's IP sits in, maps it to
// a full POSIX rule (DST transitions included — a bare UTC offset would go
// wrong twice a year), applies it, and remembers it in NVS so every later
// boot is right before the network even comes up.
//
// A secrets.h CD_TZ override always wins (never auto-learn over an
// explicit choice), and an unknown zone falls back to the reported fixed
// offset — right today, refreshed if the service becomes reachable later.
#pragma once
#include <stdint.h>

namespace canary::net {

// Returns the POSIX TZ to use at boot: NVS-learned if present, else the
// compile-time seed (CD_TZ). Caller passes the seed; buffer is filled.
void tz_boot_string(const char* seed, char* out, unsigned cap);

// True when a learned TZ (not the seed) is active.
bool tz_learned();

// Call from loop(). Once WiFi is connected and no explicit CD_TZ override
// exists, fetches the zone (bounded, one attempt per backoff window),
// applies it via configTzTime re-arm, and persists it.
void tz_auto_tick(uint32_t now_ms);

// Set the zone BY HAND at runtime (the display's web page). Same storage
// the learner writes, so it survives reboots and OTA the same way — which
// is the point: released binaries are generic and carry the CD_TZ default,
// so without this a household could only fix its clock by rebuilding the
// firmware. Takes a POSIX TZ string ("EST5EDT,M3.2.0,M11.1.0"); applies it
// immediately, persists it, and stands the learner down so nothing
// overrides a choice a human made. Returns false if the string is empty or
// too long to store, leaving the current zone untouched.
bool tz_set_manual(const char* posix);

// The POSIX TZ in force right now (stored value if there is one, else the
// compile-time seed). Never returns a partial string; out is always
// NUL-terminated.
void tz_current(char* out, unsigned cap);

}  // namespace canary::net
