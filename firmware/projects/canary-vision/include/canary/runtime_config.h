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
// First provisioning is a user-compiled USB flash (real secrets), which
// seeds NVS; every OTA release build afterwards inherits the unit's setup.

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

// True only when NVS/compiled values are real, not the generic release's
// placeholders. Used to boot into an honest offline/provisioning state instead
// of spending every boot in a Wi-Fi timeout/reboot loop.
bool wifi_credentials_configured();
bool mqtt_credentials_configured();

// Persist credentials the setup portal tested successfully (and patch the
// cached config, so a join in this same boot uses them). Empty pass = open
// network. No-op on an empty ssid.
void set_wifi_credentials(const char* ssid, const char* pass);

} // namespace canary::cfg
