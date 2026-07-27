// Host test for the Canary Voice acoustic core (canary/hal/voice_score.h).
//
// The sound engine's hardware half (the LEDC streamer) can only be judged at
// the bench, but everything that makes the grammar *correct* is pure and
// pinned here — the same discipline as fleet_model/mode_registry:
//   - THE PALETTE: every Voice resolves to a non-empty score with sane note
//     durations; the pleasant signatures draw only from the pentatonic set;
//     the alert stays on its frozen IEC pitches, outside that set.
//   - THE CONTOURS: Sunrise/Boot rise, AllClear falls, MuteOn/MuteOff mirror,
//     and the security voices never rise — the semantics you can hear.
//   - THE ENVELOPES: every shape starts and ends near zero (the de-click that
//     makes a note breathe instead of beep); SWELL peaks in the middle.
//   - THE VOLUME MODEL: monotone in the knob, interactions silent at night,
//     the Tier-1 alert audible at every volume including Off, and no UI blip
//     ever louder than a fault.
// Prints "ALL VOICE SCORE TESTS PASSED" on success. Build (from repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_voice_score.cpp -o t && ./t

#include "canary/hal/voice_score.h"

#include <cstdio>

using namespace canary::hal;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

static bool is_pentatonic(uint16_t f) {
  const uint16_t P[] = {NOTE_C6, NOTE_D6, NOTE_E6, NOTE_G6,
                        NOTE_A6, NOTE_C7, NOTE_D7, NOTE_E7};
  for (uint16_t p : P)
    if (f == p) return true;
  return false;
}

// ── The palette ─────────────────────────────────────────────────────────

static void test_every_voice_has_a_score() {
  for (uint8_t i = 0; i < (uint8_t)Voice::Count; i++) {
    Phrase ph = voice_phrase((Voice)i);
    CHECK(ph.tones != nullptr && ph.count > 0, "voice resolves to a phrase");
    for (uint8_t k = 0; k < ph.count; k++) {
      const Tone& t = ph.tones[k];
      CHECK(t.ms > 0 && t.ms <= 800, "note duration in (0, 800] ms");
      CHECK(t.freq_hz == 0 || (t.freq_hz >= 800 && t.freq_hz <= 3200),
            "audible pitch band or rest");
    }
  }
  // An out-of-range enum makes no sound rather than reading garbage.
  Phrase bad = voice_phrase(Voice::Count);
  CHECK(bad.tones == nullptr && bad.count == 0, "bad enum -> silent phrase");
}

static void test_pleasant_voices_are_pentatonic() {
  const Voice pleasant[] = {Voice::Boot,      Voice::Sunrise, Voice::Heartbeat,
                            Voice::JoinSuccess, Voice::Tap,   Voice::PageTurn,
                            Voice::AckConfirm, Voice::MuteOn, Voice::MuteOff};
  for (Voice v : pleasant) {
    Phrase ph = voice_phrase(v);
    for (uint8_t k = 0; k < ph.count; k++) {
      const Tone& t = ph.tones[k];
      if (t.freq_hz != 0) CHECK(is_pentatonic(t.freq_hz), "pleasant note in scale");
      if (t.freq_end_hz != 0)
        CHECK(is_pentatonic(t.freq_end_hz), "pleasant glide target in scale");
    }
  }
}

static void test_alarm_grammar_frozen() {
  // The Tier-1 alarm keeps the safety-critical IEC pitches, deliberately
  // OUTSIDE the pretty scale, and its fast tempo (every voiced note <= 90 ms).
  Phrase ph = voice_phrase(Voice::Alarm);
  bool saw_2600 = false, saw_3100 = false;
  for (uint8_t k = 0; k < ph.count; k++) {
    const Tone& t = ph.tones[k];
    if (t.freq_hz == 2600) saw_2600 = true;
    if (t.freq_hz == 3100) saw_3100 = true;
    if (t.freq_hz != 0) {
      CHECK(!is_pentatonic(t.freq_hz), "alarm pitch is not a pretty note");
      CHECK(t.ms <= 90, "alarm pulses stay fast");
    }
  }
  CHECK(saw_2600 && saw_3100, "alarm alternates 2.6/3.1 kHz");
}

// ── The contours (semantics you can hear) ────────────────────────────────

static uint16_t first_pitch(Voice v) {
  Phrase ph = voice_phrase(v);
  for (uint8_t k = 0; k < ph.count; k++)
    if (ph.tones[k].freq_hz) return ph.tones[k].freq_end_hz ? ph.tones[k].freq_end_hz
                                                            : ph.tones[k].freq_hz;
  return 0;
}
static uint16_t last_pitch(Voice v) {
  Phrase ph = voice_phrase(v);
  uint16_t f = 0;
  for (uint8_t k = 0; k < ph.count; k++)
    if (ph.tones[k].freq_hz)
      f = ph.tones[k].freq_end_hz ? ph.tones[k].freq_end_hz : ph.tones[k].freq_hz;
  return f;
}

static void test_contours() {
  CHECK(last_pitch(Voice::Sunrise) > first_pitch(Voice::Sunrise), "sunrise rises");
  CHECK(last_pitch(Voice::Boot) > first_pitch(Voice::Boot), "boot rises");
  CHECK(last_pitch(Voice::JoinSuccess) > first_pitch(Voice::JoinSuccess),
        "join rises to celebrate");
  CHECK(last_pitch(Voice::AckConfirm) > first_pitch(Voice::AckConfirm),
        "ack resolves upward");
  // AllClear falls: the security resolution voice descends.
  CHECK(last_pitch(Voice::AllClear) < first_pitch(Voice::AllClear),
        "all-clear falls");
  // MuteOn falls, MuteOff rises: one gesture, mirrored.
  CHECK(last_pitch(Voice::MuteOn) < first_pitch(Voice::MuteOn), "mute-on falls");
  CHECK(last_pitch(Voice::MuteOff) > first_pitch(Voice::MuteOff), "mute-off rises");
}

// ── The envelopes ────────────────────────────────────────────────────────

static void test_envelopes_declick() {
  const uint8_t envs[] = {ENV_SOFT, ENV_PLUCK, ENV_SWELL, ENV_HARD};
  for (uint8_t e : envs) {
    CHECK(voice_env_amp(e, 0, 200) < 80, "attack starts low (no click in)");
    CHECK(voice_env_amp(e, 200, 200) < 80, "release ends low (no click out)");
    // Every shape is clearly audible mid-note; PLUCK is already decaying by
    // the midpoint (it front-loads its energy), so it gets a lower bar.
    const uint16_t bar = (e == ENV_PLUCK) ? 120 : 180;
    CHECK(voice_env_amp(e, 100, 200) > bar, "sustains audibly in the middle");
  }
  // SWELL peaks in the middle, not at the edges.
  CHECK(voice_env_amp(ENV_SWELL, 100, 200) >= voice_env_amp(ENV_SWELL, 20, 200),
        "swell grows toward center");
  // PLUCK is loudest right after its instant attack, then decays.
  CHECK(voice_env_amp(ENV_PLUCK, 5, 200) > voice_env_amp(ENV_PLUCK, 180, 200),
        "pluck decays");
}

static void test_glissando_and_warble() {
  Tone glide{NOTE_C6, NOTE_C7, 100, ENV_SOFT, 0, 0};
  CHECK(voice_freq_at(glide, 0) == NOTE_C6, "glide starts at f0");
  CHECK(voice_freq_at(glide, 100) == NOTE_C7, "glide ends at f1");
  CHECK(voice_freq_at(glide, 50) > NOTE_C6 && voice_freq_at(glide, 50) < NOTE_C7,
        "glide passes through the middle");
  // Warble stays bounded within its depth around the base pitch.
  Tone trill{NOTE_E6, 0, 200, ENV_SOFT, 30, 12};  // +/-3%
  for (uint16_t t = 0; t <= 200; t += 5) {
    uint16_t f = voice_freq_at(trill, t);
    CHECK(f > NOTE_E6 - NOTE_E6 * 5 / 100 && f < NOTE_E6 + NOTE_E6 * 5 / 100,
          "warble bounded near base pitch");
  }
  // A rest has no pitch.
  Tone rest{0, 0, 40, ENV_SOFT, 0, 0};
  CHECK(voice_freq_at(rest, 10) == 0, "rest is silent");
}

// ── The volume model ─────────────────────────────────────────────────────

static void test_volume_model() {
  // Monotone in the knob (day, Notice category, ramp full).
  uint8_t prev = 0;
  for (uint8_t v = 0; v <= 4; v++) {
    uint8_t d = voice_peak_duty(VoiceCat::Notice, v, /*night=*/false, 2);
    CHECK(d >= prev, "louder knob is never quieter");
    prev = d;
  }
  CHECK(voice_peak_duty(VoiceCat::Notice, 0, false, 2) == 0, "Off is silent");

  // A UI blip is never as loud as a fault at the same setting.
  CHECK(voice_peak_duty(VoiceCat::Interaction, 4, false, 2) <
            voice_peak_duty(VoiceCat::Notice, 4, false, 2),
        "interaction ceiling under notice");

  // Night silences the gentle categories, keeps Wake and Alert.
  CHECK(voice_peak_duty(VoiceCat::Interaction, 4, true, 2) == 0,
        "interactions silent at night");
  CHECK(voice_peak_duty(VoiceCat::Ambient, 4, true, 2) == 0,
        "heartbeat silent at night");
  CHECK(voice_peak_duty(VoiceCat::Wake, 3, true, 2) > 0, "wake sounds at night");

  // The Tier-1 floor: audible at EVERY volume, including Off, day or night.
  for (uint8_t v = 0; v <= 4; v++) {
    CHECK(voice_peak_duty(VoiceCat::Alert, v, false, 2) >= 80,
          "alert audible at any volume (day)");
    CHECK(voice_peak_duty(VoiceCat::Alert, v, true, 0) >= 80,
          "alert audible at any volume even mid-ramp at night");
  }
  // Ramp escalates the alarm.
  CHECK(voice_peak_duty(VoiceCat::Alert, 4, false, 0) <=
            voice_peak_duty(VoiceCat::Alert, 4, false, 2),
        "alarm ramp escalates soft -> full");
}

int main() {
  test_every_voice_has_a_score();
  test_pleasant_voices_are_pentatonic();
  test_alarm_grammar_frozen();
  test_contours();
  test_envelopes_declick();
  test_glissando_and_warble();
  test_volume_model();

  if (g_fail) {
    std::printf("VOICE SCORE TESTS FAILED: %d\n", g_fail);
    return 1;
  }
  std::printf("ALL VOICE SCORE TESTS PASSED\n");
  return 0;
}
