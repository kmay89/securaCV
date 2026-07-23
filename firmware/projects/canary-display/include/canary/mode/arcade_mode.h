// Arcade mode runtime (FEATURE_ARCADE, docs/hardware/display_modes.md
// §arcade): the QA suite wearing a costume. One round of "Canary Catch"
// visits EVERY touch zone of the panel exactly once (the seeded shuffle,
// target placement, and pass/fail verdict live in the pure, host-tested
// arcade_logic.h — this TU draws and forwards taps, it decides nothing).
// The score screen IS the factory report: zones hit, misses, worst/avg
// latency, the replay seed, PASS/FAIL. Network-silent; dash-first (the
// watch's 240 px round glass makes a poor arcade). Hold 3 s to exit.

#ifndef CANARY_MODE_ARCADE_MODE_H
#define CANARY_MODE_ARCADE_MODE_H

namespace canary {
namespace mode {

void arcade_mode_setup();
void arcade_mode_loop();

}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_ARCADE_MODE_H
