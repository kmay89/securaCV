// firmware/tests_host/test_story.cpp — host tests for the performance engine
// (firmware/common/story/) and the scripts it plays.
//
// What is defended here:
//
//   1. THE TRUTH PREEMPTS THE STORY. An interrupt drops the scene instantly
//      and it never resumes. This is the one that matters: a device that
//      finished its charming sentence over a live Tamper would be a toy.
//   2. THE USER OUTRANKS THE STORYBOARD. First tap completes the line, next
//      tap moves on, and nobody is ever trapped in a cutscene.
//   3. NIGHT IS SACRED. day_only scenes do not start during quiet hours.
//   4. THE WRITING HOLDS ITS OWN RULES — asserted, not trusted: every line is
//      plain ASCII (the Montserrat tables carry no emoji), short enough to
//      read at a glance, and the scripts contain no diagnostic vocabulary.
//      That last one is the guard rail on rule 2 of story_scripts.h; it is
//      easy to write a charming line that quietly becomes a status claim.
//
// Build/run via the tests_host Makefile (g++ -std=c++17 -Wall -Wextra -Werror).
#include "story/story.h"
#include "story/story_scripts.h"

#include <cstdio>
#include <cstring>

using namespace canary::story;

static int g_fail = 0;
#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; }   \
  } while (0)

// Run a teller forward until the scene ends or we give up.
static uint32_t run_to_end(StoryTeller& t, uint32_t from, uint32_t limit_ms) {
  for (uint32_t now = from; now < from + limit_ms; now += 20) {
    const Frame f = t.frame(now);
    if (f.scene_done) return now;
    if (!t.active()) return now;
  }
  return 0;
}

// ── 1 · The truth preempts the story ──────────────────────────────────────

static void test_interrupt_is_absolute() {
  printf("attention drops the scene, and it does not resume...\n");
  StoryTeller t;
  t.set_subject("quiet-heron-41");
  CHECK(t.play(&kHello, 0, false), "the meeting starts");

  // Get properly into the middle of it — mid-joke, the worst moment to stop.
  for (uint32_t now = 0; now < 6000; now += 20) t.frame(now);
  CHECK(t.active(), "still performing at 6 s");

  t.interrupt();
  CHECK(!t.active(), "interrupt stops it dead");
  const Frame f = t.frame(7000);
  CHECK(f.beat == nullptr, "nothing on stage after an interrupt");

  // And it must not pick up where it left off — resuming would say the alarm
  // was the interruption rather than the point.
  for (uint32_t now = 7000; now < 20000; now += 20)
    CHECK(t.frame(now).beat == nullptr, "stays off stage without a new play()");
}

static void test_no_talking_over_yourself() {
  printf("a scene never starts on top of another...\n");
  StoryTeller t;
  CHECK(t.play(&kHello, 0, false), "first scene plays");
  CHECK(!t.play(&kIdleQuiet, 100, false), "second is refused while busy");
  t.interrupt();
  CHECK(t.play(&kIdleQuiet, 200, false), "and allowed once the stage is free");
}

// ── 2 · The user outranks the storyboard ──────────────────────────────────

static void test_tap_completes_then_advances() {
  printf("first tap completes the line, next advances...\n");
  StoryTeller t;
  t.set_subject("x");
  t.play(&kHello, 0, false);

  // Walk to the first beat that actually has words.
  uint32_t now = 0;
  while (now < 30000) {
    const Frame f = t.frame(now);
    if (f.beat && f.beat->line) break;
    now += 20;
  }
  Frame f = t.frame(now + 60);
  CHECK(f.beat && f.beat->line, "found a speaking beat");
  CHECK(!f.line_done, "the line is still typing");
  const uint8_t before = f.index;

  t.tap(now + 60);
  f = t.frame(now + 61);
  CHECK(f.line_done, "the tap completed the line");
  CHECK(f.index == before, "...without skipping the beat");

  t.tap(now + 62);
  f = t.frame(now + 63);
  CHECK(f.index == before + 1 || f.scene_done, "the next tap moved on");
}

static void test_scene_always_ends() {
  printf("no cutscene is a hostage situation...\n");
  StoryTeller t;
  t.set_subject("quiet-heron-41");
  t.play(&kHello, 0, false);
  // The meeting is the longest thing written; it must still finish on its own
  // in a reasonable wall time with nobody touching it.
  const uint32_t end = run_to_end(t, 0, 120000);
  CHECK(end != 0, "the meeting ends on its own");
  CHECK(!t.active(), "and leaves the stage clear");

  // And tapping through it is fast.
  StoryTeller q;
  q.set_subject("x");
  q.play(&kHello, 0, false);
  uint32_t now = 0;
  for (int i = 0; i < 200 && q.active(); i++) {
    q.tap(now);
    now += 30;
    q.frame(now);
  }
  CHECK(!q.active(), "a determined tapper gets out in seconds");
}

static void test_unskippable_scene_ignores_taps() {
  printf("you may not tap past the truth arriving...\n");
  StoryTeller t;
  t.play(&kAlertApproach, 0, false);
  const Frame a = t.frame(10);
  t.tap(20);
  const Frame b = t.frame(30);
  CHECK(a.index == b.index, "the alert approach ignores taps");
}

// ── 3 · Night is sacred ───────────────────────────────────────────────────

static void test_night_gate() {
  printf("night is sacred...\n");
  StoryTeller t;
  CHECK(!t.play(&kIdleQuiet, 0, /*night=*/true),
        "an idle bit will not start at 3 a.m.");
  CHECK(t.play(&kIdleQuiet, 0, /*night=*/false), "but does by day");
  t.interrupt();

  // The two that MUST still run at night: meeting someone, and an alarm
  // arriving. Both are things the user is present for.
  CHECK(t.play(&kHello, 0, true), "the first meeting still happens at night");
  t.interrupt();
  CHECK(t.play(&kAlertApproach, 0, true), "and so does the alert approach");

  // Every idle vignette must be day_only — this is the rule, checked over the
  // whole pool rather than the one we happened to test.
  for (uint8_t i = 0; i < kIdlePoolCount; i++)
    CHECK(kIdlePool[i]->day_only, "every idle vignette is day_only");
}

// ── 4 · The writing holds its own rules ───────────────────────────────────

static void for_every_line(void (*fn)(const char*, const char*)) {
  const Scene* all[] = {&kHello,     &kHelloAgain, &kIdlePreen,
                        &kIdleGlance, &kIdleCheck, &kIdleQuiet,
                        &kIdleStillHere, &kIdleProud, &kAlertApproach};
  for (const Scene* s : all)
    for (uint8_t i = 0; i < s->n; i++)
      if (s->beats[i].line) fn(s->id, s->beats[i].line);
}

static void check_ascii(const char* id, const char* line) {
  for (const char* p = line; *p; p++)
    if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) {
      printf("  FAIL: non-ASCII in scene '%s': %s\n", id, line);
      g_fail++;
      return;
    }
}

static void check_short(const char* id, const char* line) {
  // Read at a glance, at three meters, on a 172 px pane. This is generous
  // already; anything longer is a paragraph pretending to be a line.
  if (strlen(line) > 60) {
    printf("  FAIL: line too long for the glass in '%s' (%zu): %s\n", id,
           strlen(line), line);
    g_fail++;
  }
}

// The guard rail on "ambient only, never diagnosis". A charming line that
// quietly becomes a status claim is the easiest mistake to make here, and the
// hardest to notice in review — so the diagnostic vocabulary is banned
// outright from the scripts. The UI owns these words.
static void check_not_diagnostic(const char* id, const char* line) {
  static const char* banned[] = {
      "error", "fail",  "offline", "disconnect", "warning", "alert",
      "alarm", "down",  "lost",    "missing",    "tamper",  "critical",
      "battery", "unreachable",
  };
  char low[256];
  size_t n = strlen(line);
  if (n >= sizeof(low)) n = sizeof(low) - 1;
  for (size_t i = 0; i < n; i++)
    low[i] = (line[i] >= 'A' && line[i] <= 'Z') ? (char)(line[i] + 32) : line[i];
  low[n] = '\0';
  for (const char* b : banned)
    if (strstr(low, b)) {
      printf("  FAIL: diagnostic word '%s' in scene '%s': %s\n", b, id, line);
      g_fail++;
      return;
    }
}

static void test_writing_rules() {
  printf("the writing holds its own rules...\n");
  for_every_line(check_ascii);
  for_every_line(check_short);
  for_every_line(check_not_diagnostic);

  // The alarm approach is wordless, on purpose: the words on an alarm belong
  // to the UI, which knows what is actually wrong.
  for (uint8_t i = 0; i < kAlertApproach.n; i++)
    CHECK(kAlertApproach.beats[i].line == nullptr,
          "the alert approach never speaks");

  // The meeting opens on presence, not speech.
  CHECK(kHello.beats[0].line == nullptr, "the meeting opens wordless");
  CHECK(kHello.beats[1].line == nullptr, "and stays wordless for a second beat");

  // Two thirds of idle draws should be wordless — company, not a chatbot.
  int wordless = 0;
  for (uint32_t r = 0; r < 1000; r++) {
    const Scene* s = story_pick_idle(r * 2654435761u, 0);
    if (s->beats[0].line == nullptr) wordless++;
  }
  CHECK(wordless > 550, "most idle moments are wordless");
}

// ── The subject substitution ──────────────────────────────────────────────

static void test_subject_substitution() {
  printf("the one name it has...\n");
  StoryTeller t;
  t.set_subject("quiet-heron-41");

  // Find the line with the %s and expand it.
  const Beat* named = nullptr;
  for (uint8_t i = 0; i < kHello.n; i++)
    if (kHello.beats[i].line && strstr(kHello.beats[i].line, "%s"))
      named = &kHello.beats[i];
  CHECK(named != nullptr, "the meeting names the device's pseudonym");

  char buf[128];
  const uint16_t n = t.expand(named, buf, sizeof(buf));
  CHECK(strstr(buf, "quiet-heron-41") != nullptr, "the pseudonym is filled in");
  CHECK(strstr(buf, "%s") == nullptr, "and the placeholder is gone");
  CHECK(n == strlen(buf), "the reported length matches what was written");

  // The typing clock must count the EXPANDED length, or the line finishes
  // typing before it is finished — which reads as a glitch, not as speech.
  StoryTeller u;
  u.set_subject("a-very-much-longer-pseudonym-than-that");
  char big[192];
  const uint16_t m = u.expand(named, big, sizeof(big));
  CHECK(m > n, "a longer subject makes a longer line");

  // An empty subject must not corrupt the sentence.
  StoryTeller v;
  v.set_subject(nullptr);
  char none[128];
  v.expand(named, none, sizeof(none));
  CHECK(strstr(none, "%s") == nullptr, "an empty subject still expands");
}

static void test_expand_never_overflows() {
  printf("expansion respects its buffer...\n");
  StoryTeller t;
  char longsub[400];
  memset(longsub, 'z', sizeof(longsub) - 1);
  longsub[sizeof(longsub) - 1] = '\0';
  t.set_subject(longsub);

  const Beat* named = nullptr;
  for (uint8_t i = 0; i < kHello.n; i++)
    if (kHello.beats[i].line && strstr(kHello.beats[i].line, "%s"))
      named = &kHello.beats[i];

  char small[16];
  memset(small, 'A', sizeof(small));
  t.expand(named, small, sizeof(small));
  CHECK(small[sizeof(small) - 1] == '\0', "the buffer is always terminated");
  CHECK(strlen(small) < sizeof(small), "and never overrun");
}

// ── Choreography sanity ───────────────────────────────────────────────────

static void test_the_joke_is_built_right() {
  printf("the joke is built right...\n");
  // The setup must be followed by a held silent beat, and the punchline must
  // land on its own beat. Comedy is timing, and here timing is data — so it
  // is checked like data.
  int setup = -1;
  for (uint8_t i = 0; i < kHello.n; i++)
    if (kHello.beats[i].line && strstr(kHello.beats[i].line, "worked out"))
      setup = i;
  CHECK(setup >= 0, "there is a setup line");
  CHECK(kHello.beats[setup].tone == Tone::Sly, "the setup is played sly");
  CHECK(kHello.beats[setup].gesture == Gesture::Narrow,
        "and the light pulls in for it");
  CHECK(kHello.beats[setup + 1].line == nullptr,
        "a silent beat holds before the punchline");
  CHECK(kHello.beats[setup + 1].hold_ms >= 600, "and it holds long enough");
  CHECK(kHello.beats[setup + 2].line != nullptr &&
            strcmp(kHello.beats[setup + 2].line, "Nothing.") == 0,
        "the punchline is one word on its own beat");

  // The scene must end warm, not clever. The last spoken beat is the one the
  // user remembers.
  const Beat& last = kHello.beats[kHello.n - 1];
  CHECK(last.tone == Tone::Warm || last.tone == Tone::Earnest,
        "the meeting ends warm, not on the gag");
}

int main() {
  printf("== story engine ==\n");
  test_interrupt_is_absolute();
  test_no_talking_over_yourself();
  test_tap_completes_then_advances();
  test_scene_always_ends();
  test_unskippable_scene_ignores_taps();
  test_night_gate();
  test_writing_rules();
  test_subject_substitution();
  test_expand_never_overflows();
  test_the_joke_is_built_right();
  if (g_fail) { printf("FAILED (%d)\n", g_fail); return 1; }
  printf("all story tests passed\n");
  return 0;
}
