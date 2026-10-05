#pragma once
// The Vision Doorbell's button and glow ring — the hardware half. The
// decisions (debounce, holdoff, stuck, the glow's curves and its slew
// limit) are firmware/common/doorbell/doorbell_logic.h, host-tested; this
// is pins, an ISR, LEDC and NVS around it.
//
// Compiled in on the boards whose pins.h names DOORBELL_BUTTON_PIN and
// DOORBELL_GLOW_PIN (the XIAO hosts the doorbell case takes); elsewhere
// every call is a no-op and available() is false. Off until the owner
// switches it on from Home Assistant — or until the first real press: a
// plain Vision never sees one (its D1 just idles on the pull-up), a
// doorbell's first visitor turns it on. Wiring:
// docs/hardware/canary_vision_doorbell_wiring.md.

#include <stdint.h>

#include "doorbell/doorbell_logic.h"

namespace canary::doorbell_hw {

bool available();  // this build has the pins

void init();       // pins, ISR, LEDC, the glow timer; reads NVS

// Drain the ISR's edges and poll the pin; calls on_event once per event,
// in order. Call every loop pass, broker or not.
void poll(uint32_t now_ms, void (*on_event)(::doorbell::ButtonEvent ev, uint32_t now_ms));

// The witness can (true) or cannot (false) reach its hub: AWAKE vs UNSURE.
void set_hub_ok(bool ok);

// A ring was sealed: swell the glow.
void swell(uint32_t now_ms);

bool enabled();
bool set_enabled(bool on);       // persists; true if it changed
uint8_t glow_pct();
bool set_glow_pct(uint8_t pct);  // clamps to 10..100, persists; true if changed

bool stuck();                    // the button is jammed down
bool ring_fault();               // the glow timer failed: the ring holds a steady glow
uint32_t repeats();              // presses inside the holdoff since boot
uint32_t rings();                // sealed rings since boot

}  // namespace canary::doorbell_hw
