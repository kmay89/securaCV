// test_greeting.cpp — the first-meet story (canary/care/greeting.h).
//
// Pins the arc AND the three rules that outrank it: first meet only, never
// over trouble (including trouble that arrives mid-story), and no line that
// words an alarm. The copy itself is pinned too — this is the one place the
// device gets to introduce itself, and a silent edit to that wording should
// have to walk past a test.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "canary/care/greeting.h"

using canary::care::GreetBeat;
using canary::care::Greeting;
using canary::care::SelfIntro;

static SelfIntro sample() {
  SelfIntro me;
  me.mac = "30:AE:A4:1B:2C:3D";
  me.ip = "192.168.1.44";
  me.nickname = "Pip";
  me.fleet_size = 6;
  return me;
}

static std::string line_of(GreetBeat b, const SelfIntro& me) {
  char buf[128];
  return std::string(Greeting::line(b, me, buf, sizeof buf));
}

// Walk the whole arc, beat by beat, at the exact dwell boundaries.
static void test_the_arc_runs_in_order() {
  Greeting g;
  uint32_t t = 1000;
  assert(g.begin(t, /*met_before=*/false, /*calm=*/true));
  assert(g.beat() == GreetBeat::Hello);
  assert(g.running());

  // A tick before the dwell expires changes nothing.
  assert(g.step(t + Greeting::HELLO_MS - 1, true) == GreetBeat::Hello);
  t += Greeting::HELLO_MS;
  assert(g.step(t, true) == GreetBeat::Introduce);
  t += Greeting::INTRODUCE_MS;
  assert(g.step(t, true) == GreetBeat::Nickname);
  t += Greeting::NICKNAME_MS;
  assert(g.step(t, true) == GreetBeat::Purpose);
  t += Greeting::PURPOSE_MS;
  assert(g.step(t, true) == GreetBeat::Fleet);
  t += Greeting::FLEET_MS;
  assert(g.step(t, true) == GreetBeat::Settle);
  t += Greeting::SETTLE_MS;
  assert(g.step(t, true) == GreetBeat::Done);
  assert(!g.running());
  printf("  ok: the arc runs Hello -> Introduce -> Nickname -> Purpose -> Fleet -> Settle -> Done\n");
}

// Rule 1: a device you've lived with does not re-introduce itself.
static void test_only_on_the_first_meeting() {
  Greeting g;
  assert(!g.begin(1000, /*met_before=*/true, /*calm=*/true));
  assert(g.beat() == GreetBeat::Done);
  assert(!g.running());
  printf("  ok: a device you already know never re-introduces itself\n");
}

// Rule 2, at the start: trouble means the story never begins.
static void test_never_starts_over_trouble() {
  Greeting g;
  assert(!g.begin(1000, /*met_before=*/false, /*calm=*/false));
  assert(g.beat() == GreetBeat::Done);
  printf("  ok: an unquiet fleet never gets an introduction\n");
}

// Rule 2, mid-story: trouble ABORTS. This is the one that matters — charm
// during a real alarm is the thing the character system has always refused.
static void test_trouble_arriving_midway_aborts_the_story() {
  Greeting g;
  uint32_t t = 1000;
  assert(g.begin(t, false, true));
  t += Greeting::HELLO_MS;
  assert(g.step(t, true) == GreetBeat::Introduce);

  // Something goes wrong while it is mid-sentence.
  assert(g.step(t + 10, /*calm=*/false) == GreetBeat::Done);
  assert(!g.running());
  // And it does not resume when things calm down again.
  assert(g.step(t + 5000, /*calm=*/true) == GreetBeat::Done);
  printf("  ok: trouble mid-story aborts it, and it never resumes\n");
}

static void test_it_can_be_skipped() {
  Greeting g;
  assert(g.begin(1000, false, true));
  g.skip();
  assert(!g.running());
  assert(g.step(9999, true) == GreetBeat::Done);
  printf("  ok: an introduction you can't skip would be a cutscene\n");
}

// The copy. Pinned, including the degraded forms.
static void test_the_lines_say_what_they_should() {
  const SelfIntro me = sample();
  assert(line_of(GreetBeat::Hello, me) == "Hello.");
  assert(line_of(GreetBeat::Introduce, me) == "I am 30:AE:A4:1B:2C:3D, at 192.168.1.44.");
  assert(line_of(GreetBeat::Nickname, me) == "That's a lot. Call me Pip.");
  assert(line_of(GreetBeat::Purpose, me) == "I watch. I never record.");
  assert(line_of(GreetBeat::Fleet, me) == "There are 6 of us. We keep watch together.");
  // Hand-off beats carry no copy.
  assert(line_of(GreetBeat::Settle, me).empty());
  assert(line_of(GreetBeat::Done, me).empty());
  printf("  ok: the introduction reads as written\n");
}

static void test_missing_facts_degrade_instead_of_printing_holes() {
  SelfIntro bare;                     // no mac, no ip, no nickname, alone
  assert(line_of(GreetBeat::Introduce, bare) == "I am one of the quiet ones.");
  assert(line_of(GreetBeat::Nickname, bare) == "That's a lot. Just call me your Canary.");
  assert(line_of(GreetBeat::Fleet, bare) == "It's just me so far. I'll keep watch.");

  SelfIntro mac_only;
  mac_only.mac = "30:AE:A4:1B:2C:3D";
  assert(line_of(GreetBeat::Introduce, mac_only) == "I am 30:AE:A4:1B:2C:3D.");

  SelfIntro solo = sample();
  solo.fleet_size = 1;
  assert(line_of(GreetBeat::Fleet, solo) == "It's just me so far. I'll keep watch.");
  printf("  ok: a missing fact degrades the line instead of printing a hole\n");
}

// Rule 3 + the ASCII rule, checked mechanically over every beat and both
// the full and degraded fact sets: no severity vocabulary, no glyphs the
// built-in font tables can't draw.
static void test_no_line_ever_words_trouble_or_leaves_ascii() {
    const char* forbidden[] = {
        "alert", "Alert", "alarm", "Alarm", "tamper", "Tamper", "warn", "Warn",
        "offline", "Offline", "fail", "Fail", "error", "Error", "lost", "Lost",
        "danger", "Danger", "intrud", "Intrud", "breach", "Breach",
    };
    const SelfIntro sets[] = {sample(), SelfIntro{}};
    const GreetBeat beats[] = {GreetBeat::Hello,   GreetBeat::Introduce,
                               GreetBeat::Nickname, GreetBeat::Purpose,
                               GreetBeat::Fleet,   GreetBeat::Settle};
    for (const auto& me : sets) {
        for (GreetBeat b : beats) {
            const std::string s = line_of(b, me);
            for (const char* bad : forbidden) {
                assert(s.find(bad) == std::string::npos &&
                       "an introduction may never word trouble (Voice rule)");
            }
            for (unsigned char c : s) {
                assert(c >= 0x20 && c < 0x7F &&
                       "plain ASCII only — Montserrat carries no emoji");
            }
        }
    }
    printf("  ok: no beat words trouble, and every line stays plain ASCII\n");
}

// A caller sizing a timeout should get the real total.
static void test_total_matches_the_sum_of_the_beats() {
  static_assert(Greeting::total_ms() ==
                    Greeting::HELLO_MS + Greeting::INTRODUCE_MS +
                        Greeting::NICKNAME_MS + Greeting::PURPOSE_MS +
                        Greeting::FLEET_MS + Greeting::SETTLE_MS,
                "total_ms must stay the sum of the dwells");
  assert(Greeting::total_ms() < 20000 && "the whole introduction stays under twenty seconds");
  printf("  ok: the story is %u ms end to end\n", (unsigned)Greeting::total_ms());
}

// A tiny buffer must truncate, never overrun.
static void test_a_small_buffer_truncates_safely() {
  const SelfIntro me = sample();
  char tiny[8];
  const char* out = Greeting::line(GreetBeat::Introduce, me, tiny, sizeof tiny);
  assert(strlen(out) < sizeof tiny);
  printf("  ok: a small buffer truncates instead of overrunning\n");
}

int main() {
  printf("greeting (the first-meet story)\n");
  test_the_arc_runs_in_order();
  test_only_on_the_first_meeting();
  test_never_starts_over_trouble();
  test_trouble_arriving_midway_aborts_the_story();
  test_it_can_be_skipped();
  test_the_lines_say_what_they_should();
  test_missing_facts_degrade_instead_of_printing_holes();
  test_no_line_ever_words_trouble_or_leaves_ascii();
  test_total_matches_the_sum_of_the_beats();
  test_a_small_buffer_truncates_safely();
  printf("all greeting tests passed\n");
  return 0;
}
