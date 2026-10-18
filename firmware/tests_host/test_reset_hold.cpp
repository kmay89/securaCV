// Host tests for firmware/common/io/reset_hold.h — the 10 s factory-reset
// hold: each stage once per press, the optional wipe stage, and a release
// that says which stage it came from.

#include "../common/io/reset_hold.h"

#include <cstdio>
#include <cstring>

using namespace canary::io;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static void a_tap_is_silent_here() {
  ResetHold h;
  CHECK(h.step(true, 1000) == HoldEvent::None, "press edge");
  CHECK(h.step(true, 1500) == HoldEvent::None, "held half a second");
  CHECK(h.step(false, 1600) == HoldEvent::None, "released before warn: someone else's gesture");
  CHECK(!h.warning(), "no warning pending");
}

static void the_stages_fire_once_each_while_held() {
  ResetHold h;
  CHECK(h.reset_ms == 10000 && h.warn_ms == 3000 && h.wipe_ms == 0, "defaults");
  h.step(true, 0);
  CHECK(h.step(true, 2999) == HoldEvent::None, "just before warn");
  CHECK(h.step(true, 3000) == HoldEvent::Warn, "warn at 3 s");
  CHECK(h.warning(), "warning flag up");
  CHECK(h.step(true, 3001) == HoldEvent::None, "warn only once");
  CHECK(h.step(true, 9999) == HoldEvent::None, "still holding");
  CHECK(h.step(true, 10000) == HoldEvent::Reset, "reset at 10 s");
  CHECK(h.step(true, 10001) == HoldEvent::None, "reset only once");
  CHECK(h.step(true, 60000) == HoldEvent::None, "no wipe stage by default, however long");
  CHECK(h.step(false, 60001) == HoldEvent::Released, "release reports");
  CHECK(h.released_stage == 2, "from the reset stage");
  CHECK(!h.warning(), "flag down after release");
  CHECK(h.step(false, 60002) == HoldEvent::None, "idle");
}

static void a_release_during_warn_is_reported_as_warn() {
  ResetHold h;
  h.step(true, 0);
  CHECK(h.step(true, 4000) == HoldEvent::Warn, "warned");
  CHECK(h.step(false, 5000) == HoldEvent::Released, "let go");
  CHECK(h.released_stage == 1, "from warn: the board does nothing");
}

static void the_wipe_stage_is_opt_in() {
  ResetHold h;
  h.wipe_ms = 30000;
  h.step(true, 100);
  CHECK(h.step(true, 3100) == HoldEvent::Warn, "warn");
  CHECK(h.step(true, 10100) == HoldEvent::Reset, "reset");
  CHECK(h.step(true, 29999) == HoldEvent::None, "not yet");
  CHECK(h.step(true, 30100) == HoldEvent::Wipe, "wipe at 30 s");
  CHECK(h.step(true, 30101) == HoldEvent::None, "once");
  CHECK(h.step(false, 31000) == HoldEvent::Released, "release");
  CHECK(h.released_stage == 3, "from wipe");
}

static void a_second_press_starts_over() {
  ResetHold h;
  h.step(true, 0);
  h.step(true, 10000);  // Reset
  h.step(false, 10001); // Released
  h.step(true, 20000);
  CHECK(h.step(true, 22999) == HoldEvent::None, "new press, not yet warned");
  CHECK(h.step(true, 23000) == HoldEvent::Warn, "warns again on its own clock");
}

static void the_clock_may_wrap() {
  ResetHold h;
  const uint32_t t0 = 0xFFFFF000u;
  h.step(true, t0);
  CHECK(h.step(true, t0 + 3000) == HoldEvent::Warn, "warn across the wrap");
  CHECK(h.step(true, t0 + 10000) == HoldEvent::Reset, "reset across the wrap");
}

static void the_scope_words() {
  CHECK(!std::strcmp(reset_scope_label(ResetScope::CredentialsAndSettings), "credentials and settings"), "default scope");
  CHECK(!std::strcmp(reset_scope_label(ResetScope::Everything), "everything (identity too)"), "wipe scope");
}

int main() {
  a_tap_is_silent_here();
  the_stages_fire_once_each_while_held();
  a_release_during_warn_is_reported_as_warn();
  the_wipe_stage_is_opt_in();
  a_second_press_starts_over();
  the_clock_may_wrap();
  the_scope_words();
  if (g_failures) {
    std::printf("test_reset_hold: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_reset_hold: all passed\n");
  return 0;
}
