#pragma once
#include "topics.h"

// Signed pull-OTA glue — wires the shared engine (firmware/common/ota) to
// this variant: boot self-test with rollback, daily jittered update check,
// the HA update entity's retained state, and the Install / auto-update
// commands HA publishes. Same flow as canary and canary-wap; see
// docs/firmware_ota.md.

namespace canary::net {

// Call once in setup(), right AFTER WiFi is up and BEFORE the blocking
// MQTT connect. Confirms — or rolls back — a freshly installed image; the
// required probe asserts WiFi connectivity. May not return: a failed
// required probe reboots into the previous firmware. The engine owns
// rollback confirmation, so skipping this call would revert every fresh
// install on its second boot.
void ota_boot_validate();

// Call once in setup(), after the one bounded MQTT connect attempt —
// whether or not it reached the broker. Logs a rollback found at boot.
void ota_init(const Topics& topics);

// Call every loop() pass that has the broker: the daily check, HA's Install
// / auto-update / channel commands, and the update entity's publishes. A
// glass with no broker runs none of these; it installs only when asked
// (Settings → firmware, POST /api/ota/install).
void ota_loop(uint32_t now_ms);

// The health-gated half of the A/B confirmation (docs/OTA.md §2): call on
// EVERY loop() pass, broker or no broker, with healthy = the Wi-Fi station
// link; a pending image is confirmed once that has held for the configured
// dwell. A glass with no broker can still install, so behind the broker
// gate its new image would never confirm and the next restart would return
// to the previous firmware. Logs "Firmware update applied." on the pass
// that confirms.
void ota_health_tick(uint32_t now_ms, bool healthy);

// ── On-glass settings facade ─────────────────────────────────────────────
// A thin read/act surface for the Settings "firmware" page, so the UI layer
// never includes the raw OTA engine header. Mirrors what the HA update
// entity already exposes — the wall glass and the app can't disagree.

struct OtaStatus {
  const char* installed;    // running firmware version (CANARY_FW_VERSION)
  const char* latest;       // newest known version (== installed if none)
  bool update_available;    // a newer, verified image is offered
  bool busy;                // a check or install is in flight
  uint8_t progress;         // 0..100 during install (0 otherwise)
  bool auto_update;         // nightly auto-install is on
  bool dev_channel;         // true = dev (prerelease), false = release
  const char* state_text;   // friendly one-liner ("up to date", "installing…")
};

// Snapshot the current OTA state for rendering. Cheap; call each rebuild.
OtaStatus ota_status();

// Ask the engine to check the manifest now (jumps the daily timer). Safe on
// the main loop, same call ota_loop makes; result lands in ota_status().
void ota_request_check();

// Check and, if a newer verified image exists, install it now. May reboot
// on success (does not return); rolls back on a failed post-flash probe.
void ota_request_install();

// Persist the nightly auto-install preference (NVS-backed, flash-wear aware).
void ota_set_auto_update(bool on);

} // namespace canary::net
