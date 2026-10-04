#pragma once
#include <stdint.h>

#include "network/wifi_join_policy.h"  // JoinFailure, shared fleet-wide

namespace canary::net {

// Boot-time connect: blocking, with a hard timeout.
//
// It does NOT reboot on failure, and that is deliberate. Rebooting re-runs this
// identical join against the identical network with the identical credentials,
// so a wrong password, a renamed SSID, or a 5 GHz-only AP this radio cannot see
// turned into a silent ~30-second reboot cycle with nothing on the glass — the
// device never finished booting, so the setup wizard that could have fixed the
// password never appeared. Boot now always completes: the screen, the touch
// surface, and the fleet this device can still reach over ESP-NOW do not depend
// on the uplink. Retry is wifi_loop's job.
//
// Applies the WiFi power policy (modem sleep / TX power cap from
// canary/config.h) once the link is up.
void wifi_init_or_reboot();

// Steady-state STA supervision — call every loop() pass. Non-blocking:
// detects link loss and retries with exponential backoff plus jitter
// (2 s → 4 s → 8 s → 16 s → 30 s cap, reset after a sustained reconnect).
//
// Reboots as a last resort after WIFI_OUTAGE_REBOOT_MS of continuous outage —
// but ONLY for a link that was associated at least once since power-on, where a
// reboot plausibly clears a wedged radio or a stale DHCP lease. The rules live
// in common/network/wifi_join_policy.h and are host-tested.
void wifi_loop(uint32_t now_ms);

// True while the STA link is up.
bool wifi_connected();

// Current RSSI in dBm (0 when not connected) — surfaced as an HA
// diagnostic sensor via the status heartbeat.
int wifi_rssi();

// Why the link isn't up, as of the last attempt. Feeds the status line on the
// glass, so a device that can't join says WHICH thing went wrong rather than
// showing a spinner that never resolves.
JoinFailure wifi_last_failure();

// True once the device has failed to associate enough times, with a failure a
// human could actually fix (wrong password, or an SSID this radio can't see),
// AND has never been online since power-on.
//
// This is the case the onboarding wizard used to miss entirely: it only ran
// when credentials were *placeholders*. Credentials that were SET BUT WRONG
// sailed past it into a join that could never succeed — which is what a
// keyboard-less operator experiences as a device that just doesn't work. The
// "never been online" half matters just as much in the other direction: a
// Canary that has run for months and loses its AP for ten minutes must never
// throw away a good configuration and start advertising a setup network.
bool wifi_wants_setup();

} // namespace canary::net
