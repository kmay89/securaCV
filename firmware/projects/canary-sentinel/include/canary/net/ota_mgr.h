#pragma once
#include "canary/topics.h"

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
// and auto-update commands, and the update entity's publishes.
void ota_loop(uint32_t now_ms);

// The health-gated half of the A/B confirmation (docs/OTA.md §2): call on
// EVERY loop() pass, broker or no broker, with healthy = the Wi-Fi station
// link; a pending image is confirmed once that has held for the configured
// dwell. Behind the broker gate, a new image that booted to a dark broker
// waited on the broker to confirm, and a restart before the broker came
// back returned it to the previous firmware. Logs "Firmware update
// applied." on the pass that confirms.
void ota_health_tick(uint32_t now_ms, bool healthy);

} // namespace canary::net
