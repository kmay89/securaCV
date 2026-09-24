/* Host tests for the canary's NVS store helpers, run on the firmware's own
 * code: nvs_store_u32() and nvs_store_bytes(), the NvsManager puts under them
 * and the session begin()/end() around them, all cut out of
 * firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp verbatim by
 * cut_functions.awk (the Makefile writes build/nvs_store_result.inc) and
 * compiled here against the real securacv_crypto.h.
 *
 * THE FAILURE THIS PREVENTS (repo sweep F55). Both helpers returned true once
 * their session opened, whatever the put wrote, so a write NVS refused (a
 * full partition, a flash error) read as stored: the witness chain's atomic
 * {seq, head} blob was dropped with nothing counting or logging it. Now each
 * returns true only when the put wrote the whole value. The stubs'
 * Preferences (stubs/nvs_manager) answers puts as the real one does and can
 * be told to report a refused or short write.
 *
 * F52's lock stays as it was: every path ends the session it began, so a
 * failed write never keeps the NVS lock, a helper that cannot get the lock
 * writes nothing and leaves the other task's session open, and a helper
 * called inside a read-only session on its own task writes on a reopened
 * read-write handle and leaves the outer session open.
 *
 * What this does not prove: ESP-IDF's NVS, or anything on a device. The
 * canary envs compile the real thing in CI; it has not run on a bench.
 *
 * Build & run: make -C firmware/tests_host (the cut needs the Makefile).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "securacv_crypto.h"  // the real declarations, over the stubs
#include "canary_config.h"    // NVS_MAIN_NS, as securacv_crypto.cpp has it

// NvsManager's constructor is private and instance() is its only way in; the
// firmware's is a one-line static local, and so is this one.
NvsManager& NvsManager::instance() {
  static NvsManager s_instance;
  return s_instance;
}

// securacv_crypto.cpp's own session functions, puts and store helpers.
#include "nvs_store_result.inc"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

static NvsManager& nvs() { return NvsManager::instance(); }
static HostRecursiveMutex& lock() { return g_host_mutexes[0]; }
static Preferences& handle() { return *g_host_prefs[0]; }

static const uint8_t kBlob[39] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

// After every call: no session anywhere, the lock free, the handle closed,
// and nothing the fakes count as a fault.
static void check_idle_and_clean() {
  CHECK(lock().count == 0 && lock().owner == -1);
  CHECK(!handle().started);
  CHECK(!nvs().isOpen());
  CHECK(lock().bad_gives == 0);
  CHECK(handle().double_begins == 0);
  CHECK(handle().ends_while_closed == 0);
  CHECK(handle().wrong_namespace == 0);
  CHECK(g_host_null_handle_calls == 0);
}

// ── a write that landed is true, and it ran on a writable handle ────────────
static void test_a_full_write_is_true() {
  g_host_task = 0;
  const int puts = handle().puts, rw = handle().puts_open_rw;
  CHECK(nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  CHECK(handle().puts == puts + 1 && handle().puts_open_rw == rw + 1);
  CHECK(handle().last_put_len == sizeof(kBlob));
  check_idle_and_clean();

  CHECK(nvs_store_u32(NVS_KEY_BOOTS, 7));
  CHECK(handle().puts == puts + 2 && handle().puts_open_rw == rw + 2);
  CHECK(handle().last_put_len == sizeof(uint32_t));
  check_idle_and_clean();
}

// ── the failure: a put NVS refused read as stored ───────────────────────────
static void test_a_refused_put_is_false() {
  g_host_task = 0;
  handle().put_reports = 0;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  check_idle_and_clean();                // the session ended all the same

  handle().put_reports = 0;
  CHECK(!nvs_store_u32(NVS_KEY_BOOTS, 7));
  check_idle_and_clean();
}

// ── the failure: a short put read as stored ─────────────────────────────────
//
// Preferences reports all or nothing today; the helpers still ask for the
// whole length rather than "anything at all", so a partial count is false.
static void test_a_short_put_is_false() {
  g_host_task = 0;
  handle().put_reports = (long)sizeof(kBlob) - 1;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  check_idle_and_clean();

  handle().put_reports = 1;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  check_idle_and_clean();

  handle().put_reports = 2;
  CHECK(!nvs_store_u32(NVS_KEY_BOOTS, 7));
  check_idle_and_clean();

  // A put that reports MORE than asked is not a full write of this value.
  handle().put_reports = (long)sizeof(kBlob) + 1;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  handle().put_reports = 8;
  CHECK(!nvs_store_u32(NVS_KEY_BOOTS, 7));
  check_idle_and_clean();
}

// ── a zero-length store keeps nothing, so it is not a write ─────────────────
static void test_zero_length_is_false() {
  g_host_task = 0;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, 0));
  check_idle_and_clean();
}

// ── a session that did not open writes nothing and is false ─────────────────
static void test_a_failed_begin_writes_nothing() {
  g_host_task = 0;
  const int puts = handle().puts;
  handle().fail_next = true;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  handle().fail_next = true;
  CHECK(!nvs_store_u32(NVS_KEY_BOOTS, 7));
  CHECK(handle().puts == puts);
  check_idle_and_clean();
}

// ── F52 intact: another task's session is neither written under nor closed ──
//
// The httpd task (1) holds a read-only session. The loop's store (task 0)
// waits, gives up, writes nothing and answers false; the httpd task's handle
// is still open, in its mode, and its session ends normally afterwards.
static void test_another_tasks_session_is_left_alone() {
  g_host_task = 1;
  CHECK(nvs().begin(true));
  const int puts = handle().puts;

  g_host_task = 0;
  CHECK(!nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  CHECK(!nvs_store_u32(NVS_KEY_BOOTS, 7));
  CHECK(handle().puts == puts);
  CHECK(handle().started && handle().read_only);
  CHECK(lock().owner == 1 && lock().count == 1);

  g_host_task = 1;
  nvs().end();
  check_idle_and_clean();
}

// ── F52 intact: a store inside this task's own read-only session ────────────
//
// The helper's read-write begin reopens the handle read-write, the put runs
// on it, and the helper's end() leaves the outer session open (read-write
// now, which is what the reopen means). A refused put there is still false,
// and still leaves the outer session as it found it.
static void test_a_store_nested_in_a_read_only_session() {
  g_host_task = 0;
  CHECK(nvs().begin(true));
  const int rw = handle().puts_open_rw;
  CHECK(nvs_store_bytes(NVS_KEY_CHAINST, kBlob, sizeof(kBlob)));
  CHECK(handle().puts_open_rw == rw + 1);
  CHECK(handle().started && lock().count == 1 && nvs().isOpen());

  handle().put_reports = 0;
  CHECK(!nvs_store_u32(NVS_KEY_BOOTS, 7));
  CHECK(handle().started && lock().count == 1 && nvs().isOpen());

  nvs().end();
  check_idle_and_clean();
}

int main() {
  g_host_prefs_namespace = NVS_MAIN_NS;
  (void)nvs();
  CHECK(g_host_mutexes_created == 1 && g_host_prefs_count == 1);
  test_a_full_write_is_true();
  test_a_refused_put_is_false();
  test_a_short_put_is_false();
  test_zero_length_is_false();
  test_a_failed_begin_writes_nothing();
  test_another_tasks_session_is_left_alone();
  test_a_store_nested_in_a_read_only_session();

  if (g_failures == 0) { std::printf("ALL nvs-store-result tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
