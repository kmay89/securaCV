// Host test for the microphone decision core (canary/io/mic_logic.h).
//
// Pins the two promises the mic-bearing dash (Waveshare 4.3C) makes in
// docs/hardware/display_mic_variant.md, without a board:
//   - THE GATE: off by default; armed alone never starts the driver while
//     the audio pins are unset; the indicator IS the running bit (no state
//     exists where the driver runs unlit, or the chip lies lit); disarm is
//     a Stop action (the runtime maps it to i2s_driver_uninstall — hard
//     mute, not a flag).
//   - THE EARS: only the two regulated alarm grammars raise events (smoke
//     T3, CO T4), each needing two on-grammar cycles; doorbells, knocks,
//     speech-shaped noise, and a stale streak after silence never do.
// Run in CI by the "mic logic host test" step in firmware.yml; prints
// "ALL MIC LOGIC TESTS PASSED" on success. Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_mic_logic.cpp -o t && ./t

#include "canary/io/mic_logic.h"

#include <cstdio>
#include <cstring>

using namespace canary::io::mic;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ── The gate ────────────────────────────────────────────────────────────────

static void test_gate_off_by_default() {
  Gate g;
  CHECK(g.update() == Action::None, "fresh gate does nothing");
  CHECK(!g.running && !indicator_lit(g), "fresh gate: not running, chip dark");
}

static void test_gate_pins_block_arming() {
  Gate g;
  g.armed = true;  // user armed it, but AUDIO_PIN_* are still -1
  CHECK(g.update() == Action::None, "armed with unset pins never starts");
  CHECK(!g.running, "the mics stay provably un-driven");
  g.pins_ok = true;
  CHECK(g.update() == Action::Start, "pins filled at bench -> start");
  CHECK(g.running && indicator_lit(g), "driver up == chip lit, same bit");
}

static void test_gate_disarm_is_a_hard_stop() {
  Gate g;
  g.armed = true;
  g.pins_ok = true;
  CHECK(g.update() == Action::Start, "start");
  g.armed = false;
  CHECK(g.update() == Action::Stop, "disarm -> Stop (driver uninstalled)");
  CHECK(!g.running && !indicator_lit(g), "stopped == chip dark, same bit");
  CHECK(g.update() == Action::None, "idempotent after stop");
}

static void test_indicator_cannot_desync() {
  // The invariant is structural: indicator_lit IS the running bit. Walk
  // every reachable state and confirm there is no lit-but-stopped or
  // running-but-dark combination to even represent.
  Gate g;
  for (int armed = 0; armed <= 1; armed++) {
    for (int pins = 0; pins <= 1; pins++) {
      g.armed = armed;
      g.pins_ok = pins;
      g.update();
      CHECK(indicator_lit(g) == g.running, "indicator == driver, always");
      CHECK(g.running == (armed && pins), "runs iff armed AND pins ok");
    }
  }
}

// ── The ears ────────────────────────────────────────────────────────────────

// Drive a beep pattern through Envelope+CadenceDetector: `beeps` pulses of
// `on_ms`, separated by `gap_ms`, then `rest_ms` of silence. Returns the
// last non-None detection seen. Frames every 25 ms, RMS 2000 loud / 0 quiet.
static Detection play_group(Envelope& env, CadenceDetector& det, uint32_t& t,
                            int beeps, uint16_t on_ms, uint16_t gap_ms,
                            uint32_t rest_ms) {
  Detection last;
  auto frames = [&](uint32_t span, uint16_t rms) {
    for (uint32_t i = 0; i < span; i += 25) {
      const bool loud = env.update(rms);
      const Detection d = det.update(loud, t);
      if (d.event != Event::None) last = d;
      t += 25;
    }
  };
  for (int b = 0; b < beeps; b++) {
    frames(on_ms, 2000);
    frames(b + 1 < beeps ? gap_ms : 0, 0);
  }
  frames(rest_ms, 0);
  return last;
}

static void test_t3_smoke_needs_two_cycles() {
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  Detection d = play_group(env, det, t, 3, 500, 500, 1500);
  CHECK(d.event == Event::None, "one T3 group alone is a horn, not an alarm");
  d = play_group(env, det, t, 3, 500, 500, 1500);
  CHECK(d.event == Event::SmokeT3, "second on-grammar cycle raises smoke T3");
  CHECK(d.cycles == 2 && d.confidence >= 50, "cycles + confidence carried");
  d = play_group(env, det, t, 3, 500, 500, 1500);
  CHECK(d.event == Event::SmokeT3 && d.cycles == 3, "streak keeps growing");
}

static void test_t4_co_detects() {
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  play_group(env, det, t, 4, 100, 100, 5000);
  const Detection d = play_group(env, det, t, 4, 100, 100, 5000);
  CHECK(d.event == Event::CoT4, "two T4 cycles raise CO");
}

static void test_doorbell_and_speech_never_alarm() {
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  // Ding-dong: two long tones.
  Detection d = play_group(env, det, t, 2, 700, 300, 2000);
  CHECK(d.event == Event::None, "doorbell is not an alarm");
  // Speech-shaped: bursts too long/uneven for either grammar.
  d = play_group(env, det, t, 3, 1200, 400, 2000);
  CHECK(d.event == Event::None, "long slurred bursts rejected (duration window)");
  d = play_group(env, det, t, 5, 150, 150, 2000);
  CHECK(d.event == Event::None, "five-beat patter rejected (count window)");
}

static void test_silence_resets_a_stale_streak() {
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  play_group(env, det, t, 3, 500, 500, 1500);   // streak = 1, then it stops
  Detection d = play_group(env, det, t, 0, 0, 0, 20000);  // dead room > reset gap
  CHECK(d.event == Event::None, "silence raises nothing");
  d = play_group(env, det, t, 3, 500, 500, 1500);
  CHECK(d.event == Event::None,
        "a stale streak lends no credibility after the room went quiet");
}

static void test_switching_alarm_type_needs_fresh_two_cycles() {
  // One T3 cycle then a T4 group must NOT fire: the streak is type-scoped,
  // so a different grammar resets the count to one. Only a second matching
  // T4 cycle raises CO. (A room where a smoke horn and a CO horn each honk
  // once should not be read as a confirmed CO alarm.)
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  play_group(env, det, t, 3, 500, 500, 1500);        // streak: SmokeT3 = 1
  Detection d = play_group(env, det, t, 4, 100, 100, 5000);
  CHECK(d.event == Event::None, "type switch resets the streak to one");
  d = play_group(env, det, t, 4, 100, 100, 5000);
  CHECK(d.event == Event::CoT4, "second matching T4 cycle raises CO");
}

static void test_count_and_duration_windows_are_disjoint() {
  // The count collision (3 vs 4 beeps) is disambiguated by the duration
  // window, so a miscount can't cross-classify:
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  // 3 SHORT beeps (T4-length): right count for T3, wrong duration -> neither.
  play_group(env, det, t, 3, 100, 100, 1500);
  Detection d = play_group(env, det, t, 3, 100, 100, 1500);
  CHECK(d.event == Event::None, "3 short beeps are not T3 (duration window)");
  // 4 LONG beeps (T3-length): right count for T4, wrong duration -> neither.
  Envelope env2;
  CadenceDetector det2;
  uint32_t t2 = 0;
  play_group(env2, det2, t2, 4, 500, 500, 5000);
  d = play_group(env2, det2, t2, 4, 500, 500, 5000);
  CHECK(d.event == Event::None, "4 long beeps are not T4 (duration window)");
}

static void test_collapsed_timing_fails_safe() {
  // The runtime feeds this core one detection per 20 ms audio frame on the
  // AUDIO clock. If a caller ever collapsed several frames onto one
  // timestamp (the pre-fix runtime bug: a slow main loop draining a DMA
  // batch with a single millis() stamp), every beep would measure ~0 ms.
  // A zero-duration group must NEVER classify — the failure mode is a
  // MISSED alarm, never a phantom one. This pins that safety property at
  // the core so the bug cannot regress into false positives.
  CadenceDetector det;
  // Three beeps that rise and fall at the SAME instant (dur = 0), then a
  // gap long enough to close the group.
  for (int b = 0; b < 3; b++) {
    det.update(true, 1000);   // rising, now == edge_ms after the prior edge
    det.update(false, 1000);  // falling at the same now -> dur 0
  }
  Detection d;
  const uint32_t close_at = 1000 + CadenceDetector::GROUP_GAP_MS + 100;
  for (uint32_t q = 1000; q <= close_at; q += 25) d = det.update(false, q);
  CHECK(d.event == Event::None, "zero-duration beeps never raise an alarm");
}

static void test_confidence_grows_and_caps() {
  Envelope env;
  CadenceDetector det;
  uint32_t t = 0;
  play_group(env, det, t, 3, 500, 500, 1500);  // cycle 1: no fire
  uint8_t last_conf = 0;
  for (int i = 0; i < 8; i++) {
    Detection d = play_group(env, det, t, 3, 500, 500, 1500);
    CHECK(d.event == Event::SmokeT3, "sustained T3 keeps firing");
    CHECK(d.confidence >= last_conf, "confidence is non-decreasing");
    CHECK(d.confidence <= 95, "confidence caps at 95 (never a bare 100)");
    last_conf = d.confidence;
  }
  CHECK(last_conf == 95, "a long-standing alarm saturates confidence");
}

static void test_envelope_hysteresis() {
  Envelope env;
  CHECK(!env.update(800), "below on-threshold stays quiet");
  CHECK(env.update(950), "above on-threshold goes loud");
  CHECK(env.update(700), "between thresholds holds loud (no chatter)");
  CHECK(!env.update(500), "below off-threshold releases");
}

static void test_wire_names_match_the_real_classifier_vocabulary() {
  // classify_event(): "smoke" and "co_alarm" both land Sev::Alert — these
  // names are chosen to hit those substrings (the demo-script host test
  // pins the classifier itself; here we pin our spelling).
  const char* smoke = event_wire_name(Event::SmokeT3);
  const char* co = event_wire_name(Event::CoT4);
  CHECK(smoke && std::strstr(smoke, "smoke") != nullptr,
        "smoke event carries the classifier's Alert substring");
  CHECK(co && std::strstr(co, "co_alarm") != nullptr,
        "CO event carries the classifier's Alert substring");
  CHECK(event_wire_name(Event::None)[0] == '\0', "None has no wire name");
}

int main() {
  test_gate_off_by_default();
  test_gate_pins_block_arming();
  test_gate_disarm_is_a_hard_stop();
  test_indicator_cannot_desync();
  test_t3_smoke_needs_two_cycles();
  test_t4_co_detects();
  test_doorbell_and_speech_never_alarm();
  test_silence_resets_a_stale_streak();
  test_switching_alarm_type_needs_fresh_two_cycles();
  test_count_and_duration_windows_are_disjoint();
  test_collapsed_timing_fails_safe();
  test_confidence_grows_and_caps();
  test_envelope_hysteresis();
  test_wire_names_match_the_real_classifier_vocabulary();

  if (g_fail == 0) {
    std::printf("ALL MIC LOGIC TESTS PASSED\n");
    return 0;
  }
  std::printf("%d MIC LOGIC TEST(S) FAILED\n", g_fail);
  return 1;
}
