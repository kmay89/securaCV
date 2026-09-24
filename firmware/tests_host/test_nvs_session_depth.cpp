/* Host tests for NvsManager's session depth
 * (firmware/canary/lib/securacv_crypto/src/nvs_session_depth.h).
 *
 * NvsManager is the canary's one shared Preferences handle. Three tasks open
 * sessions on it after setup (the loop, the httpd task serving the API, the
 * pull-OTA task), and with a bare open flag a session ending on one task
 * closed the handle under another: a status poll during an MQTT reload could
 * read the broker host as empty and leave MQTT off, and a write racing that
 * end() could land nothing while nvs_store_bytes() still returned true. The
 * fix holds a recursive FreeRTOS mutex from begin() to the matching end() and
 * counts nested sessions on the holding task. Every test here is about a way
 * that count could still let a handle close under a session that is using
 * it, hand a write a read-only handle, or leave the lock out of step with the
 * sessions — so the suite is organized by the failure prevented.
 *
 * What this proves and what it does not. The header is pure arithmetic and is
 * proved here directly. The last two tests drive it through a MODEL of
 * NvsManager::begin()/end(): the same calls in the same order as
 * securacv_crypto.cpp, with Preferences replaced by a fake that refuses a
 * double begin, and the FreeRTOS recursive mutex by a counter with an owner.
 * The model is not the firmware: test_nvs_manager_lock.cpp runs the same
 * scenarios on securacv_crypto.cpp's own begin()/end(), cut out of the file
 * by the Makefile. The real FreeRTOS mutex is compile-tested by CI's canary
 * envs and has not run on a bench.
 *
 * Build & run (via firmware/tests_host/Makefile, mirrors the CI contract):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I ../canary/lib/securacv_crypto/src \
 *       -I ../canary/include test_nvs_session_depth.cpp
 */
#include <cstdint>
#include <cstdio>

#include "canary_config.h"  // WATCHDOG_TIMEOUT_SEC — the loop's task watchdog
#include "nvs_session_depth.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

using nvs_session::Begin;
using nvs_session::End;
using nvs_session::State;
using nvs_session::commit_begin;
using nvs_session::end_gives;
using nvs_session::kMaxDepth;
using nvs_session::kSessionWaitMs;
using nvs_session::on_begin;
using nvs_session::on_end;

// One successful begin, the way NvsManager does it: decide, then commit ok.
static Begin begin_ok(State& s, bool ro) {
  const Begin a = on_begin(s, ro);
  CHECK(a != Begin::Refuse);
  CHECK(commit_begin(s, a, ro, true));
  return a;
}

// ── the failure: a nested end() closing the outer session's handle ──────────
//
// The count's whole job. With the lock held from begin() to end(), a helper
// that opens a session inside another session on the same task no longer
// deadlocks (the mutex is recursive), but its end() must not close the handle
// the outer session goes on reading or writing. Every mix of modes: only the
// outermost end() closes.
static void test_nested_end_keeps_the_outer_handle() {
  for (int outer = 0; outer < 2; ++outer) {
    for (int inner = 0; inner < 2; ++inner) {
      State s;
      CHECK(begin_ok(s, outer != 0) == Begin::Open);
      begin_ok(s, inner != 0);
      CHECK(s.depth == 2);
      CHECK(on_end(s) == End::Keep);
      CHECK(s.depth == 1);
      CHECK(s.open);
      CHECK(on_end(s) == End::Close);
      CHECK(s.depth == 0);
      CHECK(!s.open && !s.read_only);
    }
  }
}

// ── the failure: a nested write handed a read-only handle ───────────────────
//
// Preferences opened read-only refuses every put. A read-write session inside
// a read-only one must reopen the handle read-write (ReopenRw), or its writes
// fail silently; and the handle stays read-write until the outer end() —
// reopening it read-only under the outer session would be another close under
// a live session. A read-only session inside a read-write one keeps the
// read-write handle as it is (Keep): it only reads.
static void test_rw_inside_ro_reopens_rw() {
  State s;
  CHECK(begin_ok(s, true) == Begin::Open);
  CHECK(s.read_only);
  CHECK(begin_ok(s, false) == Begin::ReopenRw);
  CHECK(s.open && !s.read_only);
  CHECK(on_end(s) == End::Keep);
  CHECK(s.open && !s.read_only);   // still writable for the rest of the outer session
  CHECK(on_end(s) == End::Close);

  State w;
  begin_ok(w, false);
  CHECK(begin_ok(w, true) == Begin::Keep);
  CHECK(w.open && !w.read_only);   // RO inside RW keeps the RW handle
  CHECK(begin_ok(w, false) == Begin::Keep);
  CHECK(on_end(w) == End::Keep);
  CHECK(on_end(w) == End::Keep);
  CHECK(on_end(w) == End::Close);
}

// ── the failure: an end() with no session closing something ─────────────────
//
// An end() from a task with no session is a no-op. At depth 0 the arithmetic
// says Unbalanced and changes nothing, and end() gives the lock back only
// once — for its own zero-wait take — never a second time for a begin that
// never kept one. A second end() after the session closed is the same no-op.
static void test_end_without_a_session_is_a_noop() {
  State s;
  CHECK(on_end(s) == End::Unbalanced);
  CHECK(s.depth == 0 && !s.open);
  CHECK(end_gives(End::Unbalanced) == 1);

  begin_ok(s, true);
  CHECK(on_end(s) == End::Close);
  CHECK(on_end(s) == End::Unbalanced);
  CHECK(s.depth == 0 && !s.open);
}

// ── the failure: a failed begin counted as a session ────────────────────────
//
// A begin that returns false must add no session: the caller that got false
// never calls end() (none does), so a counted failure would hold the lock
// forever and stall every other task's begin. A failed first open leaves the
// handle closed; a failed nested reopen has already ended the outer handle,
// so it is marked closed rather than kept.
static void test_failed_begin_adds_no_session() {
  State s;
  Begin a = on_begin(s, true);
  CHECK(a == Begin::Open);
  CHECK(!commit_begin(s, a, true, false));
  CHECK(s.depth == 0 && !s.open);
  CHECK(on_end(s) == End::Unbalanced);

  State n;
  begin_ok(n, true);
  a = on_begin(n, false);
  CHECK(a == Begin::ReopenRw);
  CHECK(!commit_begin(n, a, false, false));
  CHECK(n.depth == 1);
  CHECK(!n.open);                  // Preferences::end() already ran for the reopen
}

// ── the failure: keeping a handle a failed reopen closed ────────────────────
//
// After a failed ReopenRw the outer session is still counted but its handle is
// gone. The next begin at any depth, in either mode, must OPEN it again —
// Keep would hand the caller a closed handle whose every get returns the
// default (the empty broker host again). And the outer end() then has nothing
// to close: Keep, not a second Preferences::end().
static void test_begin_after_a_failed_reopen_opens_again() {
  for (int mode = 0; mode < 2; ++mode) {
    State s;
    begin_ok(s, true);
    const Begin r = on_begin(s, false);
    commit_begin(s, r, false, false);
    CHECK(s.depth == 1 && !s.open);

    const Begin again = on_begin(s, mode != 0);
    CHECK(again == Begin::Open);
    CHECK(commit_begin(s, again, mode != 0, true));
    CHECK(s.depth == 2 && s.open && s.read_only == (mode != 0));
    CHECK(on_end(s) == End::Keep);
    CHECK(on_end(s) == End::Close);
  }

  State k;
  begin_ok(k, true);
  const Begin r = on_begin(k, false);
  commit_begin(k, r, false, false);
  CHECK(on_end(k) == End::Keep);   // counted, but nothing left to close
  CHECK(k.depth == 0);
  CHECK(end_gives(End::Keep) == 2);  // ...and the outer begin's take still goes back
}

// ── the failure: a runaway nesting wrapping the count ───────────────────────
//
// The depth is a uint8_t. Past kMaxDepth a begin is refused (false, no state
// change, the lock given back) instead of wrapping to 0 and letting the next
// end() close a handle eight sessions deep. Unwinding closes exactly once.
static void test_depth_is_bounded() {
  State s;
  for (int i = 0; i < kMaxDepth; ++i) begin_ok(s, (i % 2) != 0);
  CHECK(s.depth == kMaxDepth);
  const State before = s;
  const Begin a = on_begin(s, true);
  CHECK(a == Begin::Refuse);
  CHECK(!commit_begin(s, a, true, true));
  CHECK(s.depth == before.depth && s.open == before.open && s.read_only == before.read_only);
  int closes = 0;
  for (int i = 0; i < kMaxDepth; ++i) {
    if (on_end(s) == End::Close) closes++;
  }
  CHECK(closes == 1);
  CHECK(s.depth == 0);
}

// ── the failure: a wait that outlasts the watchdog ──────────────────────────
//
// begin() waits at most kSessionWaitMs for another task's session and then
// fails soft. The loop task is subscribed to the WATCHDOG_TIMEOUT_SEC task
// watchdog, so the wait must sit well under it (securacv_crypto.cpp repeats
// this as a static_assert on the device build). Two sequential waits — the
// MQTT reload's two reads — still fit.
static void test_wait_is_under_the_loop_watchdog() {
  CHECK(kSessionWaitMs > 0);
  CHECK(2u * kSessionWaitMs < (uint32_t)WATCHDOG_TIMEOUT_SEC * 1000u);
}

// ═══ The model: NvsManager::begin()/end() over a fake handle and lock ═══════
//
// A hand copy of securacv_crypto.cpp, call for call (test_nvs_manager_lock.cpp
// holds the real bodies to the same scenarios), with xSemaphoreTakeRecursive /
// xSemaphoreGiveRecursive replaced by FakeMutex and Preferences by FakePrefs.
// FakeMutex is a recursive mutex with an owner: a take by the owner nests, a
// take by anyone else while it is held fails (begin()'s 2 s wait running out,
// or end()'s zero-wait take), and a give by a non-owner is a failure. FakePrefs
// refuses begin() on a started handle, as Preferences::begin() does, and can
// be told to fail the next begin().

struct FakeMutex {
  int owner = -1;
  int count = 0;
  int bad_gives = 0;
  bool take(int task) {
    if (count == 0) { owner = task; count = 1; return true; }
    if (owner == task) { count++; return true; }
    return false;
  }
  void give(int task) {
    if (count == 0 || owner != task) { bad_gives++; return; }
    if (--count == 0) owner = -1;
  }
};

struct FakePrefs {
  bool started = false;
  bool read_only = false;
  bool fail_next = false;
  int double_begins = 0;
  bool begin(bool ro) {
    if (started) { double_begins++; return false; }
    if (fail_next) { fail_next = false; return false; }
    started = true;
    read_only = ro;
    return true;
  }
  void end() { started = false; read_only = false; }
};

struct Model {
  State st;
  FakeMutex mu;
  FakePrefs prefs;

  bool begin(int task, bool ro) {
    if (!mu.take(task)) return false;             // the bounded wait ran out
    const Begin a = on_begin(st, ro);
    bool ok = true;
    if (a == Begin::Open) {
      ok = prefs.begin(ro);
    } else if (a == Begin::ReopenRw) {
      prefs.end();
      ok = prefs.begin(false);
    }
    if (!commit_begin(st, a, ro, ok)) {
      mu.give(task);
      return false;
    }
    return true;                                  // the lock stays taken
  }

  void end(int task) {
    if (!mu.take(task)) return;                   // another task's session
    const End e = on_end(st);
    if (e == End::Close) prefs.end();
    for (uint8_t i = 0; i < end_gives(e); ++i) mu.give(task);
  }
};

// ── the failure the fix exists for: one task closing another's session ─────
//
// The race the wave-7 security scout found (its finding 4), replayed against
// the model. The httpd task (1) is reading the broker row; the loop (0) tries
// to start a session of its own and must wait (here: its wait runs out and it
// fails soft), and a stray end() from the loop must not touch the handle. The
// httpd task's reads keep a started handle to the end of its session; only
// then does the loop's next begin open it.
static void test_one_task_cannot_close_anothers_session() {
  Model m;
  CHECK(m.begin(1, true));
  CHECK(m.prefs.started);

  CHECK(!m.begin(0, true));        // held by the httpd task: wait, then fail soft
  m.end(0);                        // a stray end() from a task with no session
  CHECK(m.prefs.started);          // the httpd task's handle is still open
  CHECK(m.st.depth == 1 && m.mu.owner == 1);

  m.end(1);
  CHECK(!m.prefs.started);
  CHECK(m.mu.count == 0);
  CHECK(m.begin(0, false));        // now the loop's write session opens
  CHECK(m.prefs.started && !m.prefs.read_only);
  m.end(0);
  CHECK(m.mu.bad_gives == 0 && m.prefs.double_begins == 0);

  // With no session anywhere, an end() is a no-op that leaves the lock free.
  Model idle;
  idle.end(2);
  CHECK(idle.mu.count == 0 && idle.st.depth == 0 && idle.mu.bad_gives == 0);
}

// ── the failure: the lock and the sessions drifting apart ───────────────────
//
// end()'s zero-wait take tells "my session" from "another task's session" only
// while the lock is free exactly when the depth is 0. Drive the model with a
// long deterministic mix of begins (either mode, with injected Preferences
// failures) and ends on three tasks, and hold after every call: the lock's
// count IS the depth; the handle the arithmetic calls open is the one the fake
// has open, in the same mode; Preferences::begin() never lands on a started
// handle; no task ever gives a lock it does not hold; and a read-write begin
// that returned true always leaves a writable handle.
static void test_lock_count_tracks_depth_across_tasks() {
  Model m;
  uint32_t x = 0x2545F491u;  // xorshift32, fixed seed: the run is reproducible
  auto next = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  int held[3] = {0, 0, 0};   // sessions each task believes it holds
  int begins_ok = 0, ends_that_closed = 0, stray_ends = 0;
  for (int step = 0; step < 200000; ++step) {
    const uint32_t r = next();
    const int task = (int)(r % 3u);
    // Begins win 3 in 5 for a task with no session, 2 in 5 for one inside a
    // session, so nests stay shallow and close often.
    const bool do_begin = ((r >> 2) % 5u) < (held[task] == 0 ? 3u : 2u);
    if (do_begin) {
      const bool ro = ((r >> 5) & 1u) != 0;
      m.prefs.fail_next = ((r >> 6) % 16u) == 0;  // ~1 in 16 Preferences::begin() fails
      const bool ok = m.begin(task, ro);
      m.prefs.fail_next = false;
      if (ok) {
        held[task]++;
        begins_ok++;
        if (!ro) CHECK(m.prefs.started && !m.prefs.read_only);
      }
    } else if (held[task] == 0) {
      // A stray end() from a task with no session: nothing may move.
      const State before = m.st;
      const FakeMutex lock_before = m.mu;
      const bool started_before = m.prefs.started;
      m.end(task);
      stray_ends++;
      CHECK(m.st.depth == before.depth && m.st.open == before.open &&
            m.st.read_only == before.read_only);
      CHECK(m.mu.owner == lock_before.owner && m.mu.count == lock_before.count);
      CHECK(m.prefs.started == started_before);
    } else {
      const bool was_started = m.prefs.started;
      m.end(task);
      held[task]--;
      if (was_started && !m.prefs.started) ends_that_closed++;
    }
    // Only the owner holds sessions, and its count is the lock's count.
    CHECK(m.mu.count == (int)m.st.depth);
    CHECK((m.mu.count == 0) == (m.mu.owner == -1));
    for (int t = 0; t < 3; ++t) {
      if (t != m.mu.owner) CHECK(held[t] == 0);
    }
    if (m.mu.owner >= 0) CHECK(held[m.mu.owner] == (int)m.st.depth);
    CHECK(m.st.open == m.prefs.started);
    if (m.st.open) CHECK(m.st.read_only == m.prefs.read_only);
    if (g_failures > 20) break;  // one broken invariant floods; stop early
  }
  CHECK(m.mu.bad_gives == 0);
  CHECK(m.prefs.double_begins == 0);
  CHECK(begins_ok > 10000);       // the mix really exercised sessions,
  CHECK(ends_that_closed > 1000);  // really closed them,
  CHECK(stray_ends > 1000);        // and really sent ends from tasks without one
}

int main() {
  test_nested_end_keeps_the_outer_handle();
  test_rw_inside_ro_reopens_rw();
  test_end_without_a_session_is_a_noop();
  test_failed_begin_adds_no_session();
  test_begin_after_a_failed_reopen_opens_again();
  test_depth_is_bounded();
  test_wait_is_under_the_loop_watchdog();
  test_one_task_cannot_close_anothers_session();
  test_lock_count_tracks_depth_across_tasks();

  if (g_failures == 0) { std::printf("ALL nvs-session-depth tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
