/* Host tests for NvsManager's session lock, run on the firmware's own code:
 * the constructor, destructor, begin() and end() that
 * firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp compiles, cut
 * out verbatim by nvs_manager_cut.awk (the Makefile writes
 * build/nvs_manager_session.inc) and compiled here against the real
 * securacv_crypto.h.
 *
 * test_nvs_session_depth.cpp proves the depth arithmetic
 * (nvs_session_depth.h) and drives it through a MODEL of begin()/end(). The
 * model is a hand copy, so on its own it cannot catch an edit to the real
 * begin()/end() that breaks the lock accounting: a second give on a stray
 * end(), a failure path that keeps the lock, an end() that closes another
 * task's session, a read-write reopen that skips Preferences::end(). The
 * review of that suite made each of those edits to securacv_crypto.cpp and
 * every host test stayed green. This suite is the check they lacked: the
 * same scenarios, on the code the firmware builds.
 *
 * The stubs (stubs/nvs_manager) replace Arduino's Serial, Preferences and
 * the FreeRTOS recursive mutex. The fake mutex has an owner: a take by the
 * owner nests, a take by any other task while it is held fails at once, and
 * a give by a task that does not hold it is counted as a fault. The host
 * cannot block, so every contended wait here is one that ran out. The fake
 * Preferences refuses a begin on a started handle, as the real one does.
 *
 * What this does not prove: FreeRTOS's own mutex, a wait that ends because
 * the holder finished, or anything on a device. The canary envs compile the
 * real thing in CI; it has not run on a bench.
 *
 * Build & run: make -C firmware/tests_host (the cut needs the Makefile).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "securacv_crypto.h"  // the real class declaration, over the stubs
#include "canary_config.h"    // NVS_MAIN_NS, as securacv_crypto.cpp has it

// NvsManager's constructor is private and instance() is its only way in. The
// firmware's instance() is a one-line static local; the suite supplies the
// same, plus a second instance built while mutex creation fails, for the
// unlocked path.
static bool g_want_unlocked = false;
NvsManager& NvsManager::instance() {
  if (g_want_unlocked) {
    static NvsManager s_unlocked;
    return s_unlocked;
  }
  static NvsManager s_instance;
  return s_instance;
}

// securacv_crypto.cpp's own constructor, destructor, begin() and end().
#include "nvs_manager_session.inc"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

using nvs_session::kMaxDepth;
using nvs_session::kSessionWaitMs;

static NvsManager& nvs() { return NvsManager::instance(); }
static HostRecursiveMutex& lock() { return g_host_mutexes[0]; }
static Preferences& handle() { return *g_host_prefs[0]; }

static bool begin_on(int task, bool ro) {
  g_host_task = task;
  return nvs().begin(ro);
}
static void end_on(int task) {
  g_host_task = task;
  nvs().end();
}

// Between scenarios: no session anywhere, the lock free, the handle closed,
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

// ── the constructor makes the one recursive mutex ───────────────────────────
//
// The stub offers only the recursive create, so a non-recursive mutex does
// not compile here; this pins that the constructor made exactly one.
static void test_constructor_creates_the_lock() {
  (void)nvs();
  CHECK(g_host_mutexes_created == 1);
  CHECK(g_host_prefs_count == 1);
  check_idle_and_clean();
}

// ── the failure the fix exists for: one task closing another's session ─────
//
// The wave-7 security scout's race, on the real begin()/end(). The httpd
// task (1) is reading the broker row; the loop (0) tries a session and its
// wait runs out; a stray end() from the loop must not touch the handle. The
// timeout is said once per boot, and begin() asks for kSessionWaitMs while
// end() never waits.
static void test_one_task_cannot_close_anothers_session() {
  CHECK(begin_on(1, true));
  CHECK(lock().last_wait == pdMS_TO_TICKS(kSessionWaitMs));
  CHECK(handle().started && handle().read_only);
  CHECK(lock().owner == 1 && lock().count == 1);

  CHECK(Serial.lines == 0);
  CHECK(!begin_on(0, true));             // held by the httpd task: fail soft
  CHECK(Serial.lines == 1);
  CHECK(std::strstr(Serial.last, "[NVS] session wait timed out") != nullptr);

  end_on(0);                             // a stray end() from the loop
  CHECK(lock().last_wait == 0);          // end() never waits
  CHECK(handle().started);               // the httpd task's handle is still open
  CHECK(lock().owner == 1 && lock().count == 1);

  CHECK(!begin_on(2, false));            // a second timeout (the OTA task)...
  CHECK(Serial.lines == 1);              // ...is not reported again

  end_on(1);
  CHECK(!handle().started);
  CHECK(lock().count == 0);
  CHECK(begin_on(0, false));             // now the loop's write session opens
  CHECK(handle().started && !handle().read_only);
  CHECK(nvs().isOpen() && !nvs().isReadOnly());
  end_on(0);

  end_on(2);                             // an end() with no session anywhere
  check_idle_and_clean();
}

// ── the failure: a nested end() closing the outer handle, or a nested write
//    handed a read-only one ────────────────────────────────────────────────
//
// On one task: read-only, then read-write inside it (the real reopen: end,
// then begin read-write — never a begin on a started handle), then read-only
// inside that (keeps the read-write handle). Only the outermost end() closes,
// and the lock's count follows the depth down.
static void test_nesting_on_one_task() {
  CHECK(begin_on(0, true));
  CHECK(begin_on(0, false));
  CHECK(handle().started && !handle().read_only);
  CHECK(begin_on(0, true));
  CHECK(!handle().read_only);
  CHECK(lock().count == 3);
  end_on(0);
  CHECK(handle().started && lock().count == 2);
  end_on(0);
  CHECK(handle().started && !handle().read_only && lock().count == 1);
  end_on(0);
  check_idle_and_clean();
}

// ── the failure: a failed begin keeping the lock ────────────────────────────
//
// A begin() that returns false owes no end() — none of its callers makes
// one — so it must give back the take it made, or every other task's begin
// stalls behind a lock nobody holds a session under. Both failures: a first
// open that fails, and a read-write reopen inside a read-only session that
// fails (the handle is then gone, and the next begin opens it again).
static void test_failed_begin_gives_the_lock_back() {
  handle().fail_next = true;
  CHECK(!begin_on(0, true));
  CHECK(lock().count == 0 && lock().owner == -1);
  CHECK(begin_on(1, false));             // another task is not locked out
  end_on(1);
  check_idle_and_clean();

  CHECK(begin_on(0, true));
  handle().fail_next = true;
  CHECK(!begin_on(0, false));            // the reopen failed
  CHECK(lock().count == 1);              // only the outer session's take
  CHECK(!handle().started && !nvs().isOpen());
  CHECK(begin_on(0, true));              // opens again, not "keeps" nothing
  CHECK(handle().started);
  end_on(0);
  CHECK(handle().started);               // the outer session's handle
  end_on(0);
  check_idle_and_clean();
}

// ── the failure: a runaway nesting wrapping the count ───────────────────────
static void test_depth_is_bounded() {
  for (int i = 0; i < kMaxDepth; ++i) CHECK(begin_on(0, (i % 2) != 0));
  CHECK(lock().count == kMaxDepth);
  CHECK(!begin_on(0, true));             // refused, and its take given back
  CHECK(lock().count == kMaxDepth);
  for (int i = 0; i < kMaxDepth; ++i) end_on(0);
  check_idle_and_clean();
}

// ── the failure: the lock and the sessions drifting apart ───────────────────
//
// test_nvs_session_depth's long run, on the real code. Three tasks, a
// deterministic mix of begins (either mode, about 1 in 16 Preferences
// failures injected) and ends, and after every call: only the lock's owner
// holds sessions and the lock's count is how many; the handle the manager
// reports is the one the fake has, in the same mode; each call made exactly
// one take, begin() with the bounded wait and end() with none; a read-write
// begin that returned true left a writable handle; and a stray end() moved
// nothing.
static void test_lock_count_tracks_sessions_across_tasks() {
  uint32_t x = 0x2545F491u;  // xorshift32, fixed seed: the run is reproducible
  auto next = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  int held[3] = {0, 0, 0};
  int begins_ok = 0, closes = 0, stray = 0, timeouts = 0;
  for (int step = 0; step < 200000; ++step) {
    const uint32_t r = next();
    const int task = (int)(r % 3u);
    const bool do_begin = ((r >> 2) % 5u) < (held[task] == 0 ? 3u : 2u);
    const int takes_before = lock().takes;
    if (do_begin) {
      const bool ro = ((r >> 5) & 1u) != 0;
      const bool other_holds = lock().owner >= 0 && lock().owner != task;
      handle().fail_next = ((r >> 6) % 16u) == 0;
      const bool ok = begin_on(task, ro);
      handle().fail_next = false;
      CHECK(lock().takes == takes_before + 1);
      CHECK(lock().last_wait == pdMS_TO_TICKS(kSessionWaitMs));
      if (other_holds) {
        CHECK(!ok);
        timeouts++;
      }
      if (ok) {
        held[task]++;
        begins_ok++;
        if (!ro) CHECK(handle().started && !handle().read_only);
      }
    } else if (held[task] == 0) {
      const int owner = lock().owner, count = lock().count;
      const bool started = handle().started, read_only = handle().read_only;
      end_on(task);
      stray++;
      CHECK(lock().takes == takes_before + 1);
      CHECK(lock().owner == owner && lock().count == count);
      CHECK(handle().started == started && handle().read_only == read_only);
    } else {
      const bool was_started = handle().started;
      end_on(task);
      held[task]--;
      CHECK(lock().takes == takes_before + 1);
      if (was_started && !handle().started) closes++;
    }
    CHECK(lock().last_wait == (do_begin ? pdMS_TO_TICKS(kSessionWaitMs) : 0u));
    for (int t = 0; t < 3; ++t) {
      if (t != lock().owner) CHECK(held[t] == 0);
    }
    CHECK(lock().count == (lock().owner >= 0 ? held[lock().owner] : 0));
    CHECK((lock().count == 0) == (lock().owner == -1));
    g_host_task = task;
    CHECK(nvs().isOpen() == handle().started);
    if (handle().started) CHECK(nvs().isReadOnly() == handle().read_only);
    if (g_failures > 20) break;  // one broken invariant floods; stop early
  }
  for (int t = 0; t < 3; ++t) {
    while (held[t] > 0) { end_on(t); held[t]--; }
  }
  check_idle_and_clean();
  CHECK(Serial.lines == 1);        // every timeout since the first stayed quiet
  CHECK(begins_ok > 10000);        // the mix really exercised sessions,
  CHECK(closes > 1000);            // really closed them,
  CHECK(stray > 1000);             // sent ends from tasks without one,
  CHECK(timeouts > 1000);          // and really contended
}

// ── the unlocked path: mutex creation failed ────────────────────────────────
//
// With no mutex (heap exhaustion at first use) NvsManager proceeds unlocked,
// the pre-lock behavior, as the camera lifecycle lock does. It must never
// hand FreeRTOS the null handle, and on one task the depth still decides the
// handle: the reopen, and only the outermost end() closing.
static void test_null_lock_proceeds_unlocked() {
  g_host_mutex_create_fails = true;
  g_want_unlocked = true;
  NvsManager& u = NvsManager::instance();
  g_host_mutex_create_fails = false;
  CHECK(g_host_prefs_count == 2);
  Preferences& h = *g_host_prefs[1];

  g_host_task = 0;
  CHECK(u.begin(true));
  CHECK(u.begin(false));
  CHECK(h.started && !h.read_only);
  u.end();
  CHECK(h.started);
  u.end();
  CHECK(!h.started);
  u.end();                               // no session: nothing to do
  CHECK(!h.started);
  CHECK(g_host_null_handle_calls == 0);
  CHECK(h.double_begins == 0 && h.ends_while_closed == 0 && h.wrong_namespace == 0);
  g_want_unlocked = false;
}

int main() {
  g_host_prefs_namespace = NVS_MAIN_NS;
  test_constructor_creates_the_lock();
  test_one_task_cannot_close_anothers_session();
  test_nesting_on_one_task();
  test_failed_begin_gives_the_lock_back();
  test_depth_is_bounded();
  test_lock_count_tracks_sessions_across_tasks();
  test_null_lock_proceeds_unlocked();

  if (g_failures == 0) { std::printf("ALL nvs-manager-lock tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
