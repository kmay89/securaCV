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
 * CSI_TEST_HOST_BUILD), not a stub. A tamper emit is state-bearing, which
 * would normally only OPEN a RAM bundle — so the module force-closes it at
 * once (csi_bundler_flush_key): the failure a tamper records is exactly the
 * one that would erase a two-minute buffer. These tests observe
 * csi_event_on_committed — the weak host hook this file overrides, and the
 * exact boundary where the real firmware persists a row (ring, event log,
 * MQTT) — so the sealed-immediately behavior is itself under test: after
 * any accepted emit the commit hook must already have fired, with nothing
 * left open in the bundler.
 *
 * Build & run: via this directory's Makefile (mirrors the CI contract).
 */
#include <cstdio>
#include <cstring>

#include "csi_event.h"
#include "csi_bundler.h"
#include "tamper_events_module.h"

/* Strong override of the weak host commit hook: record every row that
 * reaches persistence, newest last. This is the durability ledger the
 * assertions read. */
static char   g_committed[16][32];
static size_t g_ncommitted = 0;

extern "C" void csi_event_on_committed(uint32_t /*event_id*/,
                                       const char* /*module_id*/,
                                       const char* /*type_name*/,
                                       csi_event_category_t /*category*/,
                                       csi_privacy_class_t /*privacy*/,
                                       const csi_event_values_t* v) {
  if (g_ncommitted < 16) {
    strncpy(g_committed[g_ncommitted], v->state_name,
            sizeof(g_committed[0]) - 1);
    g_committed[g_ncommitted][sizeof(g_committed[0]) - 1] = '\0';
  }
  g_ncommitted++;
}

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
  g_ncommitted = 0;
}

/* Rows that reached the persistence hook since fresh(). */
static size_t ring_count() { return g_ncommitted; }

/* The newest PERSISTED row's state word, or "" when nothing committed. */
static const char* last_kind() {
  return g_ncommitted ? g_committed[g_ncommitted - 1] : "";
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
  g_ncommitted = 0;
  tamper_events_watch(1, 0, 0, SD_ABSENT);       /* dropped: unregistered */
  CHECK(ring_count() == 0);
  csi_module_register(tamper_events_module());
  tamper_events_watch(1, 0, 0, SD_ABSENT);       /* retried: accepted */
  CHECK(std::strcmp(last_kind(), "unexpected_reboot") == 0);
}

static void test_clean_boot_confesses_nothing() {
  fresh();
  tamper_events_watch(/*crash*/ 0, /*wdt*/ 0, /*brownout*/ 0, SD_ABSENT);
  tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(ring_count() == 0);
  CHECK(csi_bundler_open_count() == 0);
}

static void test_boot_precedence_watchdog_over_brownout_over_panic() {
  fresh();
  tamper_events_watch(1, 1, 1, SD_ABSENT);   /* wdt outranks everything */
  CHECK(std::strcmp(last_kind(), "watchdog") == 0);

  fresh();
  tamper_events_watch(1, 0, 1, SD_ABSENT);   /* brownout next */
  CHECK(std::strcmp(last_kind(), "power_loss") == 0);

  fresh();
  tamper_events_watch(1, 0, 0, SD_ABSENT);   /* bare crash = panic */
  CHECK(std::strcmp(last_kind(), "unexpected_reboot") == 0);
}

static void test_a_tamper_is_sealed_before_the_next_power_loss() {
  /* THE DURABILITY PIN. A state-bearing admit only opens a RAM bundle,
   * and the bundler's gap window is two minutes — a crash loop would
   * erase a buffered boot story before it ever reached the chain. The
   * module force-closes its bundle on every accepted emit: the row must
   * have crossed the persistence hook at once, with nothing left open. */
  fresh();
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  CHECK(ring_count() == 1);
  CHECK(csi_bundler_open_count() == 0);
  CHECK(std::strcmp(last_kind(), "watchdog") == 0);
}

static void test_one_boot_story_per_boot() {
  fresh();
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  CHECK(ring_count() == 1);
  /* Later loops with the same (latched) reset facts add nothing. */
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  tamper_events_watch(1, 1, 0, SD_ABSENT);
  CHECK(ring_count() == 1);
}

static void test_first_call_adopts_sd_state_silently() {
  /* Booting WITH a mounted card: adoption, never a claim. */
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  CHECK(ring_count() == 0);
  /* And a later removal from that adopted state IS the story. */
  tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(std::strcmp(last_kind(), "sd_remove") == 0);
  CHECK(csi_bundler_open_count() == 0);   /* sealed, like every kind */
}

static void test_mounted_to_error_is_sd_error() {
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  tamper_events_watch(0, 0, 0, SD_ERROR);
  CHECK(std::strcmp(last_kind(), "sd_error") == 0);
}

static void test_recovery_is_not_a_tamper() {
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  tamper_events_watch(0, 0, 0, SD_ERROR);
  CHECK(ring_count() == 1);
  /* ERROR -> MOUNTED is the card coming back: silence. */
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  CHECK(ring_count() == 1);
  /* And the recovered state is the new baseline for the next transition. */
  tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(ring_count() == 2);
  CHECK(std::strcmp(last_kind(), "sd_remove") == 0);
}

static void test_constant_absent_feed_never_emits_sd_kinds() {
  /* A host with no SD state machine (the active PIO lane) feeds ABSENT
   * forever — the watcher adopts it and never invents a detector. */
  fresh();
  for (int i = 0; i < 50; ++i) tamper_events_watch(0, 0, 0, SD_ABSENT);
  CHECK(ring_count() == 0);
  CHECK(std::strcmp(tamper_events_active_kind(), "") == 0);
}

static void test_the_standing_condition_speaks_the_present_tense() {
  /* Rows seal instantly, so /api/events/today's envelope carries the
   * standing condition explicitly (tamper_events_active_kind). The rules:
   * nothing to confess = ""; a boot kind stands for the whole boot; an SD
   * kind stands until recovery and OUTRANKS the boot kind while it does;
   * recovery clears only the SD story — how the boot began stays true. */
  fresh();
  CHECK(std::strcmp(tamper_events_active_kind(), "") == 0);

  tamper_events_watch(1, 1, 0, SD_MOUNTED);            /* watchdog boot */
  CHECK(std::strcmp(tamper_events_active_kind(), "watchdog") == 0);
  tamper_events_watch(1, 1, 0, SD_MOUNTED);            /* still standing */
  CHECK(std::strcmp(tamper_events_active_kind(), "watchdog") == 0);

  tamper_events_watch(1, 1, 0, SD_ABSENT);             /* card removed */
  CHECK(std::strcmp(tamper_events_active_kind(), "sd_remove") == 0);

  tamper_events_watch(1, 1, 0, SD_MOUNTED);            /* card back */
  CHECK(std::strcmp(tamper_events_active_kind(), "watchdog") == 0);

  /* A clean boot with an SD story: recovery leaves nothing standing. */
  fresh();
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  tamper_events_watch(0, 0, 0, SD_ERROR);
  CHECK(std::strcmp(tamper_events_active_kind(), "sd_error") == 0);
  tamper_events_watch(0, 0, 0, SD_MOUNTED);
  CHECK(std::strcmp(tamper_events_active_kind(), "") == 0);
}

int main() {
  test_boot_story_survives_a_pre_registration_race();  /* first, by contract */
  test_clean_boot_confesses_nothing();
  test_boot_precedence_watchdog_over_brownout_over_panic();
  test_a_tamper_is_sealed_before_the_next_power_loss();
  test_one_boot_story_per_boot();
  test_first_call_adopts_sd_state_silently();
  test_mounted_to_error_is_sd_error();
  test_recovery_is_not_a_tamper();
  test_constant_absent_feed_never_emits_sd_kinds();
  test_the_standing_condition_speaks_the_present_tense();

  if (g_failures == 0) {
    std::printf("test_tamper_events_logic: ALL tamper watcher tests PASSED\n");
    return 0;
  }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
