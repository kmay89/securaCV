// Mode glue — the thin runtime shell around the pure mode registry.
//
// Spec: docs/hardware/display_modes.md. The DECISIONS (which gear a boot
// lands in, what each gear may do, the legacy-devmode migration) live in the
// pure, host-tested mode_registry.h; this layer only touches the silicon:
// it reads/writes the NVS latch ("securacv"/"mode" + the legacy "devmode"
// bool) and dispatches setup/loop to whichever gears this build carries.
//
// Compiled ONLY when at least one non-fleet gear is compiled in (the TU is
// empty otherwise) — the default watch/dash/emulator builds stay
// byte-identical, exactly like the playground before it.

#ifndef CANARY_MODE_MODE_GLUE_H
#define CANARY_MODE_MODE_GLUE_H

#include "canary/mode/mode_registry.h"

namespace canary {
namespace mode {

// Resolve the gear for THIS boot: dedicated bench env wins, else the NVS
// token (fail-safe to Fleet), else the legacy devmode bool -> Bench.
Mode boot_mode();

// Hand the device to a non-fleet gear (called once from setup(), then
// setup() returns without touching the network stack unless the gear's
// policy says otherwise). Persists the token in the new grammar so a
// legacy-latched unit is migrated on first entry.
void mode_enter(Mode m);

// One loop() pass for the active gear.
void mode_loop_step(Mode m);

// Settings doorway: latch `m` for the next boot and reboot into it.
// Bench also writes the legacy devmode bool so a firmware DOWNGRADE still
// lands in the bench the user asked for. Does not return.
void mode_request(Mode m);

// Uniform exit (every gear's 3 s long-press): clear both latches back to
// the product face and reboot. Does not return.
void mode_exit_to_fleet();

}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_MODE_GLUE_H
