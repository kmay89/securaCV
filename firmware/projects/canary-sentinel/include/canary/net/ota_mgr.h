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

// Call once in setup(), after MQTT is connected.
void ota_init(const Topics& topics);

// Call every loop() pass.
void ota_loop(uint32_t now_ms);

} // namespace canary::net
