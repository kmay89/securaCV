#pragma once
#include <stdint.h>

// Runtime device configuration — NVS-backed with compiled-in fallback.
//
// Why this exists: firmware releases published for OTA are GENERIC binaries
// (built with placeholder secrets). A unit's real identity and credentials
// must therefore live in NVS, not in the compiled image, or the first
// over-the-air update would wipe them. Policy per field:
//
//   - device_id: NVS always wins (identity is sticky across reflashes and
//     OTA updates); the compiled DEVICE_ID only seeds the very first boot.
//   - WiFi / MQTT credentials: real compiled values win and are persisted
//     (flashing a new secrets build over USB updates the unit); placeholder
//     values (CI stubs / generic release builds) defer to NVS.
//
// First provisioning is either the on-glass onboarding wizard (SoftAP captive
// portal writes NVS — see net/provision.h) or a user-compiled USB flash with
// real secrets; every OTA release build afterwards inherits the unit's setup.

namespace canary::cfg {

struct RuntimeConfig {
  char device_id[48];
  char wifi_ssid[33];
  char wifi_pass[65];
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[33];
  char mqtt_pass[65];
};

// Loaded once on first call (then cached). Safe to call from setup() onward.
const RuntimeConfig& get();

// True when the effective WiFi SSID is a placeholder (CI stub / generic OTA
// build with nothing in NVS) — i.e. this unit has never been provisioned.
// The onboarding wizard keys off this.
bool wifi_is_placeholder();

// Persist WiFi credentials to NVS and update the cached config in place —
// called by the onboarding wizard ON SUCCESS ONLY (a failed join must never
// leave broken credentials behind). No reboot needed; the net stack reads
// the refreshed cache from here on. The pair also enters the saved-network
// ring below as its last good network.
void set_wifi_credentials(const char* ssid, const char* pass);

// ── The saved-network ring (common/network/wifi_store.h) ────────────────
// Up to four saved networks under one NVS blob (`wifi_nets`), the fleet's
// one store, so a glass carried between a house and a workshop, or a home
// with a guest network beside the main one, joins whichever is there. The
// legacy pair above stays in NVS as the CURRENT pick — every reader of
// wifi_ssid keeps working, and the flashers keep seeding it — and folds into
// the ring on first load. The ring is loaded with get(); on a unit whose NVS
// is unavailable it is empty and the legacy pair stands alone.
int wifi_saved_networks();            // ring count (0 = none)
// The slot to try next (wifi_store::pick with no scan in hand): last good
// when `previous` is -1 (a fresh boot, a fresh outage), else the slot after
// `previous`, round-robin — so a NotFound moves on instead of probing an
// absent name forever. -1 when the ring is empty.
int wifi_saved_pick(int previous);
const char* wifi_saved_ssid(int idx);  // "" out of range
const char* wifi_saved_pass(int idx);
// A join verdict for slot `idx`. A success that moves last_good persists the
// ring and points the legacy pair at the network on the air; a failure only
// counts (RAM), so a flaky router does not wear the flash.
void wifi_saved_note_result(int idx, bool ok);

// Erase the stored WiFi credentials (settings › reset › wifi › forget, and
// the BOOT button's 10 s factory-reset hold) so the join wizard reopens on
// the next boot — the on-glass path for a puck that moves house or a router
// that changed its password. The whole saved-network ring goes with the
// pair, and so does the setup AP's stored password (the next wizard mints a
// fresh one — the fleet's reset scope, common/io/reset_hold.h). Identity,
// broker pins, and screen settings all stay. Returns false when NVS is
// unavailable (nothing was erased; the caller should not reboot into a
// wizard that won't come). Note the compiled-credentials rule still applies
// after the erase: a unit built with real secrets re-persists them next boot
// — for dev units, USB owns the configuration, by design.
bool forget_wifi_credentials();

} // namespace canary::cfg
