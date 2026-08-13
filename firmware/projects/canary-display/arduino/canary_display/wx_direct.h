// include/canary/net/wx_direct.h — the standalone-weather fetcher
// (FEATURE_STANDALONE_WEATHER; disclosed in docs/security/SECURITY_MODEL.md).
//
// A hub-less home may OPT IN to the display fetching its own forecast — an
// anonymous HTTPS query to a public commons service (the same shape as
// SNTP), over the coarse 0.1° location the phone stored. Three gates, all
// of them load-bearing:
//
//   1. `wx_direct` (the stored opt-in) is ON — off by default, forever;
//   2. a coarse location has been stored (`wx_loc_set`);
//   3. NO HUB WAS EVER CONFIGURED (mqtt_broker_is_placeholder). A home
//      with a hub keeps the hub as its single egress point — this fetcher
//      must never become a quiet fallback when that hub is down, or the
//      privacy story turns conditional on uptime.
//
// The result feeds bedside_on_weather as the exact retained-blob JSON a hub
// would have published, so every face and line behaves identically.
#pragma once
#include <stdint.h>

namespace canary::net {

// Call from the main loop. Cheap when idle; fetches on its own cadence
// (~45 min, 5 min backoff after a failure) and only while all three gates
// above hold.
void wx_direct_loop(uint32_t now_ms);

// Status for the settings sheet / API: 0 = waiting on the opt-in,
// 1 = waiting on a location, 2 = a hub owns weather (fetcher stands down),
// 3 = active, 4 = active but the last fetch failed.
uint8_t wx_direct_status();

// Minutes since the last successful fetch; 0xFFFF = never this boot.
uint16_t wx_direct_age_min(uint32_t now_ms);

}  // namespace canary::net
