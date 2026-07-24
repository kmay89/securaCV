// tests_host/dump_voice_score.cpp — emit the Canary Voice grammar as JSON.
//
// This is the anti-rot spine of the browser sound preview
// (canary-local/voice/): rather than hand-transcribe voice_score.h into
// JavaScript (which silently drifts the moment a note changes), the preview's
// data is GENERATED here — by the real compiler, from the real header, calling
// the real functions. Every signature is rendered to a flat [freq, amp] frame
// timeline exactly the way hal/chime.cpp's streamer walks it, and the volume
// model is emitted as the table voice_peak_duty() actually produces. The
// preview then just plays frames and reads the table; it owns no audio math,
// so it cannot diverge. canary-local/tools/gen_voice_preview.mjs runs this and
// a CI drift gate (canary-local.yml) fails if the committed page is stale.
//
// Deterministic output (no timestamps / no host paths) so the drift gate
// compares bytes.
#include "canary/hal/voice_score.h"

#include <cstdio>
#include <cstddef>

using namespace canary::hal;

// Stable string ids, in Voice enum order. The static_assert is a rot guard:
// add a Voice and this file won't build until the id list catches up (and
// test_voice_score already fails if a Voice has no score).
static const char* NAMES[] = {
    "Alarm", "Warn", "AllClear", "Sunrise", "Boot", "Heartbeat",
    "JoinSuccess", "Tap", "PageTurn", "AckConfirm", "MuteOn", "MuteOff",
};
static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == (size_t)Voice::Count,
              "NAMES is out of step with enum Voice — update it");

static const char* CAT_NAMES[] = {"Interaction", "Ambient", "Notice", "Wake",
                                  "Alert"};

// Control tick: mirror hal/chime.cpp's CTRL_MS so the preview's frame cadence
// matches what the device renders.
static constexpr int CTRL_MS = 6;

int main() {
  std::printf("{\n");

  // Volume model, emitted as the exact table voice_peak_duty() produces:
  // peak[cat][vol][night] at ramp=2. The preview reads this — never the
  // constants — so the loudness policy can't drift either.
  std::printf("  \"catNames\": [");
  for (int c = 0; c < 5; c++)
    std::printf("%s\"%s\"", c ? ", " : "", CAT_NAMES[c]);
  std::printf("],\n");
  std::printf("  \"peak\": [");
  for (int c = 0; c < 5; c++) {
    std::printf("%s[", c ? ", " : "");
    for (int v = 0; v <= 4; v++) {
      std::printf("%s[%u, %u]", v ? ", " : "",
                  voice_peak_duty((VoiceCat)c, (uint8_t)v, false, 2),
                  voice_peak_duty((VoiceCat)c, (uint8_t)v, true, 2));
    }
    std::printf("]");
  }
  std::printf("],\n");

  // Per-signature: category + a rendered [freq, amp] frame timeline. Frames
  // are produced by walking each phrase exactly like voice_loop(): per note,
  // sample every CTRL_MS with the real voice_freq_at()/voice_env_amp(). A rest
  // is [0, 0]. amp is 0..256; freq is Hz (0 = silent).
  std::printf("  \"voices\": {\n");
  for (int i = 0; i < (int)Voice::Count; i++) {
    const Voice v = (Voice)i;
    const Phrase ph = voice_phrase(v);
    std::printf("    \"%s\": {\"cat\": %d, \"frames\": [", NAMES[i],
                (int)voice_category(v));
    bool first = true;
    for (int k = 0; k < ph.count; k++) {
      const Tone& n = ph.tones[k];
      for (int t = 0; t < n.ms; t += CTRL_MS) {
        const uint16_t f = voice_freq_at(n, (uint16_t)t);
        const uint16_t a =
            f == 0 ? 0 : voice_env_amp(n.env, (uint16_t)t, n.ms);
        std::printf("%s[%u,%u]", first ? "" : ",", f, a);
        first = false;
      }
    }
    std::printf("]}%s\n", i + 1 < (int)Voice::Count ? "," : "");
  }
  std::printf("  }\n}\n");
  return 0;
}
