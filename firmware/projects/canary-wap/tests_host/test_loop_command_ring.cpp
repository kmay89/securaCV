// Host test: loop_command_ring.h, the hand-over between a requesting task
// (the HTTP server's) and the loop task that owns what a command changes
// (sweep F96). The REAL header, compiled as is; the device's portMUX lock is
// replaced by none (one thread) or a std::mutex (the two-thread tests).
//
// What it pins:
//   - ordering: commands run in post order, whichever slot they took, across
//     the ticket wrap;
//   - a full ring refuses a post (and submit() answers kBusy without waiting);
//   - results: each waiter collects its own command's result, once;
//   - timeouts: a command not started by the timeout is withdrawn and never
//     runs; one the loop task has started is waited for until it is done;
//   - two real threads: every submitted command runs exactly once, on the
//     loop thread, and every waiter gets its own result.

#include "loop_command_ring.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace lcr = loop_command_ring;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                        \
  } while (0)

struct NoLock {
  void lock() {}
  void unlock() {}
};

struct MutexLock {
  std::mutex m;
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
};

struct Cmd {
  uint32_t id;
  int32_t  arg;
};

struct Res {
  uint32_t id;
  int32_t  value;
};

using Ring4 = lcr::Ring<Cmd, Res, 4, NoLock>;

// ── ordering ─────────────────────────────────────────────────────────────

static void test_runs_in_post_order_and_reports_each_result() {
  std::printf("test_runs_in_post_order_and_reports_each_result\n");
  Ring4 ring;
  const uint32_t a = ring.post({1, 10});
  const uint32_t b = ring.post({2, 20});
  const uint32_t c = ring.post({3, 30});
  CHECK(a != 0 && b != 0 && c != 0 && a != b && b != c && a != c);
  CHECK(ring.poll(a, nullptr) == lcr::Poll::kQueued);
  CHECK(ring.queued() == 3);

  std::vector<uint32_t> order;
  const size_t ran = ring.drain([&](const Cmd& cmd) {
    order.push_back(cmd.id);
    return Res{cmd.id, cmd.arg * 2};
  });
  CHECK(ran == 3);
  CHECK((order == std::vector<uint32_t>{1, 2, 3}));
  CHECK(ring.queued() == 0);

  // Collected out of order, each gets its own, once.
  Res r = {};
  CHECK(ring.poll(c, &r) == lcr::Poll::kDone && r.id == 3 && r.value == 60);
  CHECK(ring.poll(a, &r) == lcr::Poll::kDone && r.id == 1 && r.value == 20);
  CHECK(ring.poll(b, &r) == lcr::Poll::kDone && r.id == 2 && r.value == 40);
  CHECK(ring.poll(a, &r) == lcr::Poll::kUnknown);
  CHECK(ring.poll(0, &r) == lcr::Poll::kUnknown);
}

// A command that takes a slot freed ahead of older ones still runs after
// them: the order is the tickets', not the slots'.
static void test_order_is_by_ticket_not_by_slot() {
  std::printf("test_order_is_by_ticket_not_by_slot\n");
  Ring4 ring;
  const uint32_t a = ring.post({1, 0});   // slot 0
  const uint32_t b = ring.post({2, 0});   // slot 1
  const uint32_t c = ring.post({3, 0});   // slot 2
  CHECK(ring.withdraw(a));                // slot 0 free again
  const uint32_t d = ring.post({4, 0});   // slot 0, the newest ticket
  CHECK(d != 0);
  std::vector<uint32_t> order;
  ring.drain([&](const Cmd& cmd) { order.push_back(cmd.id); return Res{cmd.id, 0}; });
  CHECK((order == std::vector<uint32_t>{2, 3, 4}));
  CHECK(ring.poll(b, nullptr) == lcr::Poll::kDone);
  CHECK(ring.poll(c, nullptr) == lcr::Poll::kDone);
  CHECK(ring.poll(d, nullptr) == lcr::Poll::kDone);
}

static void test_order_holds_across_the_ticket_wrap() {
  std::printf("test_order_holds_across_the_ticket_wrap\n");
  lcr::Ring<Cmd, Res, 4, NoLock> ring(0xFFFFFFFEu);
  const uint32_t a = ring.post({1, 0});
  const uint32_t b = ring.post({2, 0});
  const uint32_t c = ring.post({3, 0});
  CHECK(a == 0xFFFFFFFEu && b == 0xFFFFFFFFu && c == 1u);   // 0 is never a ticket
  // Free slot 0 and post again: the newest ticket (2) takes the first slot,
  // so slot order would run it first and plain numeric order would run b
  // last. Ticket order across the wrap is b, c, d.
  CHECK(ring.withdraw(a));
  const uint32_t d = ring.post({4, 0});
  CHECK(d == 2u);
  std::vector<uint32_t> order;
  ring.drain([&](const Cmd& cmd) { order.push_back(cmd.id); return Res{cmd.id, 0}; });
  CHECK((order == std::vector<uint32_t>{2, 3, 4}));
}

// ── a full ring ──────────────────────────────────────────────────────────

static void test_a_full_ring_refuses_and_frees_on_collect() {
  std::printf("test_a_full_ring_refuses_and_frees_on_collect\n");
  Ring4 ring;
  uint32_t t[4];
  for (uint32_t i = 0; i < 4; ++i) {
    t[i] = ring.post({i, 0});
    CHECK(t[i] != 0);
  }
  CHECK(ring.post({9, 0}) == 0);
  // A done command still holds its slot until its waiter collects it.
  ring.drain([](const Cmd& cmd) { return Res{cmd.id, 0}; });
  CHECK(ring.post({9, 0}) == 0);
  CHECK(ring.poll(t[2], nullptr) == lcr::Poll::kDone);
  const uint32_t e = ring.post({9, 0});
  CHECK(e != 0);
  CHECK(ring.post({10, 0}) == 0);

  // submit() on a full ring answers kBusy at once: no wait, no sleep.
  uint32_t clock = 0;
  unsigned sleeps = 0;
  Res r = {};
  const lcr::Wait w = lcr::submit(
      ring, Cmd{11, 0}, &r, 2000, 5, [&] { return clock; },
      [&](uint32_t ms) { clock += ms; ++sleeps; });
  CHECK(w == lcr::Wait::kBusy);
  CHECK(sleeps == 0);
}

// ── withdrawing ──────────────────────────────────────────────────────────

static void test_withdraw_only_before_the_command_starts() {
  std::printf("test_withdraw_only_before_the_command_starts\n");
  Ring4 ring;
  const uint32_t a = ring.post({1, 0});
  const uint32_t b = ring.post({2, 0});
  CHECK(ring.withdraw(a));
  CHECK(!ring.withdraw(a));                       // once
  CHECK(ring.poll(a, nullptr) == lcr::Poll::kUnknown);

  // b is withdrawn by its waiter while the loop task runs it: refused.
  bool tried = false, refused = false;
  ring.drain([&](const Cmd& cmd) {
    tried = true;
    refused = !ring.withdraw(b);
    CHECK(ring.poll(b, nullptr) == lcr::Poll::kRunning);
    return Res{cmd.id, 7};
  });
  CHECK(tried && refused);
  CHECK(!ring.withdraw(b));                       // done: too late too
  Res r = {};
  CHECK(ring.poll(b, &r) == lcr::Poll::kDone && r.value == 7);
}

// ── submit(): a waiter with a clock ──────────────────────────────────────

struct FakeTime {
  uint32_t now = 1000;
  unsigned sleeps = 0;
};

static void test_submit_returns_the_loop_tasks_result() {
  std::printf("test_submit_returns_the_loop_tasks_result\n");
  Ring4 ring;
  FakeTime t;
  int runs = 0;
  Res r = {};
  // The loop task gets the CPU on the waiter's third sleep.
  const lcr::Wait w = lcr::submit(
      ring, Cmd{5, 21}, &r, 2000, 5, [&] { return t.now; },
      [&](uint32_t ms) {
        t.now += ms;
        if (++t.sleeps == 3) {
          ring.drain([&](const Cmd& cmd) { ++runs; return Res{cmd.id, cmd.arg * 2}; });
        }
      });
  CHECK(w == lcr::Wait::kDone);
  CHECK(runs == 1);
  CHECK(r.id == 5 && r.value == 42);
  CHECK(t.sleeps == 3);
  CHECK(ring.queued() == 0);
  CHECK(ring.post({1, 0}) != 0);                  // its slot is free again
}

static void test_submit_withdraws_an_unstarted_command_at_the_timeout() {
  std::printf("test_submit_withdraws_an_unstarted_command_at_the_timeout\n");
  Ring4 ring;
  FakeTime t;
  const uint32_t start = t.now;
  Res r = {99, 99};
  const lcr::Wait w = lcr::submit(
      ring, Cmd{5, 21}, &r, 2000, 5, [&] { return t.now; },
      [&](uint32_t ms) { t.now += ms; ++t.sleeps; });   // the loop task never runs
  CHECK(w == lcr::Wait::kWithdrawn);
  CHECK(t.now - start == 2000);                   // bounded by the timeout, not more
  CHECK(r.id == 99 && r.value == 99);             // no result was written
  CHECK(ring.queued() == 0);
  // The loop task, back later, never runs it.
  int runs = 0;
  ring.drain([&](const Cmd& cmd) { ++runs; return Res{cmd.id, 0}; });
  CHECK(runs == 0);
}

// The loop task starts the command just before the timeout and is still in
// it at the timeout: the waiter keeps waiting and reports what it did.
static void test_submit_waits_for_a_command_that_has_started() {
  std::printf("test_submit_waits_for_a_command_that_has_started\n");
  using RingM = lcr::Ring<Cmd, Res, 4, MutexLock>;
  RingM ring;
  std::mutex m;
  std::condition_variable cv;
  bool started = false, release = false;
  std::atomic<uint32_t> clock{0};
  std::atomic<bool> running{false};   // the waiter's clock moves only once it has
  std::atomic<bool> stop{false};

  std::thread loop([&] {
    while (!stop.load()) {
      ring.drain([&](const Cmd& cmd) {
        {
          std::unique_lock<std::mutex> lk(m);
          started = true;
          running = true;
          cv.notify_all();
          cv.wait(lk, [&] { return release; });
        }
        return Res{cmd.id, 1};
      });
      std::this_thread::yield();
    }
  });

  lcr::Wait w = lcr::Wait::kBusy;
  Res r = {};
  std::thread waiter([&] {
    w = lcr::submit(ring, Cmd{8, 0}, &r, 100, 5, [&] { return clock.load(); },
                    [&](uint32_t ms) {
                      if (running.load()) clock += ms;
                      std::this_thread::sleep_for(std::chrono::microseconds(200));
                    });
  });

  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return started; });
  }
  // Let the waiter's clock run well past its 100 ms timeout while the
  // command runs.
  while (clock.load() < 1000) std::this_thread::sleep_for(std::chrono::microseconds(100));
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
  }
  cv.notify_all();
  waiter.join();
  stop = true;
  loop.join();
  CHECK(w == lcr::Wait::kDone);
  CHECK(r.id == 8 && r.value == 1);
}

// ── two real threads ─────────────────────────────────────────────────────

// Six requesters submit 3000 commands between them through a ring of four
// slots (so posts find it full and retry) while one loop thread drains. Each command runs once, on the loop thread, and each requester
// gets its own result back. The loop thread's counter is a plain int: only
// the loop thread touches it (a run off that thread would race, which
// -fsanitize=thread reports; `make tsan-loop-ring` builds that variant).
static void test_two_threads_every_command_runs_once_on_the_loop_thread() {
  std::printf("test_two_threads_every_command_runs_once_on_the_loop_thread\n");
  using RingM = lcr::Ring<Cmd, Res, 4, MutexLock>;
  RingM ring;
  std::atomic<bool> stop{false};
  std::thread::id loop_id;
  int loop_runs = 0;                         // loop thread only
  std::atomic<int> off_loop_runs{0};
  std::vector<uint8_t> seen(6 * 500, 0);     // loop thread only

  std::thread loop([&] {
    loop_id = std::this_thread::get_id();
    while (!stop.load()) {
      ring.drain([&](const Cmd& cmd) {
        if (std::this_thread::get_id() != loop_id) off_loop_runs++;
        ++loop_runs;
        if (cmd.id < seen.size()) seen[cmd.id]++;
        return Res{cmd.id, cmd.arg + 1};
      });
      std::this_thread::yield();
    }
  });

  std::atomic<int> wrong{0}, busy{0}, withdrawn{0}, done{0};
  std::vector<std::thread> requesters;
  for (int k = 0; k < 6; ++k) {
    requesters.emplace_back([&, k] {
      const auto t0 = std::chrono::steady_clock::now();
      auto now = [&] {
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
      };
      for (uint32_t i = 0; i < 500; ++i) {
        const uint32_t id = (uint32_t)k * 500 + i;
        Res r = {};
        lcr::Wait w;
        do {
          w = lcr::submit(ring, Cmd{id, (int32_t)id}, &r, 60000, 1, now,
                          [](uint32_t) { std::this_thread::yield(); });
          if (w == lcr::Wait::kBusy) busy++;
        } while (w == lcr::Wait::kBusy);
        if (w == lcr::Wait::kWithdrawn) withdrawn++;
        if (w == lcr::Wait::kDone) {
          done++;
          if (r.id != id || r.value != (int32_t)id + 1) wrong++;
        }
      }
    });
  }
  for (auto& t : requesters) t.join();
  stop = true;
  loop.join();

  CHECK(done.load() == 3000);
  CHECK(withdrawn.load() == 0);
  CHECK(wrong.load() == 0);
  CHECK(off_loop_runs.load() == 0);
  CHECK(loop_runs == 3000);
  int once = 0;
  for (uint8_t s : seen) once += s == 1 ? 1 : 0;
  CHECK(once == 3000);
  std::printf("  (%d posts found the ring full and retried)\n", busy.load());
}

int main() {
  test_runs_in_post_order_and_reports_each_result();
  test_order_is_by_ticket_not_by_slot();
  test_order_holds_across_the_ticket_wrap();
  test_a_full_ring_refuses_and_frees_on_collect();
  test_withdraw_only_before_the_command_starts();
  test_submit_returns_the_loop_tasks_result();
  test_submit_withdraws_an_unstarted_command_at_the_timeout();
  test_submit_waits_for_a_command_that_has_started();
  test_two_threads_every_command_runs_once_on_the_loop_thread();
  if (g_failures != 0) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d LOOP COMMAND RING CHECKS PASSED\n", g_checks);
  return 0;
}
