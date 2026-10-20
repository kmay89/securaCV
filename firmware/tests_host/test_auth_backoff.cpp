// Host tests for firmware/common/network/auth_backoff.h — the bearer lockout,
// one backoff per source address (sweep F343).
//
// The rows that matter most:
//   * the schedule: one source's strikes, each sent once the backoff before
//     it ended, arm 2, 4, 8, 16, 32 and then 60 s, and never more; the sixth
//     leaves exactly 60000 ms, where the old one-backoff-per-device lockout
//     armed 64000 ms (and announced Retry-After 65) for a hold its own 60 s
//     window ended first; a strike 61 s after the last arms 2 s again;
//   * the 49.7-day wrap: a backoff armed 1024 ms before millis() wraps is
//     held 500 ms later and free 2500 ms later (the old `now < until` read
//     it as free at once);
//   * the window clears a count without anyone asking locked() first: two
//     strikes 61 s apart arm 2 s, not 4 s;
//   * the count saturates: 300 strikes leave 10 and the source held (the old
//     uint8_t count wrapped to a clean 0 after 256);
//   * the overflow: eight sources fill the Slots, a ninth strikes the shared
//     overflow, and a tenth, untracked, is held while it runs and free once
//     it ends; a tracked source is judged by its own Slot only; a settled
//     Slot is reused before the overflow; a success from an untracked
//     source leaves the overflow running;
//   * grant(): no credential is NONE and records nothing; a held source is
//     HELD and its credential is never compared; the right one is OK and
//     frees the Slot; a wrong one is WRONG and exactly one strike.

#include "../common/network/auth_backoff.h"

#include <cstdio>

using namespace canary::net::auth_backoff;

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

// Host-order IPv4s, as the WAP's auth_request_source() hands them in.
static constexpr uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d) {
  return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}
static const uint32_t kPhone = ip4(192, 168, 1, 20);
static const uint32_t kLaptop = ip4(192, 168, 1, 21);

// The seconds a 429 announces for `ms` left (api_auth_send_lockout's rule).
static unsigned long retry_after_s(uint32_t ms) { return (unsigned long)(ms / 1000 + 1); }

static void the_schedule_doubles_to_the_window_and_stops() {
  Table t{};
  const uint32_t want[] = {2000, 4000, 8000, 16000, 32000, 60000};
  uint32_t now = 5000;
  for (int i = 0; i < 6; ++i) {
    CHECK(!locked(t, kPhone, now), "strike %d is sent once the backoff before it ended", i + 1);
    const uint32_t armed = record_failure(t, kPhone, now);
    CHECK(armed == want[i], "strike %d armed %u ms, want %u", i + 1, (unsigned)armed, (unsigned)want[i]);
    CHECK(remaining_ms(t, kPhone, now) == want[i], "strike %d leaves %u ms, want %u", i + 1,
          (unsigned)remaining_ms(t, kPhone, now), (unsigned)want[i]);
    CHECK(failures_of(t, kPhone, now) == i + 1, "strike %d counted", i + 1);
    CHECK(locked(t, kPhone, now + want[i] - 1), "strike %d holds to its last ms", i + 1);
    if (i < 5) now += want[i];
  }
  // The sixth: 60 s, and the 429 says 61, never the old 65.
  CHECK(retry_after_s(remaining_ms(t, kPhone, now)) == 61, "the sixth strike announces %lu s",
        retry_after_s(remaining_ms(t, kPhone, now)));
  CHECK(!locked(t, kPhone, now + MAX_MS), "the sixth strike's hold ends at 60 s");
  // A strike right at 60 s is still inside the window (its count stands):
  // capped at 60 s again, and so is every strike after it, to the saturation.
  uint32_t at = now + MAX_MS;
  for (int i = 7; i <= 12; ++i) {
    const uint32_t armed = record_failure(t, kPhone, at);
    CHECK(armed == MAX_MS, "strike %d armed %u ms, more than the cap", i, (unsigned)armed);
    CHECK(retry_after_s(remaining_ms(t, kPhone, at)) <= 61, "strike %d announces more than 61 s", i);
    at += MAX_MS;
  }
  // 61 s clean after the last strike: the count is gone, the next arms 2 s.
  const uint32_t last = at - MAX_MS;
  CHECK(failures_of(t, kPhone, last + WINDOW_MS) != 0, "the window ends after 60 s clean, not at");
  CHECK(failures_of(t, kPhone, last + 61000) == 0, "61 s clean clears the count");
  CHECK(record_failure(t, kPhone, last + 61000) == BASE_MS, "a strike 61 s later arms 2 s");
}

static void the_wrap_of_millis_ends_no_backoff_early() {
  Table t{};
  const uint32_t armed_at = 0xFFFFFFFFu - 1023u;   // 1024 ms before the wrap
  CHECK(record_failure(t, kPhone, armed_at) == 2000, "2 s armed");
  CHECK(locked(t, kPhone, armed_at + 500), "held 500 ms later, before the wrap");
  CHECK(locked(t, kPhone, armed_at + 1500), "held 1500 ms later, past the wrap");
  CHECK(remaining_ms(t, kPhone, armed_at + 1500) == 500, "500 ms left past the wrap, got %u",
        (unsigned)remaining_ms(t, kPhone, armed_at + 1500));
  CHECK(!locked(t, kPhone, armed_at + 2500), "free 2500 ms later");
  CHECK(remaining_ms(t, kPhone, armed_at + 2500) == 0, "nothing left, never a wrapped difference");
  CHECK(failures_of(t, kPhone, armed_at + 2500) == 1, "the count stands inside the window");
  // The window, read across the wrap too.
  CHECK(failures_of(t, kPhone, armed_at + WINDOW_MS + 1) == 0, "the window clears across the wrap");
}

static void the_window_clears_a_count_nobody_asked_about() {
  Table t{};
  CHECK(record_failure(t, kPhone, 1000) == 2000, "first strike 2 s");
  // No locked() in between: a caller that records without asking first.
  CHECK(record_failure(t, kPhone, 1000 + 61000) == 2000, "61 s later it is a first strike again, not 4 s");
  CHECK(failures_of(t, kPhone, 1000 + 61000) == 1, "one strike counted");
}

static void the_count_saturates_and_still_holds() {
  Table t{};
  uint32_t now = 1000;
  for (int i = 0; i < 300; ++i) record_failure(t, kPhone, now++);
  CHECK(failures_of(t, kPhone, now) == MAX_COUNT, "300 strikes leave %u, want %u",
        (unsigned)failures_of(t, kPhone, now), (unsigned)MAX_COUNT);
  CHECK(locked(t, kPhone, now), "and the source is held");
  CHECK(remaining_ms(t, kPhone, now) <= MAX_MS, "never past the cap");
}

static void the_overflow_bounds_what_the_slots_cannot_track() {
  Table t{};
  // Eight sources fill the Slots, one strike each at 1000 (2 s each). The
  // eighth strikes later, at 30000, so its Slot outlives the others.
  for (unsigned i = 1; i <= 7; ++i) record_failure(t, ip4(10, 0, 0, i), 1000);
  record_failure(t, ip4(10, 0, 0, 8), 30000);
  for (unsigned i = 1; i <= 8; ++i) {
    CHECK(failures_of(t, ip4(10, 0, 0, i), 30000) == 1, "source %u tracked", i);
  }
  // A ninth finds no free Slot: it strikes the overflow, three times.
  const uint32_t ninth = ip4(10, 0, 0, 9);
  const uint32_t tenth = ip4(10, 0, 0, 10);
  CHECK(record_failure(t, ninth, 30000) == 2000, "the overflow's first strike");
  CHECK(record_failure(t, ninth, 32000) == 4000, "its second");
  CHECK(record_failure(t, ninth, 36000) == 8000, "its third, until 44000");
  CHECK(failures_of(t, ninth, 36000) == 0, "the ninth has no Slot of its own");
  CHECK(t.overflow.failures == 3, "the overflow counted three");
  // A tenth source, untracked, never struck: held while the overflow runs.
  CHECK(locked(t, tenth, 40000), "an untracked source is held while the overflow runs");
  CHECK(remaining_ms(t, tenth, 40000) == 4000, "it reads the overflow's backoff");
  // A tracked source is judged by its own Slot only: source 1's backoff
  // ended at 3000, the overflow's has not.
  CHECK(!locked(t, ip4(10, 0, 0, 1), 40000), "a tracked source is not held by the overflow");
  // A success from an untracked source frees nothing: the overflow runs on.
  record_success(t, tenth);
  record_success(t, ninth);
  CHECK(locked(t, tenth, 40000), "a success never clears the overflow");
  // Once the overflow's backoff ends, the tenth is free.
  CHECK(!locked(t, tenth, 44000), "free once the overflow's backoff ends");
  // Slots 1-7 settle 60 s after 1000; the eighth's (struck at 30000) does
  // not. A new source then takes a settled Slot, not the overflow.
  const uint32_t later = 1000 + WINDOW_MS + 1;
  const uint32_t eleventh = ip4(10, 0, 0, 11);
  CHECK(failures_of(t, ip4(10, 0, 0, 1), later) == 0, "source 1's Slot settled");
  CHECK(failures_of(t, ip4(10, 0, 0, 8), later) == 1, "source 8's Slot has not");
  record_failure(t, eleventh, later);
  CHECK(failures_of(t, eleventh, later) == 1, "a settled Slot is reused before the overflow");
  CHECK(t.overflow.failures == 3, "the overflow untouched by it");
  // The overflow settles like any Slot, 60 s after its last strike.
  CHECK(failures_of(t, ninth, 36000 + WINDOW_MS + 1) == 0 && t.overflow.failures == 0,
        "the overflow clears after 60 s clean");
}

static void a_success_frees_only_its_own_slot() {
  Table t{};
  record_failure(t, kPhone, 1000);
  record_failure(t, kLaptop, 1000);
  record_success(t, kPhone);
  CHECK(failures_of(t, kPhone, 1001) == 0, "the phone's Slot freed");
  CHECK(failures_of(t, kLaptop, 1001) == 1, "the laptop's untouched");
  CHECK(!locked(t, kPhone, 1001) && locked(t, kLaptop, 1001), "one held, the other not");
}

static void one_hosts_strikes_hold_no_other_host() {
  Table t{};
  record_failure(t, kPhone, 1000);
  CHECK(locked(t, kPhone, 1500), "the striking host is held");
  CHECK(!locked(t, kLaptop, 1500), "another host is not");
  CHECK(failures_of(t, kLaptop, 1500) == 0, "and has no strike");
}

static void grant_decides_in_order() {
  Table t{};
  int calls = 0;
  auto right = [&calls] { ++calls; return true; };
  auto wrong = [&calls] { ++calls; return false; };
  // No credential: NONE, nothing compared, nothing recorded.
  CHECK(grant(t, kPhone, 1000, false, wrong) == BearerResult::NONE, "no credential is NONE");
  CHECK(calls == 0, "and nothing compared");
  CHECK(failures_of(t, kPhone, 1000) == 0 && t.overflow.failures == 0, "and nothing recorded");
  // A wrong one: exactly one strike.
  CHECK(grant(t, kPhone, 1000, true, wrong) == BearerResult::WRONG, "a wrong one is WRONG");
  CHECK(calls == 1, "compared once");
  CHECK(failures_of(t, kPhone, 1000) == 1, "one strike");
  // Inside the backoff: HELD, and the right one is never compared.
  calls = 0;
  CHECK(grant(t, kPhone, 1500, true, right) == BearerResult::HELD, "held");
  CHECK(grant(t, kPhone, 1600, true, wrong) == BearerResult::HELD, "held, a guess too");
  CHECK(calls == 0, "nothing compared while held");
  CHECK(failures_of(t, kPhone, 1600) == 1, "nothing recorded while held");
  // Another host is not held, and its right one is OK.
  CHECK(grant(t, kLaptop, 1600, true, right) == BearerResult::OK, "another host's right one is OK");
  // After the backoff: OK, and the Slot is freed.
  CHECK(grant(t, kPhone, 3000, true, right) == BearerResult::OK, "OK after the backoff");
  CHECK(failures_of(t, kPhone, 3000) == 0, "the Slot freed");
  CHECK(tracked(t, kPhone) == nullptr, "and no longer tracked");
  // An untracked source the overflow holds: HELD, never compared.
  Table full{};
  for (unsigned i = 1; i <= SLOTS; ++i) record_failure(full, ip4(10, 0, 1, i), 1000);
  record_failure(full, ip4(10, 0, 1, 99), 1000);
  calls = 0;
  CHECK(grant(full, kPhone, 1500, true, right) == BearerResult::HELD, "the overflow holds an untracked source");
  CHECK(calls == 0, "and its credential is never compared");
}

int main() {
  the_schedule_doubles_to_the_window_and_stops();
  the_wrap_of_millis_ends_no_backoff_early();
  the_window_clears_a_count_nobody_asked_about();
  the_count_saturates_and_still_holds();
  the_overflow_bounds_what_the_slots_cannot_track();
  a_success_frees_only_its_own_slot();
  one_hosts_strikes_hold_no_other_host();
  grant_decides_in_order();

  if (g_failures) {
    std::printf("test_auth_backoff: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_auth_backoff: all checks passed\n");
  return 0;
}
