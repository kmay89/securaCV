// First-boot WiFi onboarding — SoftAP + captive portal + on-glass guide.
//
// A fresh display (placeholder WiFi credentials) must never reboot-loop into
// a dead end. Instead it raises a device-unique setup network, shows a join
// QR on its own glass, and walks the user through a phone-side portal:
//
//   glass: Hello → Scan me (QR) → check your phone → Joining… → You're in.
//   phone: camera scan auto-joins AP → captive sheet pops → pick network →
//          password → live status → done.
//
// No dead ends, by construction:
//   wrong password  → specific reason on phone AND glass; portal stays open
//   network missing → "network not found", same recovery
//   user walks away → the join scene breathes indefinitely (display dims
//                     after a while; a joining phone or a touch re-wakes)
//   power cycle     → credentials only persist on SUCCESS, so an interrupted
//                     setup simply starts over, clean
//
// Captive mechanics follow the canary-wap wizard's hard-won lessons
// (LESSONS_LEARNED §captive-portal): A-only DNS with NODATA for AAAA/HTTPS,
// per-OS probe policy, pre-AP scan, scan-handle deletion before join, early
// failure reasons, and an AP linger after success so the phone's final
// status poll wins the channel-change race.
//
// Gated by FEATURE_ONBOARDING. Pure helpers live in provision_core.h
// (host-tested); this header is the firmware-only surface.
#pragma once

namespace canary::net {

// True when the stored WiFi credentials are placeholders — i.e. this device
// has never been provisioned (or was factory-reset).
bool provision_needed();

// Run the whole onboarding flow. Blocking modal phase, called from setup()
// BEFORE the watchdog is armed and before wifi_init_or_reboot(); returns
// with the STA associated and credentials persisted to NVS. glass_ok=false
// (panel init failed) degrades to a serial-guided portal: the AP, captive
// sheet, and join flow all still work — the SSID/password print on serial.
void provision_run(bool glass_ok);

// True while the portal is actively testing a candidate network (the join's
// WiFi.begin() association is in flight). The shared radio belongs to that
// join for the duration — nothing may retune its channel. Always false when
// the portal isn't running. Defined only in FEATURE_ONBOARDING builds, like
// the rest of this surface — gate the call site.
bool provision_join_in_flight();

}  // namespace canary::net
