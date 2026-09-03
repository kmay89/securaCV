/* Host tests for the system.integrity tamper watcher
 * (firmware/common/csi/src/tamper_events_module.{h,cpp}).
 *
 * The transition rules ARE the feature's honesty contract: booting with no
 * card must not read as a removal, recovery must not cry, one boot story
 * per boot, and the doctrine precedence (watchdog > brownout > panic) must
 * hold. They shipped with a test-only reset hook and no test — a future
 * edit could flip the adoption order and compile clean on every CI leg.
 *
 * Driven through the REAL chokepoint (csi_event + csi_module + csi_bundler,
 * CSI_TEST_HOST_BUILD), not a stub: a tamper emit is state-bearing, so it
 * lands in an OPEN bundle — exactly what /api/events/today serves live —
 * and that is where these tests read it back (csi_bundler_snapshot_open).
 *
 * Build & run: via this directory's Makefile (mirrors the CI contract).
 */
#include <cstdio>
#include <cstring>

#include "csi_event.h"
#include "csi_bundler.h"
#include "tamper_events_module.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

/* SdState numeric values, as tamper_events_module.cpp pins them. */
static const uint8_t SD_ABSENT = 0;
static const uint8_t SD_MOUNTED = 1;
static const uint8_t SD_ERROR = 2;

static void fresh() {
  csi_event_test_reset();
  csi_bundler_reset();
  tamper_events_reset();
  csi_module_register(tamper_events_module());
}

/* The newest open bundle's state word, or "" when nothing is open. */
static const char* open_kind() {
  static csi_event_record_t rec[4];
  const size_t n = csi_bundler_snapshot_open(rec, 4);
  return n ? rec[0].values.state_name : "";
}

static void test_clean_boot_confesses_nothing() {
  fresh();
  tamper_events_watch(/*crash*/ 0, /*wdt*/ 0, /*brownout*/ 0, SD_ABSENT);
  tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(csi_bundler_open_count() == 0);
}

static void test_boot_precedence_watchdog_over_brownout_over_panic() {
  fresh();
  tamper_events_watch(1, 1, 1, SD_ABSENT);   /* wdt outranks everything */
  CHECK(std::strcmp(open_kind(), "watchdog") == 0);

  fresh();
  tamper_events_watch(1, 0, 1, SD_ABSENT);   /* brownout next */
  CHECK(std::strcmp(open_kind(), "power_loss") == 0);

  fresh();
  tamper_events_watch(1, 0, 0, SD_ABSENT);   /* bare crash = panic */
  CHECK(std::strcmp(open_kind(), "unexpected_reboot") == 0);
}

static void test_one_boot_story_per_boot() {
  fresh();
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  csi_bundler_flush_all();
  CHECK(csi_bundler_open_count() == 0);
  /* Later loops with the same (latched) reset facts add nothing. */
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  CHECK(csi_bundler_open_count() == 0);
}

static void test_first_call_adopts_sd_state_silently() {
  /* Booting WITH a mounted card: adoption, never a claim. */
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  CHECK(csi_bundler_open_count() == 0);
  /* And a later removal from that adopted state IS the story. */
  tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(std::strcmp(open_kind(), "sd_remove") == 0);
}

static void test_mounted_to_error_is_sd_error() {
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  tamper_events_watch(0, 0, 0, SD_ERROR);
  CHECK(std::strcmp(open_kind(), "sd_error") == 0);
}

static void test_recovery_is_not_a_tamper() {
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  tamper_events_watch(0, 0, 0, SD_ERROR);
  csi_bundler_flush_all();
  /* ERROR -> MOUNTED is the card coming back: silence. */
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  CHECK(csi_bundler_open_count() == 0);
  /* And the recovered state is the new baseline for the next transition. */
  tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(std::strcmp(open_kind(), "sd_remove") == 0);
}

static void test_constant_absent_feed_never_emits_sd_kinds() {
  /* A host with no SD state machine (the active PIO lane) feeds ABSENT
   * forever — the watcher adopts it and never invents a detector. */
  fresh();
  for (int i = 0; i < 50; ++i) tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(csi_bundler_open_count() == 0);
}

static void test_boot_story_survives_a_pre_registration_race() {
  /* watch() before the module is registered: the chokepoint drops the
   * emit, and the bounded retry carries the story to the next loop.
   * MUST RUN FIRST — modules are process-lifetime singletons (the
   * registry deliberately has no reset), so this is the only moment
   * the module is genuinely unregistered. fresh() elsewhere re-calls
   * csi_module_register and relies on the duplicate-id rejection. */
  csi_event_test_reset();
  csi_bundler_reset();
  tamper_events_reset();
  tamper_events_watch(1, 0, 0, SD_ABSENT);       /* dropped: unregistered */
  CHECK(csi_bundler_open_count() == 0);
  csi_module_register(tamper_events_module());
  tamper_events_watch(1, 0, 0, SD_ABSENT);       /* retried: accepted */
  CHECK(std::strcmp(open_kind(), "unexpected_reboot") == 0);
}

int main() {
  test_boot_story_survives_a_pre_registration_race();  /* first, by contract */
  test_clean_boot_confesses_nothing();
  test_boot_precedence_watchdog_over_brownout_over_panic();
  test_one_boot_story_per_boot();
  test_first_call_adopts_sd_state_silently();
  test_mounted_to_error_is_sd_error();
  test_recovery_is_not_a_tamper();
  test_constant_absent_feed_never_emits_sd_kinds();

  if (g_failures == 0) {
    std::printf("test_tamper_events_logic: ALL tamper watcher tests PASSED\n");
    return 0;
  }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
