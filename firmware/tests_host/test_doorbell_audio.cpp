#include <string>
// Host tests for common/doorbell/doorbell_audio.h — the Vision Doorbell's
// voice. Every phrase is a promise to the visitor and to the neighbors:
// it starts and ends in silence (no click), it never clips, it finishes
// when the score says, volume 0 is silence and full volume reaches full
// scale (the whole point of a speaker that must carry over a street), and
// the five phrases are five different things to hear.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "../common/doorbell/doorbell_audio.h"

using doorbell::Phrase;
using doorbell::VoiceState;

static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      return 1;                                                              \
    }                                                                        \
    ++g_checks;                                                              \
  } while (0)

static const Phrase kAll[] = {Phrase::CHIME, Phrase::TICK, Phrase::WAIT, Phrase::LEAVE, Phrase::NO};

struct Render {
  int32_t n = 0;        // samples until idle
  int32_t peak = 0;     // max |sample|
  int32_t first = 0;    // |first sample|
  int32_t last = 0;     // |last non-zero sample|
  int32_t max_step = 0; // max |delta| between consecutive samples
  int64_t sum = 0;      // DC
};

static Render render(Phrase p, uint8_t vol) {
  VoiceState s = doorbell::audio_begin(vol);
  doorbell::audio_play(&s, p);
  Render r;
  int32_t prev = 0;
  int32_t last_nz = 0;
  while (doorbell::audio_active(&s) && r.n < 10 * (int32_t)doorbell::kSampleRate) {
    const int32_t x = doorbell::audio_sample(&s);
    if (r.n == 0) r.first = x < 0 ? -x : x;
    const int32_t a = x < 0 ? -x : x;
    if (a > r.peak) r.peak = a;
    const int32_t d = x - prev;
    const int32_t ad = d < 0 ? -d : d;
    if (ad > r.max_step) r.max_step = ad;
    if (x != 0) last_nz = a;
    r.sum += x;
    prev = x;
    r.n++;
  }
  r.last = last_nz;
  return r;
}

static int test_sine_table() {
  // the folded quarter wave is a sine: zero, peak, zero, trough, and odd
  CHECK(doorbell::sine_q15(0) == 0);
  CHECK(doorbell::sine_q15(16384) >= 32700);
  CHECK(std::abs((int)doorbell::sine_q15(32768)) <= 300);
  CHECK(doorbell::sine_q15(49152) <= -32700);
  for (uint32_t ph = 0; ph < 65536; ph += 97) {
    const double ref = std::sin(ph * 2.0 * M_PI / 65536.0) * 32767.0;
    CHECK(std::fabs(doorbell::sine_q15((uint16_t)ph) - ref) < 420);   // < 1.3 % of full scale
  }
  return 0;
}

static int test_every_phrase_is_clean() {
  for (Phrase p : kAll) {
    const Render r = render(p, 100);
    const doorbell::Score& sc = doorbell::score_of(p);
    CHECK(r.n > 0);
    CHECK(r.n <= (int32_t)doorbell::ms_to_samples(sc.length_ms) + 1);   // over when the score says
    CHECK(r.first < 1200);                                                // starts from silence (attack)
    CHECK(r.last < 400);                                                  // ends in silence (decayed)
    CHECK(r.peak <= 32767);                                               // never clips
    // never a full-scale flip between two samples. A bell at 8 kHz is a
    // steppy wave by nature (two voices with their octave partials can move
    // ~37000 counts in one sample at full swing); what makes it click-free
    // is the attack from silence and the decay to silence held above.
    CHECK(r.max_step < 50000);
    // no DC: the mean sits near zero over the phrase
    CHECK(std::llabs(r.sum) / r.n < 600);
  }
  return 0;
}

static int test_volume_law() {
  CHECK(doorbell::volume_q15(0) == 0);
  CHECK(doorbell::volume_q15(100) == 32767);
  CHECK(doorbell::volume_q15(50) < doorbell::volume_q15(71));            // monotone
  CHECK(doorbell::volume_q15(50) >= 32767 / 5 && doorbell::volume_q15(50) <= 32767 / 3);  // ~a quarter of full
  CHECK(render(Phrase::CHIME, 0).peak == 0);                             // silence at 0
  CHECK(render(Phrase::CHIME, 100).peak >= 0.80 * 32767);                // full scale reaches full scale
  CHECK(render(Phrase::CHIME, 60).peak < render(Phrase::CHIME, 100).peak);
  CHECK(doorbell::clamp_volume(-3) == 0 && doorbell::clamp_volume(250) == 100);
  return 0;
}

static int test_duty_mapping() {
  CHECK(doorbell::duty10(0) == 512);
  CHECK(doorbell::duty10(32767) == 1023);
  CHECK(doorbell::duty10(-32767) == 0);   // an arithmetic shift of -32767 >> 6 is -512: the floor
  CHECK(doorbell::duty10(-32768) == 0);
  return 0;
}

static int test_phrases_differ() {
  // five different things to hear: the first note's pitch and the shape
  const doorbell::Score& chime = doorbell::score_of(Phrase::CHIME);
  const doorbell::Score& wait  = doorbell::score_of(Phrase::WAIT);
  const doorbell::Score& leave = doorbell::score_of(Phrase::LEAVE);
  const doorbell::Score& no    = doorbell::score_of(Phrase::NO);
  const doorbell::Score& tick  = doorbell::score_of(Phrase::TICK);
  CHECK(chime.count == 2 && chime.notes[0].hz > chime.notes[1].hz);       // ding-dong falls
  CHECK(wait.count == 3 && wait.notes[0].hz < wait.notes[1].hz && wait.notes[1].hz < wait.notes[2].hz);
  CHECK(leave.count == 3 && leave.notes[0].hz > leave.notes[1].hz && leave.notes[1].hz > leave.notes[2].hz);
  CHECK(no.count == 2 && no.notes[0].hz == no.notes[1].hz && no.notes[0].hz < chime.notes[1].hz);
  CHECK(tick.count == 1 && tick.length_ms <= 300 && tick.notes[0].amp < 128);   // soft and short
  CHECK(render(Phrase::TICK, 100).peak < render(Phrase::CHIME, 100).peak / 2);
  CHECK(doorbell::score_of(Phrase::NONE).count == 0);
  return 0;
}

static int test_replay_and_stop() {
  VoiceState s = doorbell::audio_begin(80);
  doorbell::audio_play(&s, Phrase::CHIME);
  for (int i = 0; i < 2000; ++i) doorbell::audio_sample(&s);
  CHECK(doorbell::audio_active(&s));
  doorbell::audio_play(&s, Phrase::WAIT);                                // replace mid-phrase
  CHECK(doorbell::audio_active(&s));
  CHECK(std::abs((int)doorbell::audio_sample(&s)) < 1200);               // re-entered from an attack: no step
  doorbell::audio_stop(&s);
  CHECK(!doorbell::audio_active(&s) && doorbell::audio_sample(&s) == 0);
  // NONE never plays
  doorbell::audio_play(&s, Phrase::NONE);
  CHECK(!doorbell::audio_active(&s));
  return 0;
}

static int test_names() {
  CHECK(std::string(doorbell::phrase_name(Phrase::WAIT)) == "we're coming");
  CHECK(std::string(doorbell::phrase_name(Phrase::LEAVE)) == "leave it");
  CHECK(std::string(doorbell::phrase_name(Phrase::NO)) == "no thanks");
  CHECK(std::string(doorbell::phrase_name(Phrase::NONE)) == "—");
  return 0;
}

int main() {
  int (*tests[])() = {test_sine_table, test_every_phrase_is_clean, test_volume_law, test_duty_mapping,
                      test_phrases_differ, test_replay_and_stop, test_names};
  for (auto t : tests) {
    if (t() != 0) return 1;
  }
  std::printf("doorbell audio: %d checks passed\n", g_checks);
  return 0;
}
