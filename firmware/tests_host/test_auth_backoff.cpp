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
//   * the count saturates: 300 strikes leave 10 and the source held, and the
//     429 at the last strike says 61 (the old uint8_t count wrapped to a
//     clean 0 after 256);
//   * the overflow: eight sources fill the Slots, a ninth strikes the shared
//     overflow, and a tenth, untracked, is held while it runs and free once
//     it ends; a tracked source is judged by its own Slot only; a settled
//     Slot is reused before the overflow; a success from an untracked
//     source leaves the overflow running;
//   * grant(): no credential is NONE and records nothing; a held source is
//     HELD and its credential is never compared; the right one is OK and
//     frees the Slot; a wrong one is WRONG and exactly one strike;
//   * the edges a mutation found unpinned: a Slot left untouched for more
//     than 2^31 ms reads free from locked() itself, and from remaining_ms()
//     asked first (each its own settle, before the signed "still running"
//     test flips); the overflow's count stands at
//     exactly 60 s after its last strike and is gone 1 ms later, as a
//     Slot's; and source 0 (the bucket every unnamed source shares) is found
//     in its own live Slot, never in a freed one before it (a freed Slot
//     reads source 0 too);
//   * the guess rates the docs quote: at most 30 strikes a minute from any
//     number of addresses while the table stands, and about 155, 100 and
//     37 a minute for a guesser that empties it with a reboot and finds the
//     unit back 5, 10 and 45 s later.

#include "../common/network/auth_backoff.h"

#include <cstdio>
#include <initializer_list>

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
  // The numbers the console line and the 429 show, not only the constant:
  // the count stops at 10, and the 429 read at the last strike's own
  // millisecond announces 61, never more (a count that saturated at 6 or at
  // 255 would pass the two checks above).
  CHECK(failures_of(t, kPhone, now) == 10, "300 strikes leave a count of %u, want 10",
        (unsigned)failures_of(t, kPhone, now));
  CHECK(retry_after_s(remaining_ms(t, kPhone, now - 1)) == 61, "the 429 at the last strike says %lu, want 61",
        retry_after_s(remaining_ms(t, kPhone, now - 1)));
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

// locked() settles first. A Slot armed at `armed` and untouched until more
// than 2^31 ms later reads `until_ms - now` as a positive int32_t again:
// without its own settle, locked() read such a Slot as held, for up to
// another 24.8 days, until the difference turned negative once more.
static void a_slot_idle_past_half_the_clock_reads_free() {
  Table t{};
  const uint32_t armed = 5000;
  record_failure(t, kPhone, armed);
  const uint32_t later = armed + 0x80000000u + 5000u;
  CHECK(!locked(t, kPhone, later), "a Slot idle for 2^31 ms + 5 s reads held");
  CHECK(remaining_ms(t, kPhone, later) == 0, "and has %u ms left", (unsigned)remaining_ms(t, kPhone, later));
  CHECK(failures_of(t, kPhone, later) == 0, "its count is gone");
  // remaining_ms() asked first, on a table nothing settled since the strike:
  // its own settle frees the Slot before the signed test flips (without it
  // the Slot reads 2^31 - 3000 ms left).
  Table u{};
  record_failure(u, kPhone, armed);
  CHECK(remaining_ms(u, kPhone, later) == 0, "remaining_ms() first reads %u ms left on a Slot idle 2^31 ms + 5 s",
        (unsigned)remaining_ms(u, kPhone, later));
}

// The overflow's window ends where a Slot's does: its count stands at
// exactly WINDOW_MS after its last strike and is gone 1 ms later.
static void the_overflow_window_ends_after_60_s_not_at() {
  const uint32_t at = 10000;
  const uint32_t untracked = ip4(10, 0, 2, 99);
  for (int k = 0; k < 2; ++k) {
    Table t{};
    for (unsigned i = 1; i <= SLOTS; ++i) record_failure(t, ip4(10, 0, 2, i), at);
    CHECK(record_failure(t, untracked, at) == BASE_MS, "the overflow's first strike");
    CHECK(t.overflow.failures == 1, "struck the overflow");
    const uint32_t now = at + WINDOW_MS + (uint32_t)k;
    // A ninth address strikes again: a second strike while the count stands
    // (4 s), a first once it is gone (2 s). The eight Slots, struck at the
    // same moment, settle with it, so at +1 ms it takes a freed Slot instead.
    const uint32_t armed = record_failure(t, untracked, now);
    if (k == 0) {
      CHECK(armed == 2 * BASE_MS, "at exactly 60 s the overflow's count stands: armed %u ms", (unsigned)armed);
      CHECK(t.overflow.failures == 2, "the overflow counted %u", (unsigned)t.overflow.failures);
    } else {
      CHECK(armed == BASE_MS, "60 s + 1 ms clears the overflow's count: armed %u ms", (unsigned)armed);
      CHECK(t.overflow.failures == 0, "the overflow still counts %u", (unsigned)t.overflow.failures);
    }
  }
  // The overflow read alone, with every Slot still live: struck 1 ms after
  // the Slots, it outlives them by 1 ms.
  Table t{};
  for (unsigned i = 1; i <= SLOTS; ++i) record_failure(t, ip4(10, 0, 2, i), at + 1);
  record_failure(t, untracked, at + 1);
  CHECK(locked(t, ip4(10, 0, 2, 200), at + 1), "the overflow holds an untracked source");
  settle(t, at + 1 + WINDOW_MS);
  CHECK(t.overflow.failures == 1, "at exactly 60 s the overflow is not settled");
  settle(t, at + 1 + WINDOW_MS + 1);
  CHECK(t.overflow.failures == 0, "1 ms later it is");
}

// A freed Slot is Slot{}: failures 0 and source 0. Source 0, the bucket every
// source the caller cannot name shares (an unreadable socket, a real IPv6
// peer), is a real key, so tracked() must never take a freed Slot for it.
static void source_zero_is_found_in_its_own_slot_behind_a_freed_one() {
  Table t{};
  record_failure(t, kPhone, 1000);   // Slot 0
  record_failure(t, 0, 1000);        // Slot 1
  record_success(t, kPhone);         // Slot 0 freed: Slot{}, source 0
  CHECK(failures_of(t, 0, 1001) == 1, "source 0's strike reads %u", (unsigned)failures_of(t, 0, 1001));
  CHECK(locked(t, 0, 1001), "source 0 is held by its own Slot");
  // Its second strike lands in its own Slot: 4 s, not a first strike in the
  // freed one.
  CHECK(record_failure(t, 0, 1000 + BASE_MS) == 2 * BASE_MS, "source 0's count split across two Slots");
  CHECK(t.slot[0].failures == 0, "the freed Slot was taken");
}

// Strikes from SLOTS + 1 sources (nine), each the moment its own backoff
// ends, from `start` until `end`; at most `per_source` each (0: no limit).
// SLOTS of them take the Slots and the last the overflow: every further
// address would share the overflow with it, so these are every backoff a
// guesser can run at once.
static constexpr int kSources = SLOTS + 1;
static long strike_whenever_free(Table& t, uint32_t start, uint32_t end, int per_source) {
  uint32_t next[kSources + 1];
  int struck[kSources + 1] = {0};
  long strikes = 0;
  for (int i = 1; i <= kSources; ++i) next[i] = start;
  for (;;) {
    int pick = 0;
    for (int i = 1; i <= kSources; ++i) {
      if (per_source != 0 && struck[i] >= per_source) continue;
      if ((int32_t)(next[i] - end) >= 0) continue;
      if (pick == 0 || (int32_t)(next[i] - next[pick]) < 0) pick = i;
    }
    if (pick == 0) return strikes;
    const uint32_t now = next[pick];
    if (grant(t, (uint32_t)pick, now, true, [] { return false; }) == BearerResult::WRONG) {
      ++strikes;
      ++struck[pick];
    }
    const uint32_t rem = remaining_ms(t, (uint32_t)pick, now);
    next[pick] = now + (rem != 0 ? rem : 1);
  }
}

// The guess rates the docs quote (THREAT_MODEL.md scenario 10, magic_pairing.md
// §3.7; third review of F343). Between reboots: a guesser's best schedule
// strikes each backoff k times, each the moment the one before ends, then
// waits out the 60 s window, and k = 5 (strikes at 0, 2, 6, 14 and 30 s, then
// clean at 90 s) gives 30 a minute over nine backoffs; no k gives more. A
// reboot empties the table (the WAP keeps it in RAM, and any landing session
// can reboot that unit), so a guesser that strikes for a few seconds and then
// reboots it gets more, depending on how soon the unit is back: about 155 a
// minute at 5 s, about 100 at 10 s, about 37 at 45 s.
static void the_documented_guess_rates_hold() {
  double best = 0;
  int best_k = 0;
  for (int k = 1; k <= MAX_COUNT; ++k) {
    Table t{};
    uint32_t now = 1000;
    long strikes = 0;
    for (int cycle = 0; cycle < 20; ++cycle) {
      strikes += strike_whenever_free(t, now, now + 10u * WINDOW_MS, k);
      uint32_t last = t.overflow.last_failure_ms;
      for (const Slot& s : t.slot) {
        if ((int32_t)(s.last_failure_ms - last) > 0) last = s.last_failure_ms;
      }
      now = last + WINDOW_MS + 1;   // the first ms every backoff is clean
      CHECK(failures_of(t, 1, now) == 0 && t.overflow.failures == 0, "k=%d: the table is clean", k);
    }
    const double per_minute = (double)strikes * 60000.0 / (double)(now - 1000);
    if (per_minute > best) {
      best = per_minute;
      best_k = k;
    }
  }
  std::printf("guess rate between reboots: at most %.1f a minute (%d strikes per backoff, then clean)\n",
              best, best_k);
  CHECK(best_k == 5 && best > 29.9 && best < 30.1, "the best schedule is %.1f a minute at k=%d, want 30 at k=5",
        best, best_k);

  struct Back {
    uint32_t after_ms;
    double low, high;
  };
  for (const Back& b : {Back{5000, 150, 160}, Back{10000, 95, 105}, Back{45000, 35, 40}}) {
    double rate = 0;
    uint32_t for_ms = 0;
    for (uint32_t w = 1; w <= 2 * WINDOW_MS; w += 50) {
      Table t{};   // the reboot: a table in RAM starts empty
      const double r = (double)strike_whenever_free(t, 1000, 1000 + w, 0) * 60000.0 / (double)(w + b.after_ms);
      if (r > rate) {
        rate = r;
        for_ms = w;
      }
    }
    std::printf("guess rate rebooting the unit, back %lu s later: %.1f a minute (striking for %lu ms)\n",
                (unsigned long)(b.after_ms / 1000), rate, (unsigned long)for_ms);
    CHECK(rate > b.low && rate < b.high, "back %lu s later: %.1f a minute, want %.0f-%.0f",
          (unsigned long)(b.after_ms / 1000), rate, b.low, b.high);
  }
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
  a_slot_idle_past_half_the_clock_reads_free();
  the_overflow_window_ends_after_60_s_not_at();
  source_zero_is_found_in_its_own_slot_behind_a_freed_one();
  the_documented_guess_rates_hold();

  if (g_failures) {
    std::printf("test_auth_backoff: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_auth_backoff: all checks passed\n");
  return 0;
}
