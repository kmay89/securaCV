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

}  // namespace canary::net
