// firmware/tests_host/test_plumage.cpp — host tests for the plumage engine
// (firmware/common/color/plumage.*): the birdsong light-language behind the
// hallway nightlight.
//
// What is actually being defended here, in order of importance:
//
//   1. THE HONESTY INVARIANT. Plumage may only ever dress the calm. Under
//      Warn/Alert, and under safe_dark, its output must be BIT-IDENTICAL to
//      the look engine's — not merely "close", because the whole point of the
//      delegation in plumage.cpp is that there is one implementation of the
//      honest path. A song that survived an alarm at any amplitude would be
//      exactly the dishonesty the two-channel split was written to prevent.
//   2. IT CANNOT DARKEN THE LAMP. The song is added, never subtracted: a
//      hallway light that dips or blinks reads as a fault at 3 a.m.
//   3. IT IS REPRODUCIBLE PER DEVICE, AND DIFFERENT ACROSS DEVICES. Same id →
//      same song forever (which is what makes this file possible); different
//      ids → genuinely different voices, so two Canaries in one hallway never
//      fall into lockstep.
//   4. THE GATE HOLDS, IT DOES NOT BANK. Re-opening a long-shut gate must not
//      release a burst of stored-up speech.
//
// Build/run via the tests_host Makefile (g++ -std=c++17 -Wall -Wextra -Werror).
#include "color/color_engine.h"
#include "color/look_engine.h"
#include "color/plumage.h"

#include <cstdio>

using namespace canary::color;

static int g_fail = 0;
#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }   \
  } while (0)

static bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// Run a song forward to a moment where it is genuinely mid-syllable, so the
// tests below exercise a swelling lamp rather than a resting one.
static uint32_t advance_to_voice(Plumage& song, uint32_t from, bool night) {
  for (uint32_t t = from; t < from + 600000u; t += 20) {
    song.tick(t, night, /*gated=*/false);
    uint8_t pitch = 0, amp = 0;
    song.sample(t, &pitch, &amp);
    if (amp > 40) return t;
  }
  return 0;   // 0 = never spoke; callers assert on it
}

// ── 1 · The honesty invariant ──────────────────────────────────────────────

static void test_attention_is_untouched() {
  printf("plumage yields under attention...\n");
  Plumage song;
  song.begin(0xC0FFEE, 0);
  const uint32_t t = advance_to_voice(song, 0, false);
  CHECK(t != 0, "the song speaks at all within 10 minutes");

  LookParams p;
  p.scene_idx = 0;

  // Every severity at or above Warn must come back byte-for-byte as the look
  // engine's own answer, at FULL depth — the loudest the song can ever be.
  const Sev attention[] = {Sev::Warn, Sev::Alert, Sev::Tamper};
  for (Sev s : attention) {
    const Rgb honest = led_color(t, p, s, false);
    const Rgb sung = plumage_led(t, p, s, false, song, 255);
    CHECK(same(honest, sung), "led under attention is the look engine verbatim");

    Rgb hw[8], sw[8];
    wash_stops(t, p, s, false, hw, 8);
    plumage_bands(t, p, s, false, song, 255, sw, 8);
    bool identical = true;
    for (int i = 0; i < 8; i++) if (!same(hw[i], sw[i])) identical = false;
    CHECK(identical, "wash under attention is the look engine verbatim");
  }
}

static void test_safe_dark_stays_dark() {
  printf("plumage stays dark when safe_dark...\n");
  Plumage song;
  song.begin(0xBEEF, 0);
  const uint32_t t = advance_to_voice(song, 0, true);
  CHECK(t != 0, "the song speaks at all at night");

  LookParams p;
  p.night = true;

  // Darkness at night means safe. A song that glowed through it would be the
  // lamp claiming the fleet is fine.
  const Rgb led = plumage_led(t, p, Sev::Ok, /*safe_dark=*/true, song, 255);
  CHECK(led.r == 0 && led.g == 0 && led.b == 0, "beacon black under safe_dark");

  Rgb bands[10];
  plumage_bands(t, p, Sev::Ok, /*safe_dark=*/true, song, 255, bands, 10);
  bool black = true;
  for (int i = 0; i < 10; i++)
    if (bands[i].r || bands[i].g || bands[i].b) black = false;
  CHECK(black, "glass black under safe_dark");
}

static void test_scene_only_colors() {
  printf("plumage never introduces a non-scene color...\n");
  // "Signal" is a single-hue green scene. Sung or silent, the lamp on that
  // scene must never drift off that hue — this is the structural reason the
  // song samples the SAME palette instead of choosing a color of its own.
  LookParams p;
  for (uint8_t i = 0; i < kSceneCount; i++)
    if (kScenes[i].id[0] == 's' && kScenes[i].id[1] == 'i') p.scene_idx = i;
  p.gamma_on = false;
  p.warmth = 0;

  Plumage song;
  song.begin(0x5150, 0);
  const uint32_t t = advance_to_voice(song, 0, false);
  CHECK(t != 0, "the song speaks");

  const Rgb c = plumage_led(t, p, Sev::Ok, false, song, 255);
  // Signal is green: green must lead, and red must not run away with it (which
  // is what an amber/red semantic leak would look like).
  CHECK(c.g >= c.r && c.g >= c.b, "green-scene song stays green-dominant");
}

// ── 2 · It can only add light ──────────────────────────────────────────────

static void test_song_only_adds() {
  printf("plumage only adds light...\n");
  Plumage song;
  song.begin(0xA11CE, 0);
  LookParams p;

  bool ever_dimmer = false, ever_brighter = false;
  for (uint32_t t = 0; t < 400000u; t += 50) {
    song.tick(t, false, false);
    const Rgb rest = led_color(t, p, Sev::Ok, false);
    const Rgb sung = plumage_led(t, p, Sev::Ok, false, song, 200);
    if (sung.r < rest.r || sung.g < rest.g || sung.b < rest.b) ever_dimmer = true;
    if (sung.r > rest.r || sung.g > rest.g || sung.b > rest.b) ever_brighter = true;

    Rgb rw[6], sw[6];
    wash_stops(t, p, Sev::Ok, false, rw, 6);
    plumage_bands(t, p, Sev::Ok, false, song, 200, sw, 6);
    for (int i = 0; i < 6; i++)
      if (sw[i].r < rw[i].r || sw[i].g < rw[i].g || sw[i].b < rw[i].b)
        ever_dimmer = true;
  }
  CHECK(!ever_dimmer, "the song never dims the lamp below its resting glow");
  CHECK(ever_brighter, "the song does actually swell the lamp");
}

static void test_depth_zero_is_silent() {
  printf("depth 0 is the bare look engine...\n");
  Plumage song;
  song.begin(0x1234, 0);
  const uint32_t t = advance_to_voice(song, 0, false);
  CHECK(t != 0, "the song speaks");
  LookParams p;
  CHECK(same(plumage_led(t, p, Sev::Ok, false, song, 0),
             led_color(t, p, Sev::Ok, false)),
        "depth 0 leaves the beacon exactly as the look engine had it");
}

// ── 3 · Personality: reproducible, and distinct ────────────────────────────

static void test_voice_is_stable_and_sane() {
  printf("voice traits stay in the musical band...\n");
  bool in_band = true;
  for (uint32_t id = 0; id < 4096; id++) {
    const Voice v = voice_from_id(id * 2654435761u);
    if (v.chatter < 70 || v.chatter > 197) in_band = false;
    if (v.boldness < 90 || v.boldness > 217) in_band = false;
    if (v.restlessness < 40 || v.restlessness > 167) in_band = false;
  }
  CHECK(in_band, "no id yields a mute or strobing bird");

  const Voice a = voice_from_id(0xDEADBEEF);
  const Voice b = voice_from_id(0xDEADBEEF);
  CHECK(a.chatter == b.chatter && a.boldness == b.boldness &&
            a.pitch == b.pitch && a.restlessness == b.restlessness,
        "the same id always yields the same voice");
}

static void test_devices_do_not_lockstep() {
  printf("two devices do not speak in lockstep...\n");
  // Sequential ids are the realistic bad case (a bench of Canaries flashed in
  // a row); the avalanche in voice_from_id is what separates them.
  Plumage a, b;
  a.begin(1001, 0);
  b.begin(1002, 0);

  int both = 0, either = 0;
  for (uint32_t t = 0; t < 900000u; t += 100) {
    a.tick(t, false, false);
    b.tick(t, false, false);
    const bool sa = a.speaking(t), sb = b.speaking(t);
    if (sa || sb) either++;
    if (sa && sb) both++;
  }
  CHECK(either > 0, "the birds speak");
  // They may overlap by chance; they must not be the same bird.
  CHECK(both * 4 < either * 3, "overlap is incidental, not synchronized");
}

static void test_song_is_reproducible() {
  printf("the same device sings the same song...\n");
  Plumage a, b;
  a.begin(0x51ACE, 0);
  b.begin(0x51ACE, 0);
  bool identical = true;
  for (uint32_t t = 0; t < 300000u; t += 70) {
    a.tick(t, false, false);
    b.tick(t, false, false);
    uint8_t pa = 0, aa = 0, pb = 0, ab = 0;
    a.sample(t, &pa, &aa);
    b.sample(t, &pb, &ab);
    if (pa != pb || aa != ab) identical = false;
  }
  CHECK(identical, "reboot-identical: same id, same song, same instants");
}

// ── 4 · The gate holds, it does not bank ───────────────────────────────────

static void test_gate_holds_without_banking() {
  printf("a shut gate holds rather than banks...\n");
  Plumage song;
  song.begin(0xF00D, 0);

  // An hour with the gate shut — far longer than any phrase gap.
  uint32_t last_gated = 0;
  for (uint32_t t = 0; t < 3600000u; t += 100) {
    song.tick(t, false, /*gated=*/true);
    last_gated = t;
  }
  bool spoke_while_gated = false;
  for (uint32_t t = 0; t < 3600000u; t += 100)
    if (song.speaking(t)) spoke_while_gated = true;
  CHECK(!spoke_while_gated, "silent for the whole gated hour");

  // Open it. "No banked burst" is not "few phrases" — the bird is supposed to
  // resume its ordinary schedule. The invariant that actually distinguishes a
  // burst from a schedule is the SPACING: no two phrases may ever start closer
  // together than the shortest gap the composer can pick.
  uint32_t starts[8] = {0};
  int n = 0;
  bool was = false;
  for (uint32_t t = 3600000u; t < 3900000u && n < 8; t += 20) {
    song.tick(t, false, /*gated=*/false);
    const bool now = song.speaking(t);
    if (now && !was) starts[n++] = t;
    was = now;
  }
  CHECK(n >= 2, "the bird resumes its ordinary schedule after the gate opens");
  CHECK(n == 0 || starts[0] >= last_gated + Plumage::kSettleMs,
        "it settles before speaking rather than reacting to the switch");
  bool spaced = true;
  for (int i = 1; i < n; i++)
    if (starts[i] - starts[i - 1] < Plumage::kMinGapMs) spaced = false;
  CHECK(spaced, "no two phrases closer than the minimum gap — no banked burst");
}

static void test_boot_is_quiet() {
  printf("the lamp does not greet an empty hallway on power-up...\n");
  Plumage song;
  song.begin(0x2468, 0);
  bool early = false;
  for (uint32_t t = 0; t < 10000u; t += 20) {
    song.tick(t, true, false);
    if (song.speaking(t)) early = true;
  }
  CHECK(!early, "silent for the first 10 s after boot");
}

static void test_night_is_calmer_than_day() {
  printf("night stirs less than day...\n");
  // Same device, same window: night must spend materially less time speaking.
  auto voiced_ms = [](bool night) {
    Plumage s;
    s.begin(0x77AA, 0);
    uint32_t v = 0;
    for (uint32_t t = 0; t < 1800000u; t += 50) {
      s.tick(t, night, false);
      if (s.speaking(t)) v += 50;
    }
    return v;
  };
  const uint32_t day = voiced_ms(false), night = voiced_ms(true);
  CHECK(day > 0 && night > 0, "it speaks in both modes");
  CHECK(night < day, "a hallway lamp stirs at night, it does not perform");
}

// ── The head: light climbs the glass ───────────────────────────────────────

static void test_head_rises() {
  printf("the syllable climbs the pane...\n");
  Plumage song;
  song.begin(0x9001, 0);
  const uint32_t t0 = advance_to_voice(song, 0, false);
  CHECK(t0 != 0, "the song speaks");
  // Inside one syllable the head must be non-decreasing — the phrase reads as
  // light rising, never as light jittering.
  bool rising = true;
  uint8_t prev = song.head(t0);
  for (uint32_t t = t0; t < t0 + 200; t += 10) {
    const uint8_t h = song.head(t);
    if (h != 0 && h < prev) rising = false;
    if (h != 0) prev = h;
  }
  CHECK(rising, "head is monotonic within a syllable");
}

int main() {
  printf("== plumage ==\n");
  test_attention_is_untouched();
  test_safe_dark_stays_dark();
  test_scene_only_colors();
  test_song_only_adds();
  test_depth_zero_is_silent();
  test_voice_is_stable_and_sane();
  test_devices_do_not_lockstep();
  test_song_is_reproducible();
  test_gate_holds_without_banking();
  test_boot_is_quiet();
  test_night_is_calmer_than_day();
  test_head_rises();
  if (g_fail) { printf("FAILED (%d)\n", g_fail); return 1; }
  printf("all plumage tests passed\n");
  return 0;
}
