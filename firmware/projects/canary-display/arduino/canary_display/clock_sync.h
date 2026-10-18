/**
 * @file clock_sync.h
 * @brief When a Canary may believe its clock, and whom it asks — one policy.
 *
 * The display line runs SNTP against the public pool; the PIO canary has its
 * own; the headless trio (sense, vision, sentinel) never set their wall clock
 * at all, so every record they date is "since boot". The onboarding state
 * machine's Joining step is "STA → address → time → self-test", and this
 * header is the time part, pure:
 *
 *   - the server order: the public pool, a second public server, and the
 *     HUB'S HOST as the third — a LAN whose uplink is down still has a hub
 *     that (when it runs an NTP/chrony service, as Home Assistant OS can)
 *     knows the time. The hub is a fallback, never the first ask: a node
 *     must not depend on the hub for something the internet gives it.
 *   - what "believable" means (an epoch after 2024-01-01 — before that the
 *     clock is the chip's reset value, and a birth day or an event dated
 *     1970 is a false statement, see common/identity/birth_day.h);
 *   - how long a node online without a believable clock waits before it
 *     says so in GET /api/status (`clock: "none"`).
 *
 * Pure. firmware/tests_host/test_clock_sync.cpp.
 */

#pragma once

#include <stdint.h>

namespace canary {
namespace time_sync {

static constexpr const char* NTP_PRIMARY   = "pool.ntp.org";
static constexpr const char* NTP_SECONDARY = "time.nist.gov";
/// 2024-01-01T00:00:00Z. Anything earlier is the reset clock, not the time.
static constexpr int64_t BELIEVABLE_EPOCH = 1704067200;
/// Online this long with no believable clock → report it.
static constexpr uint32_t SYNC_GRACE_MS = 30000;

inline bool believable(int64_t epoch) { return epoch >= BELIEVABLE_EPOCH; }

enum class Source : uint8_t { None = 0, Ntp = 1, Hub = 2, Set = 3, Gps = 4 };

inline const char* source_label(Source s) {
  switch (s) {
    case Source::Ntp: return "ntp";
    case Source::Hub: return "hub";
    case Source::Set: return "set";
    case Source::Gps: return "gps";
    case Source::None: break;
  }
  return "none";
}

/// The hub as SNTP's third server, after NTP_PRIMARY and NTP_SECONDARY — or
/// null when no hub host is known (SNTP takes a null slot as "unused"). The
/// caller hands the two pools by name beside it: a compiled-in destination is
/// named at its call site so the phone-home scan (regression_check.sh) sees
/// exactly what docs/security/SECURITY_MODEL.md discloses.
inline const char* hub_server(const char* hub_host) {
  return (hub_host && hub_host[0]) ? hub_host : nullptr;
}

/// Wrap-safe: has the node been online past the grace with no believable clock?
inline bool sync_overdue(uint32_t now_ms, uint32_t online_since_ms, bool believable_now) {
  if (believable_now) return false;
  return (int32_t)(now_ms - online_since_ms) >= (int32_t)SYNC_GRACE_MS;
}

}  // namespace time_sync
}  // namespace canary
