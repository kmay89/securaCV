// Host tests for firmware/common/time/clock_sync.h — when a Canary may believe
// its clock and whom it asks.

#include "../common/time/clock_sync.h"

#include <cstdio>
#include <cstring>

using namespace canary::time_sync;

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

static void believable_means_after_2024() {
  CHECK(BELIEVABLE_EPOCH == 1704067200, "2024-01-01T00:00:00Z");
  CHECK(!believable(0), "the reset clock");
  CHECK(!believable(1704067199), "a second before");
  CHECK(believable(1704067200), "the first believable second");
  CHECK(believable(1760745600), "2025-10-18");
}

static void the_hub_is_the_third_server_never_the_first() {
  // The two disclosed pools, by name — the supervisor hands these literals
  // to configTzTime() and the hub after them.
  CHECK(!std::strcmp(NTP_PRIMARY, "pool.ntp.org"), "pool first");
  CHECK(!std::strcmp(NTP_SECONDARY, "time.nist.gov"), "nist second");
  CHECK(!std::strcmp(hub_server("homeassistant.local"), "homeassistant.local"), "hub third");
  CHECK(hub_server(nullptr) == nullptr, "no hub: slot unused");
  CHECK(hub_server("") == nullptr, "empty hub: slot unused");
}

static void a_node_online_half_a_minute_without_time_says_so() {
  CHECK(SYNC_GRACE_MS == 30000, "thirty seconds");
  CHECK(!sync_overdue(10000 + 29999, 10000, false), "within grace");
  CHECK(sync_overdue(10000 + 30000, 10000, false), "grace over");
  CHECK(!sync_overdue(10000 + 90000, 10000, true), "a believable clock is never overdue");
  CHECK(sync_overdue(0xFFFFFF00u + 30000, 0xFFFFFF00u, false), "across the wrap");
}

static void the_source_words() {
  CHECK(!std::strcmp(source_label(Source::None), "none"), "none");
  CHECK(!std::strcmp(source_label(Source::Ntp), "ntp"), "ntp");
  CHECK(!std::strcmp(source_label(Source::Hub), "hub"), "hub");
  CHECK(!std::strcmp(source_label(Source::Set), "set"), "set");
  CHECK(!std::strcmp(source_label(Source::Gps), "gps"), "gps (the WAP's L76K)");
}

int main() {
  believable_means_after_2024();
  the_hub_is_the_third_server_never_the_first();
  a_node_online_half_a_minute_without_time_says_so();
  the_source_words();
  if (g_failures) {
    std::printf("test_clock_sync: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_clock_sync: all passed\n");
  return 0;
}
