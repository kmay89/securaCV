#pragma once
#include <stdint.h>

#include "canary/fleet/fleet_model.h"  // BeaconStatus + Via (pure)

// First Light pair demo — the modal surface (docs/first_light_demo.md).
//
// A Canary Vision and this glass, powered on a desk with nothing else: the
// card shows what the camera's NPU sees the moment it sees it, with the
// trigger timing in numbers this device honestly measured itself. The
// decisions live in the pure core (canary/pair/pair_demo.h, host-tested);
// this surface is LVGL rendering, NVS persistence of the one lock, and the
// radio-parking glue.
//
// Modal contract (the commission_ui rules): a live urgent alert closes it,
// the wake window stays pinned while it is open, auto-orient and ambient
// life park, and apply_brightness gives it a bright glass.
//
// Gated by FEATURE_PAIR_DEMO (default 0; the nightlight env turns it on).
// When 0 everything here is a no-op returning false, so call sites need no
// gates of their own.

namespace canary::ui {

#if defined(FEATURE_PAIR_DEMO) && FEATURE_PAIR_DEMO

// Open / close the surface. Opening loads the remembered lock from NVS;
// closing leaves it remembered (forget is an explicit gesture, not an exit).
void pair_demo_ui_open(uint32_t now);
void pair_demo_ui_close();
bool pair_demo_ui_active();

// Drive countdowns, staleness, the edge pulse, and the urgent-close rule.
// Call every loop pass; cheap when closed.
void pair_demo_ui_tick(uint32_t now);

// One received fleet-link beacon, from the ESP-NOW / LAN receive drains.
// While the surface is open this feeds the card; while it is closed it only
// evaluates the boxed-pair auto-open rule (unprovisioned glass + a beacon on
// the router-free band) and parks the request for the main loop.
void pair_demo_note_beacon(const char* fp4,
                           const canary::fleet::BeaconStatus& s,
                           bool have_status, uint32_t now,
                           canary::fleet::Via via);

// Main-loop drain of the auto-open request (the mailbox pattern — the
// receive drains must not build LVGL surfaces from wherever they run).
bool pair_demo_take_auto_open();

// Button routing while the surface is open (the modal owns the grammar):
// Short = keep the camera on stage, Double = forget it, Long = leave (the
// caller closes). The open gesture itself (BOOT held 5 s) is the caller's
// HoldGate — see canary/pair/pair_demo.h.
void pair_demo_ui_button_short(uint32_t now);
void pair_demo_ui_button_double(uint32_t now);

#else  // FEATURE_PAIR_DEMO off — inert stubs so call sites stay gate-free

inline void pair_demo_ui_open(uint32_t) {}
inline void pair_demo_ui_close() {}
inline bool pair_demo_ui_active() { return false; }
inline void pair_demo_ui_tick(uint32_t) {}
inline void pair_demo_note_beacon(const char*,
                                  const canary::fleet::BeaconStatus&, bool,
                                  uint32_t, canary::fleet::Via) {}
inline bool pair_demo_take_auto_open() { return false; }
inline void pair_demo_ui_button_short(uint32_t) {}
inline void pair_demo_ui_button_double(uint32_t) {}

#endif  // FEATURE_PAIR_DEMO

}  // namespace canary::ui
