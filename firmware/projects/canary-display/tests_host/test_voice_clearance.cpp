// Host test: acoustic clearance for the Canary Voice palette.
//
// Two properties this pins, so a future edit can't quietly make the display
// sound illegal-adjacent or dangerously confusing (docs/hardware/
// display_sound_clearance.md):
//
//   NON-CONFUSION with regulated life-safety signals. A home-status display
//   is NOT a fire or CO alarm, and none of its sounds may reproduce the two
//   evacuation cadences a household is trained to obey:
//     · ISO 8201 / NFPA 72 Temporal-Three (T3) — fire: ~0.5 s ON, ~0.5 s OFF,
//       three times, then a long gap. The recognized "get out" pattern.
//     · Temporal-Four (T4) — carbon monoxide: four short ~0.1 s pulses, one
//       tone, then a long silence.
//   Reproducing either could make someone ignore the real appliance, or
//   panic at a status chirp. The detectors below match each canonical cadence
//   with generous tolerance and assert NO signature trips them.
//
//   ORIGINALITY, made checkable. Our claim (see the doc) is that every sound
//   is an original composition over the public-domain major-pentatonic scale,
//   and the alarm uses only the IEC 60601-1-8 *design principle* (fast, wide
//   pitch) via two bare frequencies — never a sampled or standardized melody.
//   The invariants here back that: the pleasant voices live in the pentatonic
//   set; the alarm uses only 2.6/3.1 kHz and stays fast.
//
// Prints "ALL VOICE CLEARANCE TESTS PASSED" on success. Build (repo root):
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//   firmware/projects/canary-display/tests_host/test_voice_clearance.cpp -o t && ./t

#include "canary/hal/voice_score.h"

#include <cstdio>
#include <cstddef>
#include <vector>

using namespace canary::hal;

static int g_fail = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

static const char* NAMES[] = {
    "Alarm", "Warn", "AllClear", "Sunrise", "Boot", "Heartbeat",
    "JoinSuccess", "Tap", "PageTurn", "AckConfirm", "MuteOn", "MuteOff",
};
static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == (size_t)Voice::Count,
              "NAMES out of step with enum Voice");

// One ON (voiced) or OFF (rest) segment of a phrase.
struct Seg { bool on; int ms; uint16_t pitch; };

static std::vector<Seg> segments(Voice v) {
  std::vector<Seg> s;
  const Phrase ph = voice_phrase(v);
  for (int k = 0; k < ph.count; k++) {
    const Tone& t = ph.tones[k];
    const bool on = t.freq_hz != 0;
    // Merge a segment into its predecessor when it is the same kind (defensive;
    // our scores already alternate ON/rest).
    if (!s.empty() && s.back().on == on) s.back().ms += t.ms;
    else s.push_back(Seg{on, (int)t.ms, t.freq_hz});
  }
  return s;
}

static bool near(int x, int lo, int hi) { return x >= lo && x <= hi; }

// ISO 8201 Temporal-Three: three (or more) ~0.5 s ON pulses separated by
// ~0.5 s OFF gaps. We flag any run of >=3 ON pulses each 350–650 ms with
// interior 350–650 ms gaps.
static bool matches_T3(const std::vector<Seg>& s) {
  int run = 0;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i].on && near(s[i].ms, 350, 650)) {
      // gap before this ON (if any) must also be ~0.5 s to continue a run
      if (run > 0) {
        const bool gap_ok = i >= 1 && !s[i - 1].on && near(s[i - 1].ms, 350, 650);
        run = gap_ok ? run + 1 : 1;
      } else {
        run = 1;
      }
      if (run >= 3) return true;
    } else if (s[i].on) {
      run = 0;
    }
  }
  return false;
}

// Temporal-Four (CO): exactly four short single-tone ON pulses (~0.1 s) with
// short (~0.1 s) interior gaps and nothing else voiced — the isolated
// "beep-beep-beep-beep." Rising/legato musical runs (tiny gaps, moving pitch)
// are explicitly NOT this.
static bool matches_T4(const std::vector<Seg>& s) {
  std::vector<const Seg*> ons;
  for (const auto& seg : s) if (seg.on) ons.push_back(&seg);
  if (ons.size() != 4) return false;
  uint16_t p0 = ons[0]->pitch;
  for (const Seg* o : ons) {
    if (!near(o->ms, 60, 160)) return false;                 // ~0.1 s pulses
    const int dp = (int)o->pitch - (int)p0;
    if (dp > (int)(p0 * 3 / 100) || dp < -(int)(p0 * 3 / 100)) return false;  // one tone
  }
  for (const auto& seg : s)                                   // interior gaps ~0.1 s
    if (!seg.on && !near(seg.ms, 60, 160)) return false;
  return true;
}

static void test_no_regulated_cadence() {
  for (int i = 0; i < (int)Voice::Count; i++) {
    const auto s = segments((Voice)i);
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s must not reproduce the fire (T3) cadence", NAMES[i]);
    CHECK(!matches_T3(s), msg);
    std::snprintf(msg, sizeof(msg), "%s must not reproduce the CO (T4) cadence", NAMES[i]);
    CHECK(!matches_T4(s), msg);
  }
}

// Self-check the detectors so the guard can't rot into a rubber stamp: a
// hand-built T3 and T4 phrase MUST trip them.
static void test_detectors_actually_fire() {
  std::vector<Seg> t3 = {{true,500,3000},{false,500,0},{true,500,3000},
                         {false,500,0},{true,500,3000}};
  CHECK(matches_T3(t3), "T3 detector fires on the real fire cadence");
  std::vector<Seg> t4 = {{true,100,3000},{false,100,0},{true,100,3000},{false,100,0},
                         {true,100,3000},{false,100,0},{true,100,3000}};
  CHECK(matches_T4(t4), "T4 detector fires on the real CO cadence");
}

// Originality invariants that back the clearance doc's claims.
static void test_originality_invariants() {
  const uint16_t PENT[] = {NOTE_C6, NOTE_D6, NOTE_E6, NOTE_G6,
                           NOTE_A6, NOTE_C7, NOTE_D7, NOTE_E7};
  auto pentatonic = [&](uint16_t f) {
    for (uint16_t p : PENT) if (f == p) return true;
    return false;
  };
  const Voice pleasant[] = {Voice::Boot, Voice::Sunrise, Voice::Heartbeat,
                            Voice::JoinSuccess, Voice::Tap, Voice::PageTurn,
                            Voice::AckConfirm, Voice::MuteOn, Voice::MuteOff};
  for (Voice v : pleasant) {
    const Phrase ph = voice_phrase(v);
    for (int k = 0; k < ph.count; k++) {
      const Tone& t = ph.tones[k];
      if (t.freq_hz) CHECK(pentatonic(t.freq_hz), "pleasant voices stay in the public-domain scale");
    }
  }
  // The alarm is the IEC *principle* (fast, wide pitch) via two bare tones —
  // not a sampled or standardized melody.
  const Phrase al = voice_phrase(Voice::Alarm);
  for (int k = 0; k < al.count; k++) {
    const Tone& t = al.tones[k];
    if (t.freq_hz) {
      CHECK(t.freq_hz == 2600 || t.freq_hz == 3100, "alarm uses only its two IEC frequencies");
      CHECK(t.ms <= 90, "alarm pulses stay fast (a burst, never a 0.5 s evacuation tone)");
    }
  }
}

int main() {
  test_no_regulated_cadence();
  test_detectors_actually_fire();
  test_originality_invariants();
  if (g_fail) { std::printf("VOICE CLEARANCE TESTS FAILED: %d\n", g_fail); return 1; }
  std::printf("ALL VOICE CLEARANCE TESTS PASSED\n");
  return 0;
}
