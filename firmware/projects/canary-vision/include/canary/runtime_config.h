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
//   - The broker can also arrive at runtime, over the Bluetooth setup door's
//     hub hand-off (common/network/hub_handoff.h): the phone that handed the
//     unit its Wi-Fi hands it the hub it found and the login it minted, and
//     set_mqtt_credentials() below writes them to the same NVS keys the
//     flashers seed. On a generic build that is then what every later boot
//     reads; on a build compiled with a real MQTT_HOST the compiled value
//     wins again at the next boot, by the rule above.
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
  char mqtt_user[65];   // the hub hand-off's bound (64 bytes) plus the NUL, like mqtt_pass
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
// network. No-op on an empty ssid. Returns whether the store VERIFIABLY
// holds them (readback, not the write calls) — false means this boot works
// from the patched cache but the next boot re-opens setup.
bool set_wifi_credentials(const char* ssid, const char* pass);

// Persist a broker handed over at runtime (the Bluetooth setup door's hub
// hand-off) and patch the cached config, so the MQTT manager's next bind in
// this same boot dials it. `user` / `pass` null or empty = no login (the
// manager then connects anonymously). `tls` false writes the broker socket
// mode back to plain (a CA-verified mode from an earlier flash would try TLS
// against the plaintext listener the phone named); `tls` true leaves the
// stored mode as it is — the hand-off carries no certificate, so a unit with
// a provisioned CA or pin keeps verifying and a unit without one stays
// plain. No-op (false) on an empty host or a zero port. Returns whether the
// store VERIFIABLY holds it (readback, as set_wifi_credentials) — false
// means this boot works from the patched cache and the next boot reads
// whatever NVS held before.
bool set_mqtt_credentials(const char* host, uint16_t port,
                          const char* user, const char* pass, bool tls);

} // namespace canary::cfg
