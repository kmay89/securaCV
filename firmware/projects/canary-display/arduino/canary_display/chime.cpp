// src/hal/chime.cpp — Canary Voice LEDC streamer (trailblazer spec §5).
//
// The pure score + audio math lives in voice_score.h (host-tested); this file
// is the hardware half: a non-blocking renderer that walks a Phrase one note
// at a time and, within each note, updates the LEDC tone every few ms so a
// note can breathe (envelope), chirp (glissando), and trill (warble) on a
// single square-wave piezo voice. Loudness for a given play() is resolved
// once from the volume model and then scaled by the per-tick envelope.
#include "canary/hal/chime.h"
#include "canary/hal/core_compat.h"

#include <Arduino.h>
#include <Preferences.h>

namespace canary::hal {

namespace {

// LEDC channel 1: the watch backlight owns channel 0; the dash uses none.
constexpr uint8_t LEDC_CH = 1;

// Control-tick: how often the streamer re-computes freq/duty inside a note.
// ~6 ms is smooth for warble/glissando and far below the ~40 ms shortest
// note, while keeping LEDC writes cheap.
constexpr uint32_t CTRL_MS = 6;

// Persisted-preference defaults (a flavor config may override). Volume 3 of 4
// (Medium) is a sensible bench default; interaction tones ship enabled but
// are trivially turned off (they are the "optional" half of the grammar).
#ifndef CD_VOICE_VOLUME_DEFAULT
#define CD_VOICE_VOLUME_DEFAULT 3
#endif
#ifndef CD_VOICE_INTERACTIONS_DEFAULT
#define CD_VOICE_INTERACTIONS_DEFAULT 1
#endif

int s_pin = -1;

// Playback state.
const Tone* s_seq = nullptr;
int      s_len = 0;
int      s_idx = -1;             // -1 = latched, not yet started
uint32_t s_tone_start_ms = 0;
uint32_t s_last_ctrl_ms = 0;
uint8_t  s_peak = 0;             // resolved peak duty (0..128) for this phrase

// Loudness policy inputs.
uint8_t s_volume = CD_VOICE_VOLUME_DEFAULT;
bool    s_interactions = CD_VOICE_INTERACTIONS_DEFAULT;
bool    s_night = false;

void tone_freq(uint16_t freq) {
  if (s_pin < 0) return;
  cc_ledc_tone((uint8_t)s_pin, LEDC_CH, freq);
}
void tone_duty(uint8_t duty) {
  if (s_pin < 0) return;
  cc_ledc_write((uint8_t)s_pin, LEDC_CH, duty);
}
void tone_off() {
  if (s_pin < 0) return;
  cc_ledc_tone((uint8_t)s_pin, LEDC_CH, 0);
  cc_ledc_write((uint8_t)s_pin, LEDC_CH, 0);
}

void prefs_load() {
  Preferences p;
  if (!p.begin("scv-voice", /*readOnly=*/true)) return;
  s_volume = p.getUChar("vol", CD_VOICE_VOLUME_DEFAULT);
  // Store the interactions flag as a UChar (0/1), not a Bool: the emulator's
  // Preferences shim implements getUChar/putUChar but not getBool/putBool.
  s_interactions = p.getUChar("ix", CD_VOICE_INTERACTIONS_DEFAULT ? 1 : 0) != 0;
  p.end();
  if (s_volume > 4) s_volume = 4;
}
void prefs_save() {
  Preferences p;
  if (!p.begin("scv-voice", /*readOnly=*/false)) return;
  p.putUChar("vol", s_volume);
  p.putUChar("ix", s_interactions ? 1 : 0);
  p.end();
}

}  // namespace

void chime_init(int pin) {
  if (pin < 0) return;
  s_pin = pin;
  cc_ledc_setup((uint8_t)pin, LEDC_CH, 2000, 8);  // 8-bit duty: 128 == 50%
  tone_off();
  prefs_load();
}

void voice_play(Voice v, uint8_t ramp) {
  if (s_pin < 0) return;

  const VoiceCat cat = voice_category(v);
  // Interaction tones are optional: honor the user's opt-out before we even
  // consult loudness. (Night silences them via the model regardless.)
  if (cat == VoiceCat::Interaction && !s_interactions) return;

  const uint8_t peak = voice_peak_duty(cat, s_volume, s_night, ramp);
  if (peak == 0) return;  // policy says silent — nothing to latch

  const Phrase ph = voice_phrase(v);
  if (!ph.tones || ph.count == 0) return;

  s_seq = ph.tones;
  s_len = ph.count;
  s_peak = peak;
  s_idx = -1;             // start on the next loop pass
  s_last_ctrl_ms = 0;
}

void chime_play(Chime c, uint8_t ramp) {
  switch (c) {
    case Chime::Tier1Alarm: voice_play(Voice::Alarm,    ramp); break;
    case Chime::Tier2Warn:  voice_play(Voice::Warn,     ramp); break;
    case Chime::AllClear:   voice_play(Voice::AllClear, ramp); break;
    case Chime::Sunrise:    voice_play(Voice::Sunrise,  ramp); break;
  }
}

void voice_loop(uint32_t now_ms) {
  if (s_pin < 0 || !s_seq) return;

  bool boundary = false;
  if (s_idx < 0) {
    s_idx = 0;
    s_tone_start_ms = now_ms;
    boundary = true;
  }

  uint32_t elapsed = now_ms - s_tone_start_ms;
  if ((int32_t)(elapsed - s_seq[s_idx].ms) >= 0) {
    s_idx++;
    if (s_idx >= s_len) {  // phrase done
      tone_off();
      s_seq = nullptr;
      s_idx = -1;
      return;
    }
    s_tone_start_ms = now_ms;
    elapsed = 0;
    boundary = true;
  }

  // Throttle in-note updates, but always refresh on a note boundary so each
  // note speaks crisply the instant it begins.
  if (!boundary && (int32_t)(now_ms - s_last_ctrl_ms) < (int32_t)CTRL_MS) return;
  s_last_ctrl_ms = now_ms;

  const Tone& n = s_seq[s_idx];
  const uint16_t f = voice_freq_at(n, (uint16_t)elapsed);
  if (f == 0) {           // a rest: silent, but stay latched in the phrase
    tone_duty(0);
    return;
  }
  tone_freq(f);
  const uint16_t amp = voice_env_amp(n.env, (uint16_t)elapsed, n.ms);  // 0..256
  tone_duty((uint8_t)((uint32_t)s_peak * amp / 256u));
}

bool voice_active() { return s_seq != nullptr; }

void voice_stop() { tone_off(); s_seq = nullptr; s_idx = -1; }

void voice_set_night(bool night) { s_night = night; }

void voice_set_volume(uint8_t vol) {
  if (vol > 4) vol = 4;
  if (vol == s_volume) return;
  s_volume = vol;
  prefs_save();
}
uint8_t voice_volume() { return s_volume; }

void voice_set_interactions(bool on) {
  if (on == s_interactions) return;
  s_interactions = on;
  prefs_save();
}
bool voice_interactions() { return s_interactions; }

}  // namespace canary::hal
