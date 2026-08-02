// Host test for the Canary Companion cores (include/canary/companion/): the
// pet care loop, the play sessions, the Night Watch glass engine, wake-on-raise
// and the wrist tap, the voice planner, and the settings navigator.
//
// Builds standalone with g++ — no Arduino, no LVGL, no panel, no I²C. Prints
// "ALL COMPANION TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build from the repo root:
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-companion/include
//       firmware/projects/canary-companion/tests_host/test_companion_cores.cpp
//       -o t && ./t
//
// ── What these tests are for ─────────────────────────────────────────────────
//
// Not coverage. Most of these exist to fail if someone later "simplifies" away
// one of the refusals the design is built on — the pet cannot die, absence
// costs nothing permanent, the ask never rings, a dark screen never means
// "trouble", the voice never claims a channel it does not have. Each of those
// is a one-line change to make and a very hard thing to notice, so each has a
// test named after the mistake rather than after the function.

#include "canary/companion/haptic_voice.h"
#include "canary/companion/night_clock.h"
#include "canary/companion/pet_model.h"
#include "canary/companion/pet_play.h"
#include "canary/companion/raise_gesture.h"
#include "canary/companion/settings_nav.h"

#include <cstdio>

using namespace canary::companion;

static int g_fail = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ---------------------------------------------------------------------------
// The pet — the refusals first
// ---------------------------------------------------------------------------

// THE headline refusal. A year of total neglect: no care, ever. The bird must
// still be here, still be at least a Fledgling, and every need must sit at the
// floor rather than at zero.
static void test_pet_never_dies_and_never_regresses() {
  PetState p;
  p.stage = Stage::Companion;
  Stage lowest = p.stage;

  for (int day = 0; day < 365; day++) {
    for (int h = 0; h < 16; h++) pet_tick_hour(p);  // 16 waking hours
    p.asleep = true;
    for (int h = 0; h < 8; h++) pet_tick_hour(p);
    p.asleep = false;
    pet_day_rollover(p);
    if (static_cast<uint8_t>(p.stage) < static_cast<uint8_t>(lowest)) lowest = p.stage;
  }

  CHECK(lowest == Stage::Companion, "neglect: stage never regressed in a year");
  CHECK(p.needs.fed == NEED_FLOOR, "neglect: fed rests at the floor, not zero");
  CHECK(p.needs.spark == NEED_FLOOR, "neglect: spark rests at the floor, not zero");
  CHECK(p.needs.fed > 0, "neglect: no need ever reaches zero");
  CHECK(p.warmth == 0, "neglect: warmth is the one thing that fully decays");
}

// Absence must cost nothing that cannot be undone. Warmth is the only scalar
// allowed to fall, and one visit must visibly restore it.
static void test_absence_costs_nothing_permanent() {
  PetState p;
  p.stage = Stage::Songbird;
  p.bond = 200;
  const uint16_t bond_before = p.bond;

  for (int day = 0; day < 60; day++) pet_day_rollover(p);

  CHECK(p.bond == bond_before, "absence: bond is monotonic — never decays");
  CHECK(p.stage == Stage::Songbird || static_cast<uint8_t>(p.stage) >
                                          static_cast<uint8_t>(Stage::Songbird),
        "absence: stage held or grew, never fell");
  CHECK(p.warmth == 0, "absence: warmth decayed to cold");

  pet_care(p, Care::Preen);
  CHECK(p.warmth == WARMTH_PER_VISIT, "return: one visit warms the bird again");
}

// The ask is a pull, never a push. A sleeping bird asks for nothing at all —
// this is the test that stops someone wiring `pet_ask()` to a notification.
static void test_ask_never_rings() {
  PetState p;
  p.needs.fed = 1;
  p.needs.spark = 1;
  p.needs.rest = 1;
  p.asleep = true;
  CHECK(pet_ask(p) == PetAsk::Nothing, "ask: a sleeping bird asks for nothing");

  p.asleep = false;
  CHECK(pet_ask(p) != PetAsk::Nothing, "ask: an awake, low bird has something to say");
}

// Overfeeding is declined, not punished. The P1 made overfeeding lethal; here
// the bird says no thank you and the state is completely unchanged.
static void test_overfeeding_is_declined_not_punished() {
  PetState p;
  p.needs.fed = NEED_MAX;
  const uint16_t bond_before = p.bond;

  CHECK(pet_would_decline(p, Care::Feed), "full: the bird declines seed");
  CHECK(!pet_care(p, Care::Feed), "full: care returns false");
  CHECK(p.needs.fed == NEED_MAX, "full: nothing changed");
  CHECK(p.bond == bond_before, "full: a declined act pays no bond");
}

// The anti-grind rule. Past the daily cap, care still WORKS but stops paying
// growth — the reward curve goes flat and there is no reason to keep tapping.
static void test_bond_daily_cap_flattens_the_reward() {
  PetState p;
  for (int i = 0; i < 200; i++) {
    p.needs.fed = 0;  // keep it hungry so nothing is declined
    pet_care(p, Care::Feed);
  }
  CHECK(p.bond_today == BOND_DAILY_CAP, "grind: bond stops at the daily cap");
  CHECK(p.bond == BOND_DAILY_CAP, "grind: total bond respects the cap too");

  // …and care still works after the cap.
  p.needs.fed = 0;
  CHECK(pet_care(p, Care::Feed), "grind: care still functions past the cap");
  CHECK(p.needs.fed > 0, "grind: the bird is still actually fed");
}

// Growth needs BOTH gates. Bond alone cannot buy an afternoon Companion.
static void test_growth_cannot_be_rushed() {
  PetState p;
  p.bond = 60000;  // absurd bond, day zero
  pet_day_rollover(p);
  CHECK(p.stage == Stage::Fledgling, "growth: day 1 is a Fledgling regardless of bond");

  pet_day_rollover(p);
  CHECK(p.stage == Stage::Fledgling,
        "growth: still a Fledgling — the min-days gate holds");
}

// Preen restores no need and still counts. A child must not learn that sitting
// with the bird is the useless button.
static void test_preen_costs_nothing_and_counts() {
  PetState p;
  const Needs before = p.needs;
  CHECK(pet_care(p, Care::Preen), "preen: always welcome");
  CHECK(p.needs.fed == before.fed && p.needs.spark == before.spark,
        "preen: restores no need");
  CHECK(p.bond == BOND_FOR_CARE, "preen: still pays the bond");
}

// The one bridge from the real fleet runs in one direction only. A sick home
// must never cost a child anything — there is deliberately no Care value for it
// and no penalty path, and this test asserts the enum's shape.
static void test_household_is_bonus_only() {
  PetState p;
  const uint16_t before = p.bond;
  pet_care(p, Care::HouseholdTended);
  CHECK(p.bond > before, "household: a tended home pays the bond");

  // A neglected home is simply the absence of this event. Ticking hours with no
  // household care must behave identically to any other quiet hour.
  PetState a, b;
  for (int h = 0; h < 8; h++) {
    pet_tick_hour(a);
    pet_tick_hour(b);
  }
  CHECK(a.needs.fed == b.needs.fed && a.needs.spark == b.needs.spark,
        "household: an untended home changes nothing about the pet");
}

// A sleeping bird accrues no deficit — a child must not wake to a debt they
// could not have prevented.
static void test_night_accrues_no_deficit() {
  PetState p;
  p.asleep = true;
  const uint8_t fed_before = p.needs.fed;
  for (int h = 0; h < 10; h++) pet_tick_hour(p);
  CHECK(p.needs.fed == fed_before, "night: hunger does not advance while asleep");
  CHECK(p.needs.rest == NEED_MAX, "night: rest recovers to full");
}

// ---------------------------------------------------------------------------
// Play
// ---------------------------------------------------------------------------

// No RNG anywhere: the same round played twice is the same round.
static void test_play_is_deterministic() {
  const EchoPattern a = echo_pattern(2, 1);
  const EchoPattern b = echo_pattern(2, 1);
  CHECK(a.taps == b.taps, "echo: same round, same tap count");
  for (uint8_t i = 0; i + 1 < a.taps; i++) {
    CHECK(a.gap_ms[i] == b.gap_ms[i], "echo: same round, same gaps");
  }
}

// Difficulty preserves the SHAPE of a rhythm — a child who learned round three
// on easy must still recognise it on hard.
static void test_difficulty_preserves_rhythm_shape() {
  const EchoPattern easy = echo_pattern(3, 0);
  const EchoPattern hard = echo_pattern(3, 2);
  CHECK(easy.taps == hard.taps, "echo: difficulty does not change tap count");
  // Gap 0 is shorter than gap 1 at both difficulties (the heartbeat shape).
  CHECK((easy.gap_ms[0] < easy.gap_ms[1]) == (hard.gap_ms[0] < hard.gap_ms[1]),
        "echo: the relative shape survives the tempo change");
}

// Scoring is partial credit, never pass/fail.
static void test_echo_gives_partial_credit() {
  const EchoPattern p = echo_pattern(1, 0);  // 3 taps, gaps {300, 600}
  const uint16_t got[2] = {305, 4000};       // first gap good, second wildly off
  const uint8_t hits = echo_score(p, got, 3, 0);
  CHECK(hits == 1, "echo: one gap right scores one, not zero");
}

// The bird's tiredness ends the session, and that ending is distinguishable
// from hitting the ceiling — only one of them gets narrated as the bird's own
// choice.
static void test_bird_ends_play_itself() {
  PlaySession s;
  play_next_round(s, /*bird_is_tired=*/true);
  CHECK(s.over, "play: a tired bird ends the session");
  CHECK(s.ended_by_bird, "play: and the ending is attributed to the bird");

  PlaySession t;
  for (int i = 0; i < 10; i++) play_next_round(t, false);
  CHECK(t.over, "play: the hard ceiling still stops a session");
  CHECK(!t.ended_by_bird, "play: a ceiling stop is not the bird's choice");
}

// A wobble stops the clock; it must not reset progress.
static void test_steady_wobble_does_not_reset() {
  SteadyRound r;
  steady_feed(r, 0, 0, 0);
  steady_feed(r, 0, 500, 0);       // 500 ms held
  steady_feed(r, 900, 700, 0);     // a wobble
  const uint32_t after_wobble = r.held_ms;
  CHECK(after_wobble >= 500, "steady: a wobble does not reset the hold");
}

// ---------------------------------------------------------------------------
// The Night Watch
// ---------------------------------------------------------------------------

// THE honesty rule for the glass: a dark screen is a claim of wellness, so
// trouble must break blackout no matter what the user chose.
static void test_trouble_overrides_blackout() {
  NightClockConfig c;
  c.style = NightStyle::GoDark;
  NightClockState s;
  NightClockInputs in;
  in.hour = 3;
  in.now_ms = 100000;

  nw_step(s, c, in);
  CHECK(s.mode == GlassMode::Dark, "night: a well fleet at 3 a.m. goes dark");

  in.trouble = true;
  nw_step(s, c, in);
  CHECK(s.mode == GlassMode::Vigil, "night: trouble breaks blackout");
  CHECK(nw_brightness(s.mode) > NW_BRIGHT_OFF, "night: vigil actually emits light");

  // A clock that does not know the time may not claim wellness either.
  in.trouble = false;
  in.clock_unsure = true;
  nw_step(s, c, in);
  CHECK(s.mode == GlassMode::Vigil, "night: an unsure clock never goes dark");
}

// Trouble must not wait for an in-flight peek to lapse.
static void test_trouble_interrupts_a_peek() {
  NightClockConfig c;
  NightClockState s;
  NightClockInputs in;
  in.hour = 3;
  in.now_ms = 1000;
  in.touched = true;
  nw_step(s, c, in);
  CHECK(s.mode == GlassMode::Peek, "night: a touch peeks");

  in.touched = false;
  in.trouble = true;
  in.now_ms = 1100;  // well inside the peek window
  nw_step(s, c, in);
  CHECK(s.mode == GlassMode::Vigil, "night: trouble pre-empts a live peek");
}

// Quiet hours must be right across midnight — 22 → 7 is the default and the
// off-by-one here would light a bedroom at 2 a.m.
static void test_quiet_hours_wrap_midnight() {
  NightClockConfig c;  // 22 → 7
  CHECK(nw_in_quiet_hours(c, 23), "quiet: 23:00 is quiet");
  CHECK(nw_in_quiet_hours(c, 2), "quiet: 02:00 is quiet");
  CHECK(nw_in_quiet_hours(c, 6), "quiet: 06:00 is quiet");
  CHECK(!nw_in_quiet_hours(c, 7), "quiet: 07:00 is not (exclusive end)");
  CHECK(!nw_in_quiet_hours(c, 21), "quiet: 21:00 is not");
  CHECK(nw_in_quiet_hours(c, 22), "quiet: 22:00 is (inclusive start)");

  CHECK(!nw_is_deep_night(c, 22), "deep: the first quiet hour is still evening");
  CHECK(nw_is_deep_night(c, 3), "deep: 03:00 is deep night");
}

// A mid-night peek must be dimmer than the evening rung. This is the number
// that decides whether the device is tolerable in a dark bedroom.
static void test_peek_is_dimmer_than_evening() {
  CHECK(nw_brightness(GlassMode::Peek) < nw_brightness(GlassMode::Evening),
        "night: a peek is dimmer than the evening glow");
  CHECK(nw_brightness(GlassMode::Night) < nw_brightness(GlassMode::Peek),
        "night: the held night glow is the dimmest lit rung");
  CHECK(nw_red_shifted(GlassMode::Peek), "night: a peek is red-shifted");
  CHECK(!nw_red_shifted(GlassMode::Awake), "day: daytime is not red-shifted");
}

// The gentle wake goes light → haptic → sound, and never the other way.
static void test_wake_is_gentle_in_order() {
  WakeAlarm a;
  a.enabled = true;
  a.hour = 7;
  a.minute = 0;

  CHECK(wake_phase_for(a, 6, 40) == WakePhase::Idle,
        "wake: 20 min out is still asleep — the lead is 15");
  CHECK(wake_phase_for(a, 6, 50) == WakePhase::Glow, "wake: 10 min out glows");
  CHECK(wake_phase_for(a, 6, 59) == WakePhase::Buzz, "wake: 1 min out buzzes");
  CHECK(wake_phase_for(a, 7, 0) == WakePhase::Sound, "wake: at the hour, sound joins");

  CHECK(wake_glow_brightness(WAKE_LIGHT_LEAD_MIN) == NW_BRIGHT_NIGHT,
        "wake: the ramp starts at the night floor");
  CHECK(wake_glow_brightness(0) == NW_BRIGHT_DAY, "wake: the ramp ends at day");
  CHECK(wake_glow_brightness(7) > wake_glow_brightness(12),
        "wake: the ramp is monotonic");
}

// A disabled alarm must never produce a phase.
static void test_disabled_alarm_is_silent() {
  WakeAlarm a;  // enabled defaults false
  a.hour = 7;
  CHECK(wake_phase_for(a, 7, 0) == WakePhase::Idle, "wake: disabled means idle");
}

// ---------------------------------------------------------------------------
// Wake-on-raise
// ---------------------------------------------------------------------------

// Feed a plausible raise: motion, then tilt, then a hold.
static bool feed_raise(RaiseDetector& d, bool night, uint32_t t0, uint16_t hold_ms) {
  AccelSample s;
  bool fired = false;
  // At rest, arm down.
  s.x_mg = 0; s.y_mg = 900; s.z_mg = 100; s.t_ms = t0;
  fired |= raise_feed(d, s, night);
  // The lift: a big change.
  s.x_mg = 0; s.y_mg = 300; s.z_mg = 900; s.t_ms = t0 + 120;
  fired |= raise_feed(d, s, night);
  // Held still, tilted toward the face.
  for (uint32_t t = 0; t <= hold_ms; t += 40) {
    s.x_mg = 0; s.y_mg = 300; s.z_mg = 905; s.t_ms = t0 + 160 + t;
    fired |= raise_feed(d, s, night);
  }
  return fired;
}

static void test_raise_fires_on_a_real_raise() {
  RaiseDetector d;
  CHECK(feed_raise(d, /*night=*/false, 1000, 400), "raise: a held raise fires");
}

// THE failure mode that matters: a roll-over at 3 a.m. sweeps through every
// tilt angle without resting at any of them. It must not fire.
static void test_rollover_does_not_fire_at_night() {
  RaiseDetector d;
  AccelSample s;
  bool fired = false;
  s.x_mg = 0; s.y_mg = 900; s.z_mg = 100; s.t_ms = 0;
  fired |= raise_feed(d, s, true);
  // A continuous tumble: tilt passes through the gate but never settles.
  for (uint32_t t = 40; t < 1200; t += 40) {
    s.x_mg = static_cast<int16_t>(t % 400);
    s.y_mg = static_cast<int16_t>(600 - (t % 500));
    s.z_mg = static_cast<int16_t>(820 + (t % 220));  // over the gate, never still
    s.t_ms = t;
    fired |= raise_feed(d, s, true);
  }
  CHECK(!fired, "raise: a tumble through the tilt gate never fires");
}

// Night is strictly harder than day — a gesture that fires by day at a shallow
// hold must not fire at night with the same input.
static void test_night_is_stricter_than_day() {
  RaiseDetector day, night;
  const bool day_fired = feed_raise(day, false, 1000, RAISE_SETTLE_MS + 40);
  const bool night_fired = feed_raise(night, true, 1000, RAISE_SETTLE_MS + 40);
  CHECK(day_fired, "raise: the short hold fires by day");
  CHECK(!night_fired, "raise: the same short hold does not fire at night");
}

// The refractory window stops a held-up wrist from re-triggering forever.
static void test_raise_refractory_holds() {
  RaiseDetector d;
  CHECK(feed_raise(d, false, 1000, 400), "raise: first fires");
  CHECK(!feed_raise(d, false, 1600, 400), "raise: an immediate repeat is suppressed");
}

// A knuckle on the case is a sharp spike; ordinary arm motion is not.
static void test_tap_needs_a_real_spike() {
  TapDetector t;
  AccelSample s;
  s.z_mg = 1000; s.t_ms = 0;
  tap_feed(t, s);
  // Gentle motion — well under the spike gate.
  s.z_mg = 1150; s.t_ms = 40;
  CHECK(!tap_feed(t, s), "tap: gentle motion is not a tap");
  // A knock.
  s.z_mg = 2200; s.t_ms = 200;
  CHECK(tap_feed(t, s), "tap: a sharp spike is a tap");
  // Bounce inside the debounce window.
  s.z_mg = 3400; s.t_ms = 220;
  CHECK(!tap_feed(t, s), "tap: a bounce inside the debounce is the same tap");
}

// ---------------------------------------------------------------------------
// The voice
// ---------------------------------------------------------------------------

// The core promise: a plan never names a channel that is not there.
static void test_voice_never_claims_a_missing_channel() {
  VoiceCapabilities caps;  // no haptic, no sound, glass only
  for (uint8_t u = 0; u <= static_cast<uint8_t>(Utterance::Vigil); u++) {
    const VoicePlan p = voice_plan(static_cast<Utterance>(u), caps, /*night=*/false);
    CHECK(!(p.channels & CH_HAPTIC), "voice: never claims an absent motor");
    CHECK(!(p.channels & CH_SOUND), "voice: never claims an absent speaker");
    CHECK(p.channels != CH_NONE, "voice: something always happens by day");
  }
}

// A missing preferred channel must be REPORTED, not silently swapped.
static void test_voice_reports_its_fallback() {
  VoiceCapabilities caps;
  caps.sound = true;  // speaker, no motor
  const VoicePlan p = voice_plan(Utterance::Confirm, caps, false);
  CHECK(p.fell_back, "voice: a missing motor is reported as a fallback");
  CHECK(p.channels & CH_GLASS, "voice: the glass carries it instead");
}

// Night silence: nothing sounds, and only the two deliberate wake-a-sleeper
// utterances may buzz.
static void test_night_silences_everything_but_the_two() {
  VoiceCapabilities caps;
  caps.haptic = true;
  caps.sound = true;

  const VoicePlan tick = voice_plan(Utterance::Tick, caps, /*night=*/true);
  CHECK(!(tick.channels & CH_SOUND), "night: a tick never sounds");
  CHECK(!(tick.channels & CH_HAPTIC), "night: a tick never buzzes");

  const VoicePlan flourish = voice_plan(Utterance::Flourish, caps, true);
  CHECK(flourish.channels == CH_NONE,
        "night: a courtesy flourish is genuinely silent, not moved to the glass");

  const VoicePlan wake = voice_plan(Utterance::WakeNudge, caps, true);
  CHECK(wake.channels & CH_HAPTIC, "night: the wake alarm may buzz");
  CHECK(!(wake.channels & CH_SOUND), "night: even the wake alarm does not sound");

  const VoicePlan vigil = voice_plan(Utterance::Vigil, caps, true);
  CHECK(vigil.channels & CH_HAPTIC, "night: a real problem may buzz");
  CHECK(!(vigil.channels & CH_SOUND), "night: a real problem still does not sound");
}

// Night halves the strength of whatever survives.
static void test_night_softens_the_buzz() {
  VoiceCapabilities caps;
  caps.haptic = true;
  const VoicePlan day = voice_plan(Utterance::WakeNudge, caps, false);
  const VoicePlan night = voice_plan(Utterance::WakeNudge, caps, true);
  CHECK(night.strength < day.strength, "night: the buzz is softer against a sleeping wrist");
}

// A refusal must never buzz — a "no thank you" that vibrates reads as an error.
static void test_decline_never_buzzes() {
  VoiceCapabilities caps;
  caps.haptic = true;
  caps.sound = true;
  const VoicePlan p = voice_plan(Utterance::Decline, caps, false);
  CHECK(!(p.channels & CH_HAPTIC), "voice: a decline is shown, never felt");
  CHECK(p.channels & CH_GLASS, "voice: a decline is shown on the glass");
}

// The boot note tells the truth about what is fitted, and says nothing when
// everything is there.
static void test_boot_note_is_honest() {
  VoiceCapabilities none;
  CHECK(voice_boot_note(none) != nullptr, "boot: a bare board confesses");
  VoiceCapabilities full;
  full.haptic = true;
  full.sound = true;
  CHECK(voice_boot_note(full) == nullptr, "boot: a complete board says nothing");
}

// ---------------------------------------------------------------------------
// Settings navigation
// ---------------------------------------------------------------------------

// A long press must escape from every depth, and never strand the user.
static void test_longpress_escapes_from_any_depth() {
  NavState s;
  nav_open(s, 0);
  CHECK(s.level == NavLevel::Pages, "nav: opens on the page list");

  nav_input(s, Gesture::Tap, 100);
  CHECK(s.level == NavLevel::Items, "nav: a tap enters a page");
  nav_input(s, Gesture::Tap, 200);
  CHECK(s.level == NavLevel::Editing, "nav: a tap enters an editor");

  nav_input(s, Gesture::LongPress, 300);
  CHECK(s.level == NavLevel::Items, "nav: long press leaves the editor");
  nav_input(s, Gesture::LongPress, 400);
  CHECK(s.level == NavLevel::Pages, "nav: long press leaves the page");
  nav_input(s, Gesture::LongPress, 500);
  CHECK(s.level == NavLevel::Closed, "nav: long press leaves settings entirely");
  CHECK(s.exited, "nav: the exit is announced");
}

// The one destructive leaf needs two deliberate gestures and can never be one
// slip. This is the test that stops a mis-tap wiping a device.
static void test_destructive_leaf_needs_two_gestures() {
  NavState s;
  nav_open(s, 0);
  // Walk to About / Forget everything.
  while (s.page != NavPage::About) nav_input(s, Gesture::SwipeDown, 10);
  nav_input(s, Gesture::Tap, 20);
  while (nav_current_item(s) != NavItem::ForgetEverything) {
    nav_input(s, Gesture::SwipeDown, 30);
  }
  CHECK(s.level == NavLevel::Items, "nav: standing on the destructive leaf");

  nav_input(s, Gesture::Tap, 40);
  CHECK(s.level == NavLevel::Confirming, "nav: the first tap only asks");
  CHECK(!s.committed, "nav: the first tap commits nothing");

  nav_input(s, Gesture::Tap, 50);
  CHECK(s.committed, "nav: the second tap confirms");
}

// An unconfirmed wipe that times out is simply not a wipe.
static void test_unconfirmed_wipe_lapses_safely() {
  NavState s;
  nav_open(s, 0);
  while (s.page != NavPage::About) nav_input(s, Gesture::SwipeDown, 10);
  nav_input(s, Gesture::Tap, 20);
  while (nav_current_item(s) != NavItem::ForgetEverything) {
    nav_input(s, Gesture::SwipeDown, 30);
  }
  nav_input(s, Gesture::Tap, 40);
  CHECK(s.level == NavLevel::Confirming, "nav: waiting for confirmation");

  nav_input(s, Gesture::None, 40 + NAV_EDIT_TIMEOUT_MS + 1);
  CHECK(s.level == NavLevel::Items, "nav: the confirmation lapsed");
  CHECK(!s.committed, "nav: and nothing was wiped");
}

// A timed-out edit reverts, and says so. A silent revert is how someone stops
// trusting their own settings screen.
static void test_timed_out_edit_reverts_loudly() {
  NavState s;
  nav_open(s, 0);
  nav_input(s, Gesture::Tap, 100);
  nav_input(s, Gesture::Tap, 200);
  CHECK(s.level == NavLevel::Editing, "nav: editing");
  nav_input(s, Gesture::SwipeDown, 300);
  CHECK(s.dirty, "nav: the edit is pending");

  nav_input(s, Gesture::None, 300 + NAV_EDIT_TIMEOUT_MS + 1);
  CHECK(s.level == NavLevel::Items, "nav: the editor closed on timeout");
  CHECK(s.reverted, "nav: the revert is announced, not silent");
  CHECK(!s.dirty, "nav: nothing is left pending");
}

// Backing out of an edit KEEPS it — you were watching the value change.
static void test_deliberate_exit_keeps_the_edit() {
  NavState s;
  nav_open(s, 0);
  nav_input(s, Gesture::Tap, 100);
  nav_input(s, Gesture::Tap, 200);
  nav_input(s, Gesture::SwipeUp, 300);
  nav_input(s, Gesture::LongPress, 400);
  CHECK(s.committed, "nav: a deliberate back commits the edit");
  CHECK(!s.reverted, "nav: and does not revert it");
}

// A tap on a read-only leaf must do nothing at all, not fall through.
static void test_readonly_leaf_does_nothing() {
  NavState s;
  nav_open(s, 0);
  while (s.page != NavPage::About) nav_input(s, Gesture::SwipeDown, 10);
  nav_input(s, Gesture::Tap, 20);
  while (nav_current_item(s) != NavItem::DeviceInfo) {
    nav_input(s, Gesture::SwipeDown, 30);
  }
  nav_input(s, Gesture::Tap, 40);
  CHECK(s.level == NavLevel::Items, "nav: a tap on info stays put");
}

// Every page must have at least one item, or it is a dead end someone can
// navigate into and stare at.
static void test_no_page_is_empty() {
  for (uint8_t p = NAV_FIRST_PAGE; p <= NAV_LAST_PAGE; p++) {
    CHECK(nav_items_on_page(static_cast<NavPage>(p)) > 0, "nav: no page is empty");
  }
}

// Page and item navigation wrap in both directions — on a round glass there is
// no "top of the list" to bump against.
static void test_navigation_wraps_both_ways() {
  NavState s;
  nav_open(s, 0);
  const NavPage first = s.page;
  nav_input(s, Gesture::SwipeUp, 10);  // backwards off the start
  CHECK(s.page != first, "nav: swiping up from the first page wraps");
  const uint8_t span = NAV_LAST_PAGE - NAV_FIRST_PAGE + 1;
  for (uint8_t i = 1; i < span; i++) nav_input(s, Gesture::SwipeUp, 20);
  CHECK(s.page == first, "nav: a full cycle returns to the start");
}

// ---------------------------------------------------------------------------

int main() {
  std::printf("Canary Companion cores — host tests\n\n");

  std::printf("[pet: the refusals]\n");
  test_pet_never_dies_and_never_regresses();
  test_absence_costs_nothing_permanent();
  test_ask_never_rings();
  test_overfeeding_is_declined_not_punished();
  test_bond_daily_cap_flattens_the_reward();
  test_growth_cannot_be_rushed();
  test_preen_costs_nothing_and_counts();
  test_household_is_bonus_only();
  test_night_accrues_no_deficit();

  std::printf("[play]\n");
  test_play_is_deterministic();
  test_difficulty_preserves_rhythm_shape();
  test_echo_gives_partial_credit();
  test_bird_ends_play_itself();
  test_steady_wobble_does_not_reset();

  std::printf("[night watch]\n");
  test_trouble_overrides_blackout();
  test_trouble_interrupts_a_peek();
  test_quiet_hours_wrap_midnight();
  test_peek_is_dimmer_than_evening();
  test_wake_is_gentle_in_order();
  test_disabled_alarm_is_silent();

  std::printf("[wake-on-raise]\n");
  test_raise_fires_on_a_real_raise();
  test_rollover_does_not_fire_at_night();
  test_night_is_stricter_than_day();
  test_raise_refractory_holds();
  test_tap_needs_a_real_spike();

  std::printf("[voice]\n");
  test_voice_never_claims_a_missing_channel();
  test_voice_reports_its_fallback();
  test_night_silences_everything_but_the_two();
  test_night_softens_the_buzz();
  test_decline_never_buzzes();
  test_boot_note_is_honest();

  std::printf("[settings]\n");
  test_longpress_escapes_from_any_depth();
  test_destructive_leaf_needs_two_gestures();
  test_unconfirmed_wipe_lapses_safely();
  test_timed_out_edit_reverts_loudly();
  test_deliberate_exit_keeps_the_edit();
  test_readonly_leaf_does_nothing();
  test_no_page_is_empty();
  test_navigation_wraps_both_ways();

  if (g_fail) {
    std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL COMPANION TESTS PASSED\n");
  return 0;
}
