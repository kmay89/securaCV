#pragma once
// The Vision Doorbell's speaker — the hardware half. The voice itself
// (the chime, the tick, the three reply tones, the volume law) is
// firmware/common/doorbell/doorbell_audio.h, host-tested and integer-only;
// this is one PWM pin, a sample timer and NVS around it.
//
// Compiled in on the boards whose pins.h names DOORBELL_AUDIO_PIN (the XIAO
// hosts the doorbell case takes, on D3); elsewhere every call is a no-op
// and available() is false. The pin feeds an RC-filtered class-D amplifier
// (PAM8302A class) and a sealed driver behind the face's grille:
// docs/hardware/canary_vision_doorbell_wiring.md, "The speaker".
//
// Nothing here is heard by the device: there is no microphone path, and the
// replies are tones (docs/hardware/canary_doorbell_research.md §3, §4.3).

#include <stdint.h>

#include "doorbell/doorbell_audio.h"

namespace canary::doorbell_audio {

bool available();        // this build has the pin

void init();             // the PWM channel, the sample timer; reads NVS

// Play a phrase now (replacing one in progress). Safe from the main loop.
void play(::doorbell::Phrase p);

bool playing();

uint8_t volume();                 // 0..100
bool set_volume(uint8_t pct);     // clamps, persists; true if it changed

// The reply the household last sent (for the state row); NONE after boot.
::doorbell::Phrase last_reply();

bool fault();            // the sample timer could not start: the speaker is silent

}  // namespace canary::doorbell_audio
