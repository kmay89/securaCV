// Host tests for common/csi/src/contact_tamper.h — the debounce + hold FSM
// between an enclosure tamper contact's raw pin level and
// tamper_events_watch_contact(). Every case is a way the device could
// narrate "enclosure opened" when nobody opened it (bounce, a knock, a
// boot with the lid off) or miss a real opening (a clock wrap).
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdio>

#include "../common/csi/src/contact_tamper.h"

using contact_tamper::Policy;
using contact_tamper::State;
using contact_tamper::Transition;
using contact_tamper::kDefaultPolicy;
using contact_tamper::kInitial;
using contact_tamper::sample;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// Hold `raw` for `n` samples spaced `step_ms` apart starting at *t; returns
// the last transition seen and how many non-NONE transitions fired.
static Transition hold(State* s, bool raw, int n, uint32_t* t, uint32_t step_ms,
                       int* fired) {
  Transition last = Transition::NONE;
  for (int i = 0; i < n; ++i) {
    const Transition tr = sample(s, raw, *t);
    if (tr != Transition::NONE) {
      last = tr;
      ++*fired;
    }
    *t += step_ms;
  }
  return last;
}

static int test_first_sample_adopts_silently() {
  // Booting closed, and booting with the lid off: both are configurations.
  State s = kInitial;
  CHECK(sample(&s, false, 1000) == Transition::NONE);
  CHECK(s.adopted && !s.open);
  State t = kInitial;
  CHECK(sample(&t, true, 1000) == Transition::NONE);
  CHECK(t.adopted && t.open);
  return 0;
}

static int test_bounce_shorter_than_debounce_is_ignored() {
  State s = kInitial;
  uint32_t now = 0;
  int fired = 0;
  sample(&s, false, now);
  // Four samples at the tamper level (one short of five), then back.
  hold(&s, true, 4, &now, 100, &fired);
  hold(&s, false, 1, &now, 100, &fired);
  CHECK(fired == 0);
  CHECK(!s.open);
  // A reed bouncing every other sample never builds a run at all.
  for (int i = 0; i < 50; ++i) {
    hold(&s, (i & 1) != 0, 1, &now, 1, &fired);
  }
  CHECK(fired == 0);
  CHECK(!s.open);
  return 0;
}

static int test_enough_samples_but_too_short_is_ignored() {
  // A fast loop can collect five samples inside a millisecond knock: the
  // hold time is what separates a knock from a lid.
  State s = kInitial;
  uint32_t now = 5000;
  int fired = 0;
  sample(&s, false, now);
  hold(&s, true, 20, &now, 10, &fired);   // 20 samples, 190 ms of hold
  CHECK(fired == 0);
  CHECK(!s.open);
  hold(&s, false, 1, &now, 10, &fired);   // released before 300 ms
  CHECK(fired == 0);
  return 0;
}

static int test_opened_after_debounce_and_hold_then_closed() {
  State s = kInitial;
  uint32_t now = 0;
  int fired = 0;
  sample(&s, false, now);
  now += 50;
  // 300 ms at 50 ms spacing: samples at +0..+300 — the 7th crosses the hold.
  Transition tr = hold(&s, true, 7, &now, 50, &fired);
  CHECK(tr == Transition::OPENED);
  CHECK(fired == 1);
  CHECK(s.open);
  // Staying open reports nothing more.
  hold(&s, true, 30, &now, 50, &fired);
  CHECK(fired == 1);
  // And the return is debounced the same way.
  tr = hold(&s, false, 7, &now, 50, &fired);
  CHECK(tr == Transition::CLOSED);
  CHECK(fired == 2);
  CHECK(!s.open);
  return 0;
}

static int test_one_sample_back_restarts_the_hold() {
  State s = kInitial;
  uint32_t now = 0;
  int fired = 0;
  sample(&s, false, now);
  hold(&s, true, 6, &now, 50, &fired);    // 250 ms into the hold
  hold(&s, false, 1, &now, 50, &fired);   // contact recovers once
  hold(&s, true, 6, &now, 50, &fired);    // a fresh run, again under 300 ms
  CHECK(fired == 0);
  hold(&s, true, 1, &now, 50, &fired);    // now 300 ms into the new run
  CHECK(fired == 1 && s.open);
  return 0;
}

static int test_millis_wrap_is_safe() {
  State s = kInitial;
  uint32_t now = 0xFFFFFF00u;   // 256 ms before the uint32_t wrap
  int fired = 0;
  sample(&s, false, now);
  now += 100;
  const Transition tr = hold(&s, true, 8, &now, 50, &fired);  // crosses 0
  CHECK(tr == Transition::OPENED);
  CHECK(fired == 1);
  return 0;
}

static int test_hold_that_ends_before_the_wrap_is_seen_after_it() {
  // The case the test above cannot tell apart from a naive
  // `now < since + hold` compare: here since + hold does NOT wrap, but a
  // slow loop (150 ms per pass, e.g. behind an SD write) only reaches the
  // fifth sample after millis() wrapped. The naive compare then reads a
  // small `now` as "hold not yet elapsed" for ~49 days; wrap-safe
  // (now - since) reads 600 ms.
  State s = kInitial;
  uint32_t now = 0xFFFFFE00u - 150u;
  int fired = 0;
  sample(&s, false, now);
  now += 150;                    // candidate starts at 0xFFFFFE00
  // Samples at 0xFFFFFE00, +150, +300, +450 (all before the wrap), then
  // +600 = 0x58 after it: the fifth sample, 600 ms into the run.
  Transition tr = hold(&s, true, 4, &now, 150, &fired);
  CHECK(tr == Transition::NONE);
  CHECK(fired == 0);
  CHECK(now < 0x100u);           // the next sample lands past the wrap
  tr = hold(&s, true, 1, &now, 150, &fired);
  CHECK(tr == Transition::OPENED);
  CHECK(fired == 1);
  CHECK(s.open);
  return 0;
}

static int test_zero_debounce_acts_as_one_sample() {
  const Policy p = {0, 0};
  State s = kInitial;
  CHECK(sample(&s, false, 0, p) == Transition::NONE);
  CHECK(sample(&s, true, 1, p) == Transition::OPENED);
  CHECK(sample(&s, false, 2, p) == Transition::CLOSED);
  return 0;
}

static int test_default_policy_is_the_documented_one() {
  CHECK(kDefaultPolicy.debounce_samples == 5);
  CHECK(kDefaultPolicy.min_hold_ms == 300);
  return 0;
}

int main() {
  if (test_first_sample_adopts_silently()) return 1;
  if (test_bounce_shorter_than_debounce_is_ignored()) return 1;
  if (test_enough_samples_but_too_short_is_ignored()) return 1;
  if (test_opened_after_debounce_and_hold_then_closed()) return 1;
  if (test_one_sample_back_restarts_the_hold()) return 1;
  if (test_millis_wrap_is_safe()) return 1;
  if (test_hold_that_ends_before_the_wrap_is_seen_after_it()) return 1;
  if (test_zero_debounce_acts_as_one_sample()) return 1;
  if (test_default_policy_is_the_documented_one()) return 1;
  std::printf("test_contact_tamper: %d checks passed\n", g_checks);
  return 0;
}
