// Demo mode runtime (FEATURE_DEMO_MODE, docs/hardware/display_modes.md §demo).
//
// The story the real code tells: the scripted cast + beats from the pure,
// host-tested demo_script.h are fed through the REAL FleetModel into the
// REAL faces — nothing is mocked downstream of the script, so the demo is
// the product code rendering a storyline, never a video. Network-silent by
// policy (mode_policy(Demo).network == false): main.cpp never initializes
// WiFi/MQTT/OTA on this path, and the glass wears a persistent DEMO chip so
// a demo unit can never impersonate a live fleet.
//
// Gestures: tap = the face's normal navigation (page on the watch, proof
// sheets on the dash), long-press = acknowledge (the ack UX is part of the
// story), hold 3 s = exit back to the fleet face.

#ifndef CANARY_MODE_DEMO_MODE_H
#define CANARY_MODE_DEMO_MODE_H

namespace canary {
namespace mode {

void demo_mode_setup();
void demo_mode_loop();

}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_DEMO_MODE_H
