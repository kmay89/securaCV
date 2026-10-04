#pragma once
#include <stdint.h>

#include "canary/hal/voice_score.h"

// Canary Voice — the display's sound engine (trailblazer spec §5).
//
// The score tables and all the audio math live in voice_score.h (pure,
// host-tested). THIS header is the runtime face of the LEDC streamer in
// src/hal/chime.cpp: it drives one passive piezo on one LEDC channel, fully
// non-blocking — voice_play() latches a phrase, voice_loop() renders it a
// control-tick at a time (envelope shaping, glissando, warble) from the main
// loop. Nothing sounds until chime_init() runs, which main.cpp calls solely
// under FEATURE_CHIME (piezo pad populated); the whole engine is always
// compiled for CI coverage.
//
// The grammar is the product. A dead battery must never sound like an
// intruder (distinct pitch + tempo), resolution gets a falling tone so
// silence keeps meaning "nothing new," and the one Tier-1 alert keeps an
// audible floor at every volume — the sound that is allowed to break the
// night can never be dialed to nothing. Beyond the alert tiers, the palette
// is a *family* of acoustic signatures (Boot, Sunrise, Ack, page turns,
// mute…) sharing one timbre and one warm scale — recognizably ours.

namespace canary::hal {

// ── Back-compat alert facade ─────────────────────────────────────────────
// The original four-tier enum; a thin alias onto Voice so existing callers
// (care_glue, wake_glue, main.cpp's legacy grammar) compile untouched.
enum class Chime : uint8_t {
  Tier1Alarm,  // Alert/Tamper  -> Voice::Alarm
  Tier2Warn,   // Warn          -> Voice::Warn
  AllClear,    // resolution    -> Voice::AllClear
  Sunrise,     // wake chirp    -> Voice::Sunrise
};

// Set up the LEDC channel and load the persisted volume/interaction prefs.
// pin < 0 disables all sound. Uses its own LEDC channel (not the backlight's).
void chime_init(int pin);

// Latch a pattern (replaces any in progress). `ramp` scales an alarm episode
// 0 (soft) / 1 (mid) / 2 (full): the attention policy wakes gently and
// escalates on each re-voice — both "siren scared the house" and "too quiet
// to wake me" answered by one ladder. Non-alarm voices pass ramp = 2.
void chime_play(Chime c, uint8_t ramp = 2);

// ── The full palette ─────────────────────────────────────────────────────
// Play any acoustic signature. Loudness is resolved at play time from the
// signature's category, the user's volume, night state, and ramp
// (voice_score.h::voice_peak_duty). An Interaction-category voice is
// suppressed when interactions are off or at night; a peak of 0 simply makes
// no sound — the caller need not pre-check policy.
void voice_play(Voice v, uint8_t ramp = 2);

// Advance the current phrase; call every loop pass. No-op when idle/uninit.
void voice_loop(uint32_t now_ms);
inline void chime_loop(uint32_t now_ms) { voice_loop(now_ms); }

// True while a phrase is latched and still rendering. Lets a caller pump the
// engine to completion (e.g. render the boot chime synchronously at power-on,
// before the steady-state loop that normally drives voice_loop() begins).
bool voice_active();

// Silence immediately and drop any latched phrase. Use it to bound a
// synchronous pump: if the phrase hasn't finished by a deadline, stop() ensures
// the tone can't keep sounding through subsequent blocking work.
void voice_stop();

// Runtime state the loudness model reads. Night gates the gentle categories
// (see voice_peak_duty); the render/care loop keeps it in step with quiet
// hours. Volume is 0..4 (Off, Soft, Low, Medium, Full); interactions are the
// optional UI feedback tones. Both persist to NVS ("scv-voice").
void    voice_set_night(bool night);
void    voice_set_volume(uint8_t vol_0_to_4);
uint8_t voice_volume();
void    voice_set_interactions(bool on);
bool    voice_interactions();

}  // namespace canary::hal
