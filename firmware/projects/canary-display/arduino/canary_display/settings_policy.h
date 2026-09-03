// include/canary/net/settings_policy.h — which settings the LAN API may write.
//
// The display's web API (POST /api/set in net/glass_web.cpp) is a LAN
// surface. Its Origin + per-boot CSRF guard stops a drive-by web page, but a
// host already on the home WiFi can read the token from GET /api/settings
// and post with it — the documented trust boundary for the whole mirror.
// That is fine for a brightness slider. It is not fine for the two settings
// that decide whether this device opens an outbound connection it would not
// otherwise open, and where it says it is:
//
//   wx_direct — the standalone-weather opt-in: the display's one OPT-IN
//               outbound path (SNTP and the daily signed update check are
//               always-on and carry no location — see
//               docs/security/SECURITY_MODEL.md);
//   wx_loc    — the coarse location that fetch would carry.
//
// "Zero phone-home" must not be a principle a neighbor can flip. So these
// keys are refused by the network API outright — every caller, every token,
// 403 {"ok":false,"err":"on_glass_only"} — and are settable only from the
// on-glass settings menu (settings > weather > fetch itself), which takes a
// hand on the glass. The network handler has no branch that stores them.
//
// Everything else /api/set and /api/tz accept stays an ordinary knob behind
// Origin + CSRF: brightness, night behavior, the look, the lamp, the zone.
// `tz` is deliberately ordinary — a POSIX clock rule, not a stored place: it
// never leaves the device (the forecast query asks the service to infer the
// zone from the grid point; SNTP carries none), and the web route is the
// only way a household on a generic OTA image ever fixes a wrong-coast
// clock (see handle_tz_set).
//
// Pure C++ (no Arduino, no board) so a host test can pin the list
// (tests_host/test_settings_policy.cpp) and glass_web.cpp can both enforce
// it and serve it (GET /api/settings lists these keys under `on_glass`, so a
// client never draws a control that would 403).
#pragma once
#include <stddef.h>
#include <string.h>

namespace canary::net {

// The on-glass-only keys, in ONE place. A new setting that stores a
// location or opens a network path goes here first — then the host test,
// the mirror page's read-only rows, and the docs.
constexpr const char* const kOnGlassOnlyKeys[] = {"wx_direct", "wx_loc"};
constexpr size_t kOnGlassOnlyKeyCount =
    sizeof(kOnGlassOnlyKeys) / sizeof(kOnGlassOnlyKeys[0]);

// The table's own constant for a key the LAN write API must refuse, or
// nullptr. Exact, case-sensitive match: the API's keys are fixed lowercase
// identifiers, and a looser match would be a second parser to keep in step
// with the handler's. Returning the table entry (never the caller's bytes)
// gives the refusal log a string that was never request text.
inline const char* settings_key_on_glass_only_name(const char* key) {
  if (!key) return nullptr;
  for (size_t i = 0; i < kOnGlassOnlyKeyCount; i++) {
    if (strcmp(kOnGlassOnlyKeys[i], key) == 0) return kOnGlassOnlyKeys[i];
  }
  return nullptr;
}

// True when the LAN write API must refuse this key.
inline bool settings_key_is_on_glass_only(const char* key) {
  return settings_key_on_glass_only_name(key) != nullptr;
}

}  // namespace canary::net
