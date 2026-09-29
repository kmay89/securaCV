/* Host tests for firmware/common/health/boot_guard.h — the device glue that
 * feeds boot_policy.h real inputs (NVS counter, OTA image state, image id).
 *
 * test_boot_policy.cpp proves the decisions; this proves the glue calls them
 * right, over a fake NVS and a fake OTA partition (stubs/boot_guard/):
 *   · the count persists across boots and trips safe mode on the Nth
 *     unhealthy boot of a confirmed image, then stays there;
 *   · a pending (unconfirmed) image is never sent to safe mode;
 *   · every way out works: healthy, operator clear, a different build (OTA,
 *     A/B rollback or USB re-flash) — and re-flashing the SAME build does not;
 *   · an A/B rollback's churn does not inflate the good image's count;
 *   · a broken NVS boots normally and says so — including one that still
 *     reads but can no longer write, even with a count at the threshold
 *     (safe mode there could never be cleared); an unreadable image id keeps
 *     the count (the conservative direction);
 *   · a good device's steady state does not write NVS on every healthy.
 *
 * What it cannot prove: that the ESP bootloader actually reverts, that
 * nvs_* behave on silicon as the fake does, or that main.cpp calls these at
 * the right moments. Those are the bench rows (docs/V1_BENCH_TEST_RUNBOOK.md,
 * Track E).
 *
 * Build & run: make -C firmware/tests_host (target test_boot_guard).
 */

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "health/boot_guard.h"

using bootpolicy::BootMode;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void fresh_device() {
  g_fake_nvs.reset();
  g_fake_ota = FakeOta{};
}

static void flash_build(uint8_t tag) {
  std::memset(g_fake_ota.sha, 0, sizeof(g_fake_ota.sha));
  g_fake_ota.sha[0] = tag;
}

static void test_first_boot() {
  fresh_device();
  flash_build(0xA1);
  bootguard::Status s = bootguard::begin();
  CHECK(s.nvs_ok);
  CHECK(s.image_confirmed);
  CHECK(s.prev_count == 0);
  CHECK(s.count == 1);
  CHECK(s.mode == BootMode::Normal);
}

static void test_good_image_never_trips_and_spares_flash() {
  fresh_device();
  flash_build(0xA1);
  for (int boot = 0; boot < 50; boot++) {
    bootguard::Status s = bootguard::begin();
    CHECK(s.mode == BootMode::Normal);
    CHECK(s.count == 1);
    CHECK(bootguard::mark_healthy());
  }
  // Two counter writes per boot (1, then 0) plus the one image-id write.
  CHECK(g_fake_nvs.writes == 50 * 2 + 1);
  // A second healthy in the same boot writes nothing.
  const int before = g_fake_nvs.writes;
  CHECK(bootguard::mark_healthy());
  CHECK(g_fake_nvs.writes == before);
}

static void test_confirmed_crash_loop_enters_and_holds_safe_mode() {
  fresh_device();
  flash_build(0xA1);
  const uint16_t T = bootpolicy::kDefaultSafeModeThreshold;
  for (uint16_t boot = 1; boot < T; boot++) {
    bootguard::Status s = bootguard::begin();  // never reaches healthy
    CHECK(s.count == boot);
    CHECK(s.mode == BootMode::Normal);
  }
  bootguard::Status trip = bootguard::begin();
  CHECK(trip.count == T);
  CHECK(trip.mode == BootMode::SafeMode);
  for (int i = 0; i < 40; i++) {
    CHECK(bootguard::begin().mode == BootMode::SafeMode);
  }
  CHECK(bootguard::begin().count == bootpolicy::kBootAttemptCap);
}

static void drive_into_safe_mode() {
  while (bootguard::begin().mode != BootMode::SafeMode) {}
}

static void test_operator_clear_escapes() {
  fresh_device();
  flash_build(0xA1);
  drive_into_safe_mode();
  CHECK(bootguard::operator_clear());
  bootguard::Status s = bootguard::begin();
  CHECK(s.mode == BootMode::Normal);
  CHECK(s.count == 1);
}

static void test_reflash_other_build_escapes_same_build_does_not() {
  fresh_device();
  flash_build(0xA1);
  drive_into_safe_mode();

  // USB re-flash of the SAME build: still the image that could not come up.
  flash_build(0xA1);
  CHECK(bootguard::begin().mode == BootMode::SafeMode);

  // A different build starts the count over.
  flash_build(0xB2);
  bootguard::Status s = bootguard::begin();
  CHECK(s.mode == BootMode::Normal);
  CHECK(s.count == 1);
}

static void test_pending_image_never_safe_mode() {
  fresh_device();
  flash_build(0xA1);
  g_fake_ota.state = ESP_OTA_IMG_PENDING_VERIFY;
  for (int i = 0; i < 40; i++) {
    bootguard::Status s = bootguard::begin();
    CHECK(!s.image_confirmed);
    CHECK(s.mode == BootMode::Normal);
  }
}

static void test_rollback_churn_does_not_inflate_good_image() {
  fresh_device();
  flash_build(0xA1);  // good image A, confirmed, healthy
  CHECK(bootguard::begin().mode == BootMode::Normal);
  CHECK(bootguard::mark_healthy());

  // OTA installs B; B boots pending and crashes before healthy.
  CHECK(bootguard::fresh_image_reset());
  flash_build(0xB2);
  g_fake_ota.state = ESP_OTA_IMG_PENDING_VERIFY;
  bootguard::Status b = bootguard::begin();
  CHECK(b.mode == BootMode::Normal);
  CHECK(b.count == 1);

  // The bootloader reverts to A (VALID). A is a different image from the one
  // that left the count, so it starts clean rather than inheriting B's boot.
  flash_build(0xA1);
  g_fake_ota.state = ESP_OTA_IMG_VALID;
  bootguard::Status a = bootguard::begin();
  CHECK(a.mode == BootMode::Normal);
  CHECK(a.count == 1);
}

static void test_broken_nvs_boots_normally_and_says_so() {
  fresh_device();
  flash_build(0xA1);
  g_fake_nvs.broken = true;
  for (int i = 0; i < 10; i++) {
    bootguard::Status s = bootguard::begin();
    CHECK(!s.nvs_ok);
    CHECK(s.mode == BootMode::Normal);
  }
  CHECK(!bootguard::mark_healthy());
  CHECK(!bootguard::operator_clear());
}

// The count reads fine but this boot's count cannot be written back (or
// committed). Safe mode would be a trap: operator_clear() writes through the
// same failing store, so "clear & retry" would land straight back here.
static void test_unwritable_nvs_at_threshold_boots_normally() {
  fresh_device();
  flash_build(0xA1);
  const uint16_t T = bootpolicy::kDefaultSafeModeThreshold;
  for (uint16_t i = 1; i < T; i++) bootguard::begin();  // stored count T-1
  g_fake_nvs.writes_fail = true;
  for (int i = 0; i < 5; i++) {
    bootguard::Status s = bootguard::begin();  // would persist T -> SafeMode
    CHECK(!s.nvs_ok);
    CHECK(s.prev_count == T - 1);
    CHECK(s.count == T - 1);   // nothing new was persisted
    CHECK(s.mode == BootMode::Normal);
  }
  CHECK(!bootguard::operator_clear());
  // Writes come back: the count resumes where NVS left it, and trips.
  g_fake_nvs.writes_fail = false;
  CHECK(bootguard::begin().mode == BootMode::SafeMode);
}

static void test_unreadable_image_id_keeps_the_count() {
  fresh_device();
  flash_build(0xA1);
  const uint16_t T = bootpolicy::kDefaultSafeModeThreshold;
  for (uint16_t i = 1; i < T; i++) bootguard::begin();
  g_fake_ota.desc_ok = false;  // id unreadable: must not be read as "new image"
  CHECK(bootguard::begin().mode == BootMode::SafeMode);
}

static void test_non_ota_partition_counts_as_confirmed() {
  fresh_device();
  flash_build(0xA1);
  g_fake_ota.has_state = false;  // factory / non-OTA layout
  CHECK(bootguard::image_confirmed());
  drive_into_safe_mode();         // so a crash loop there still degrades
  CHECK(bootguard::begin().mode == BootMode::SafeMode);
}

int main() {
  test_first_boot();
  test_good_image_never_trips_and_spares_flash();
  test_confirmed_crash_loop_enters_and_holds_safe_mode();
  test_operator_clear_escapes();
  test_reflash_other_build_escapes_same_build_does_not();
  test_pending_image_never_safe_mode();
  test_rollback_churn_does_not_inflate_good_image();
  test_broken_nvs_boots_normally_and_says_so();
  test_unwritable_nvs_at_threshold_boots_normally();
  test_unreadable_image_id_keeps_the_count();
  test_non_ota_partition_counts_as_confirmed();

  if (g_failures) {
    std::printf("%d CHECK(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL boot-guard checks PASSED\n");
  return 0;
}
