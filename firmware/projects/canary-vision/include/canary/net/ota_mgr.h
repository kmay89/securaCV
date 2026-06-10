#pragma once
#include "canary/topics.h"

// Signed pull-OTA glue — wires the shared engine (firmware/common/ota) to
// this variant: boot self-test with rollback, daily jittered update check,
// the HA update entity's retained state, and the Install / auto-update
// commands HA publishes. Same flow as canary and canary-wap; see
// docs/firmware_ota.md.

namespace canary::net {

// Call once in setup(), AFTER WiFi + MQTT are up (the boot self-test's
// required probe asserts connectivity; reaching it on a fresh image means
// the new firmware can still do its job). May not return: a failed
// required probe rolls back into the previous firmware.
void ota_init(const Topics& topics);

// Call every loop() pass.
void ota_loop(uint32_t now_ms);

} // namespace canary::net
