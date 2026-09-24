/* Host tests for canary-wap's NvsManager session lock (sweep F53), run on the
 * sketch's own code: the header-only NvsManager, the nvs_store:: helpers and
 * the legacy nvs_open_rw()/nvs_open_ro()/nvs_close() wrappers in
 * arduino/canary_wap/nvs_store.h, included here as the firmware includes
 * them, over the staged nvs_session_depth.h.
 *
 * Five tasks open sessions on the WAP's one settings handle: the loop task
 * (setup() and loop()), the httpd task serving the API, the NimBLE host task,
 * the Bluetooth bring-up task and the QR-scan task. Before the lock the
 * handle carried a bare open flag, so a session ending on one task closed it
 * under another and a write racing that end() landed nothing. The canary had
 * the same shape (F52); its NvsManager suite
 * (firmware/tests_host/test_nvs_manager_lock.cpp) runs these scenarios on
 * securacv_crypto.cpp's begin()/end(), and this one runs them on the WAP's.
 * Both use the same fake recursive mutex (firmware/tests_host/stubs/
 * nvs_manager/freertos): a take by the owner nests, a take by any other task
 * while it is held fails at once (the host cannot block, so every contended
 * wait here is one that ran out), and a give by a task that does not hold it
 * is counted as a fault. The fake Preferences (stubs/nvs_store) refuses a
 * begin on a started handle, as the real one does, and counts a get or put
 * made with the handle closed.
 *
 * Built twice: once as the firmware runs (the lock is created), and once with
 * -DNVS_STORE_TEST_NULL_LOCK, where the first instance() meets a mutex
 * creation that fails and NvsManager must proceed unlocked. The singleton is
 * the only way in, so one binary cannot hold both.
 *
 * What this does not prove: FreeRTOS's own mutex, a wait that ends because
 * the holder finished, or anything on a device. The WAP's PlatformIO and
 * Arduino CLI legs compile the real thing in CI; it has not run on a bench.
 *
 * Build & run: make -C firmware/projects/canary-wap/tests_host
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "nvs_store.h"  // the sketch's own header, over the stubs

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

// The WAP's tasks, as g_host_task names them.
enum : int { LOOP = 0, HTTPD = 1, NIMBLE = 2, BT_BRINGUP = 3, QR_SCAN = 4, kTasks = 5 };

static Preferences& handle() { return *g_host_prefs[0]; }

// The lock covers the "securacv" namespace only: NvsSession opens its own
// handle on a module namespace (chirp, mesh), which never shares
// NvsManager's.
static void check_namespaces() {
  CHECK(std::strcmp(NVS_MAIN_NS, "securacv") == 0);
  CHECK(std::strcmp(NVS_MAIN_NS, NVS_CHIRP_NS) != 0);
  CHECK(std::strcmp(NVS_MAIN_NS, NVS_MESH_NS) != 0);
}

#ifndef NVS_STORE_TEST_NULL_LOCK

static NvsManager& nvs() { return NvsManager::instance(); }

static HostRecursiveMutex& lock() { return g_host_mutexes[0]; }

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
  CHECK(handle().ops_while_closed == 0);
  CHECK(handle().puts_while_read_only == 0);
  CHECK(g_host_null_handle_calls == 0);
}

// ── the constructor makes the one recursive mutex ───────────────────────────
//
// The stub offers only the recursive create, so a non-recursive mutex does
// not compile here; this pins that the first instance() made exactly one, and
// that NvsManager's handle is the only Preferences built so far.
static void test_constructor_creates_the_lock() {
  (void)nvs();
  CHECK(g_host_mutexes_created == 1);
  CHECK(g_host_prefs_count == 1);
  check_namespaces();
  check_idle_and_clean();
}

// ── the failure the fix exists for: one task closing another's session ─────
//
// The httpd task is reading the Wi-Fi credentials (wifi_load_credentials's
// shape, a read-only session); a bonded phone's pairing record arrives on the
// NimBLE host task and its write session's wait runs out; a stray end() from
// the NimBLE task must not touch the handle. The timeout is said once per
// boot, begin() asks for kSessionWaitMs, and end() never waits.
static void test_one_task_cannot_close_anothers_session() {
  CHECK(begin_on(HTTPD, true));
  CHECK(lock().last_wait == pdMS_TO_TICKS(kSessionWaitMs));
  CHECK(handle().started && handle().read_only);
  CHECK(lock().owner == HTTPD && lock().count == 1);

  CHECK(Serial.lines == 0);
  CHECK(!begin_on(NIMBLE, false));       // held by the httpd task: fail soft
  CHECK(Serial.lines == 1);
  CHECK(std::strstr(Serial.last, "[NVS] session wait timed out") != nullptr);

  end_on(NIMBLE);                        // a stray end() from the NimBLE task
  CHECK(lock().last_wait == 0);          // end() never waits
  CHECK(handle().started);               // the httpd task's handle is still open
  CHECK(lock().owner == HTTPD && lock().count == 1);

  CHECK(!begin_on(QR_SCAN, false));      // a second timeout (the QR-scan save)...
  CHECK(Serial.lines == 1);              // ...is not reported again

  end_on(HTTPD);
  CHECK(!handle().started);
  CHECK(lock().count == 0);
  CHECK(begin_on(NIMBLE, false));        // now the pairing record's write opens
  CHECK(handle().started && !handle().read_only);
  CHECK(nvs().isOpen() && !nvs().isReadOnly());
  end_on(NIMBLE);

  end_on(LOOP);                          // an end() with no session anywhere
  check_idle_and_clean();
}

// ── the failure: a nested end() closing the outer handle, or a nested write
//    handed a read-only one ────────────────────────────────────────────────
//
// No WAP caller nests today (the vault's clear_pubkey() ends its own session
// before persist_config() opens one, sweep F53), but the lock makes nesting a
// question: read-only, then read-write inside it (the real reopen: end,
// then begin read-write — never a begin on a started handle), then read-only
// inside that (keeps the read-write handle). Only the outermost end() closes,
// and the lock's count follows the depth down.
static void test_nesting_on_one_task() {
  CHECK(begin_on(LOOP, true));
  CHECK(begin_on(LOOP, false));
  CHECK(handle().started && !handle().read_only);
  CHECK(begin_on(LOOP, true));
  CHECK(!handle().read_only);
  CHECK(lock().count == 3);
  end_on(LOOP);
  CHECK(handle().started && lock().count == 2);
  end_on(LOOP);
  CHECK(handle().started && !handle().read_only && lock().count == 1);
  end_on(LOOP);
  check_idle_and_clean();
}

// ── the failure: a failed begin keeping the lock ────────────────────────────
//
// A begin() that returns false owes no end() — no caller makes one — so it
// must give back the take it made, or every other task's begin stalls behind
// a lock nobody holds a session under. vault_snapshot::init() leans on this
// at boot: on a factory-fresh unit its read-only open fails (the namespace
// does not exist yet) and it falls back to read-write. Also the read-write
// reopen inside a read-only session that fails (the handle is then gone, and
// the next begin opens it again).
static void test_failed_begin_gives_the_lock_back() {
  g_host_task = LOOP;
  handle().fail_next = true;             // vault init: the read-only open fails...
  CHECK(!nvs().beginReadOnly());
  CHECK(lock().count == 0 && lock().owner == -1);
  CHECK(nvs().beginReadWrite());         // ...and the read-write fallback opens
  CHECK(lock().owner == LOOP && lock().count == 1);
  end_on(LOOP);
  check_idle_and_clean();

  handle().fail_next = true;
  CHECK(!begin_on(LOOP, true));
  CHECK(begin_on(HTTPD, false));         // another task is not locked out
  end_on(HTTPD);
  check_idle_and_clean();

  CHECK(begin_on(LOOP, true));
  handle().fail_next = true;
  CHECK(!begin_on(LOOP, false));         // the reopen failed
  CHECK(lock().count == 1);              // only the outer session's take
  CHECK(!handle().started && !nvs().isOpen());
  CHECK(begin_on(LOOP, true));           // opens again, not "keeps" nothing
  CHECK(handle().started);
  end_on(LOOP);
  CHECK(handle().started);               // the outer session's handle
  end_on(LOOP);
  check_idle_and_clean();
}

// ── the failure: a runaway nesting wrapping the count ───────────────────────
static void test_depth_is_bounded() {
  for (int i = 0; i < kMaxDepth; ++i) CHECK(begin_on(LOOP, (i % 2) != 0));
  CHECK(lock().count == kMaxDepth);
  CHECK(!begin_on(LOOP, true));          // refused, and its take given back
  CHECK(lock().count == kMaxDepth);
  for (int i = 0; i < kMaxDepth; ++i) end_on(LOOP);
  check_idle_and_clean();
}

// ── the failure: a helper leaving its session open ──────────────────────────
//
// Most of the WAP's sessions go through nvs_store::'s four helpers (baseline,
// familiar, household, notify, presence_context, rf_presence, wizard). Each
// is one session on the calling task; each must end it on every path —
// success, a failed open, a blob of the wrong length — or the lock stays on
// that task. Values written in one session read back in the next.
static void test_nvs_store_helpers_close_their_sessions() {
  g_host_task = HTTPD;
  CHECK(nvs_store::set_u32("rf_epoch", 7u));
  check_idle_and_clean();
  CHECK(nvs_store::get_u32("rf_epoch", 0u) == 7u);
  check_idle_and_clean();
  CHECK(nvs_store::get_u32("absent", 41u) == 41u);
  check_idle_and_clean();

  const uint8_t blob[5] = {1, 2, 3, 4, 5};
  uint8_t back[5] = {0};
  CHECK(nvs_store::set_blob("rf_settings", blob, sizeof(blob)));
  check_idle_and_clean();
  CHECK(nvs_store::get_blob("rf_settings", back, sizeof(back)));
  CHECK(std::memcmp(blob, back, sizeof(blob)) == 0);
  check_idle_and_clean();

  uint8_t wrong[4] = {0};
  CHECK(!nvs_store::get_blob("rf_settings", wrong, sizeof(wrong)));  // length differs
  check_idle_and_clean();
  CHECK(!nvs_store::get_blob("rf_epoch", back, sizeof(back)));        // a u32, not a blob
  check_idle_and_clean();

  handle().fail_next = true;
  CHECK(!nvs_store::set_u32("rf_epoch", 9u));   // the open failed: no write
  check_idle_and_clean();
  handle().fail_next = true;
  CHECK(nvs_store::get_u32("rf_epoch", 99u) == 99u);
  check_idle_and_clean();
  CHECK(nvs_store::get_u32("rf_epoch", 0u) == 7u);
  check_idle_and_clean();
}

// ── the failure: a helper writing into another task's session ───────────────
//
// With the bare flag, a helper on the loop found the handle "already open"
// under the httpd task's session and wrote through it, and either task's
// end() then closed it under the other. Now the loop's helpers wait, fail
// soft (set_u32 and set_blob report false, the reads give the default) and
// write nothing, and the httpd task's session is untouched; once it ends the
// same calls land.
static void test_nvs_store_helpers_wait_for_another_tasks_session() {
  CHECK(begin_on(HTTPD, true));
  const int puts_before = handle().puts;
  const uint8_t blob[3] = {9, 8, 7};
  uint8_t back[3] = {0};

  g_host_task = LOOP;
  CHECK(!nvs_store::set_u32("fam_rot", 5u));
  CHECK(nvs_store::get_u32("fam_rot", 123u) == 123u);
  CHECK(!nvs_store::set_blob("fam_salt", blob, sizeof(blob)));
  CHECK(!nvs_store::get_blob("fam_salt", back, sizeof(back)));
  CHECK(handle().puts == puts_before);   // nothing landed in the httpd session
  CHECK(handle().started && handle().read_only);
  CHECK(lock().owner == HTTPD && lock().count == 1);

  end_on(HTTPD);
  g_host_task = LOOP;
  CHECK(nvs_store::set_u32("fam_rot", 5u));
  CHECK(nvs_store::set_blob("fam_salt", blob, sizeof(blob)));
  CHECK(nvs_store::get_u32("fam_rot", 0u) == 5u);
  CHECK(nvs_store::get_blob("fam_salt", back, sizeof(back)));
  CHECK(std::memcmp(blob, back, sizeof(blob)) == 0);
  check_idle_and_clean();
}

// ── the legacy wrappers take the same lock ──────────────────────────────────
//
// nvs_open_rw() / nvs_open_ro() / nvs_close() delegate to the same singleton,
// so a session opened through them is serialized like any other.
static void test_legacy_wrappers_take_the_same_lock() {
  g_host_task = LOOP;
  CHECK(nvs_open_rw());
  CHECK(lock().owner == LOOP && handle().started && !handle().read_only);
  g_host_task = HTTPD;
  CHECK(!nvs_open_ro());                 // waits for the loop's session, fails soft
  nvs_close();                           // not the httpd task's session to close
  CHECK(handle().started);
  g_host_task = LOOP;
  nvs_close();
  check_idle_and_clean();
}

// ── the failure: the lock and the sessions drifting apart ───────────────────
//
// test_nvs_manager_lock's long run, on the WAP's code and its five tasks. A
// deterministic mix of begins (either mode, about 1 in 16 Preferences
// failures injected) and ends, and after every call: only the lock's owner
// holds sessions and the lock's count is how many; the handle the manager
// reports is the one the fake has, in the same mode; each call made exactly
// one take, begin() with the bounded wait and end() with none; a read-write
// begin that returned true left a writable handle; and a stray end() moved
// nothing.
static void test_lock_count_tracks_sessions_across_tasks() {
  uint32_t x = 0x9E3779B9u;  // xorshift32, fixed seed: the run is reproducible
  auto next = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  int held[kTasks] = {0, 0, 0, 0, 0};
  int begins_ok = 0, closes = 0, stray = 0, timeouts = 0;
  for (int step = 0; step < 250000; ++step) {
    const uint32_t r = next();
    const int task = (int)(r % (uint32_t)kTasks);
    const bool do_begin = ((r >> 3) % 5u) < (held[task] == 0 ? 3u : 2u);
    const int takes_before = lock().takes;
    if (do_begin) {
      const bool ro = ((r >> 6) & 1u) != 0;
      const bool other_holds = lock().owner >= 0 && lock().owner != task;
      handle().fail_next = ((r >> 7) % 16u) == 0;
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
      CHECK(lock().last_wait == 0u);
      CHECK(lock().owner == owner && lock().count == count);
      CHECK(handle().started == started && handle().read_only == read_only);
    } else {
      const bool was_started = handle().started;
      end_on(task);
      held[task]--;
      CHECK(lock().takes == takes_before + 1);
      CHECK(lock().last_wait == 0u);
      if (was_started && !handle().started) closes++;
    }
    for (int t = 0; t < kTasks; ++t) {
      if (t != lock().owner) CHECK(held[t] == 0);
    }
    CHECK(lock().count == (lock().owner >= 0 ? held[lock().owner] : 0));
    CHECK((lock().count == 0) == (lock().owner == -1));
    g_host_task = task;
    CHECK(nvs().isOpen() == handle().started);
    if (handle().started) CHECK(nvs().isReadOnly() == handle().read_only);
    if (g_failures > 20) break;  // one broken invariant floods; stop early
  }
  for (int t = 0; t < kTasks; ++t) {
    while (held[t] > 0) { end_on(t); held[t]--; }
  }
  check_idle_and_clean();
  CHECK(Serial.lines == 1);        // every timeout since the first stayed quiet
  CHECK(begins_ok > 10000);        // the mix really exercised sessions,
  CHECK(closes > 1000);            // really closed them,
  CHECK(stray > 1000);             // sent ends from tasks without one,
  CHECK(timeouts > 1000);          // and really contended
}

// ── the wait sits under the loop's task watchdog ────────────────────────────
//
// canary_wap.ino's static_assert holds kSessionWaitMs under the loop's
// WATCHDOG_TIMEOUT_SEC, but only CI's firmware legs compile the .ino. So the
// suite reads the sketch (its path is passed in, so a moved file fails
// closed): the constant it declares, the arithmetic, and that the
// static_assert is still there as code rather than a comment.
static void test_wait_sits_under_the_loop_watchdog() {
  std::ifstream in(CANARY_WAP_INO);
  CHECK(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string ino = ss.str();

  const std::string decl = "static const uint32_t WATCHDOG_TIMEOUT_SEC = ";
  const size_t d = ino.find(decl);
  CHECK(d != std::string::npos);
  CHECK(ino.find(decl, d + 1) == std::string::npos);
  unsigned long watchdog_s = 0;
  if (d != std::string::npos) watchdog_s = std::strtoul(ino.c_str() + d + decl.size(), nullptr, 10);
  CHECK(watchdog_s > 0);
  CHECK((unsigned long)kSessionWaitMs < watchdog_s * 1000ul);

  const std::string guard =
      "\nstatic_assert(nvs_session::kSessionWaitMs < WATCHDOG_TIMEOUT_SEC * 1000u,";
  const size_t g = ino.find(guard);
  CHECK(g != std::string::npos);
  CHECK(g == std::string::npos || g > d);  // after the constant it reads
}

int main() {
  g_host_prefs_namespace = NVS_MAIN_NS;
  test_constructor_creates_the_lock();
  test_one_task_cannot_close_anothers_session();
  test_nesting_on_one_task();
  test_failed_begin_gives_the_lock_back();
  test_depth_is_bounded();
  test_nvs_store_helpers_close_their_sessions();
  test_nvs_store_helpers_wait_for_another_tasks_session();
  test_legacy_wrappers_take_the_same_lock();
  test_lock_count_tracks_sessions_across_tasks();
  test_wait_sits_under_the_loop_watchdog();

  if (g_failures == 0) { std::printf("ALL nvs-store-lock tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}

#else  // NVS_STORE_TEST_NULL_LOCK

// ── the unlocked path: mutex creation failed ────────────────────────────────
//
// With no mutex (heap exhaustion at the first instance()) NvsManager proceeds
// unlocked, the pre-lock behavior, as the canary's does. It must never hand
// FreeRTOS the null handle, and on one task the depth still decides the
// handle: the reopen, only the outermost end() closing, and a helper's session
// opening and closing.
int main() {
  g_host_prefs_namespace = NVS_MAIN_NS;
  g_host_mutex_create_fails = true;
  NvsManager& u = NvsManager::instance();
  g_host_mutex_create_fails = false;
  CHECK(g_host_mutexes_created == 0);
  CHECK(g_host_prefs_count == 1);
  check_namespaces();
  Preferences& h = handle();

  g_host_task = LOOP;
  CHECK(u.begin(true));
  CHECK(u.begin(false));
  CHECK(h.started && !h.read_only);
  u.end();
  CHECK(h.started);
  u.end();
  CHECK(!h.started);
  u.end();                               // no session: nothing to do
  CHECK(!h.started);

  CHECK(nvs_store::set_u32("boots", 3u));
  CHECK(!h.started);
  CHECK(nvs_store::get_u32("boots", 0u) == 3u);
  CHECK(!h.started);

  CHECK(g_host_null_handle_calls == 0);
  CHECK(h.double_begins == 0 && h.ends_while_closed == 0 && h.wrong_namespace == 0);
  CHECK(h.ops_while_closed == 0 && h.puts_while_read_only == 0);
  CHECK(Serial.lines == 0);

  if (g_failures == 0) { std::printf("ALL nvs-store-lock (null lock) tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}

#endif  // NVS_STORE_TEST_NULL_LOCK
