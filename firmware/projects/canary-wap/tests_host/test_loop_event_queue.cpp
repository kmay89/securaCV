// Host test: loop_event_queue.h, what another task reports, handed to the
// loop task to apply (sweep F143: the NimBLE host task's Bluetooth
// callbacks). The REAL header, compiled as is; the device's portMUX lock is
// replaced by a lock that counts its depth (one thread), or a std::mutex
// (the threaded tests, also run under -fsanitize=thread by
// `make tsan-loop-events`).
//
// What it pins:
//   - post order is apply order, across the wrap of the slots;
//   - a full queue refuses an event, counts it, and keeps every event it
//     holds; a post with a lower limit is refused once that many wait, and
//     the room above the limit stays for the posts that may use it;
//   - consume() applies only the events waiting when it starts (an apply
//     that posts leaves its event for the next consume), with no lock held
//     while an apply runs, and returns how many it applied;
//   - real threads (three producers, one consumer): every event is applied
//     once or counted as dropped, each producer's in its post order, and a
//     producer posting at full limit while a flooder posts under a lower
//     one loses nothing the reserve can hold.
//
// Run: ./test_loop_event_queue

#include "loop_event_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace leq = loop_event_queue;

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

// One thread: counts how deep the lock is held, so an apply can see it runs
// with none held.
struct DepthLock {
  static int depth;
  void lock() { ++depth; }
  void unlock() { --depth; }
};
int DepthLock::depth = 0;

struct MutexLock {
  std::mutex m;
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
};

struct Ev {
  uint32_t producer;
  uint32_t seq;
  char tag[20];
};

Ev ev(uint32_t producer, uint32_t seq) {
  Ev e;
  memset(&e, 0, sizeof e);
  e.producer = producer;
  e.seq = seq;
  snprintf(e.tag, sizeof e.tag, "p%u-%u", (unsigned)producer, (unsigned)seq);
  return e;
}

bool whole(const Ev& e) {
  char want[20];
  snprintf(want, sizeof want, "p%u-%u", (unsigned)e.producer, (unsigned)e.seq);
  return strcmp(want, e.tag) == 0;
}

void test_post_order_is_apply_order_across_the_wrap() {
  std::printf("post_order_is_apply_order_across_the_wrap\n");
  static leq::Queue<Ev, 4, DepthLock> q;
  uint32_t next = 0;
  std::vector<uint32_t> seen;
  for (int round = 0; round < 5; ++round) {     // 15 posts through 4 slots
    for (int i = 0; i < 3; ++i) CHECK(q.post(ev(0, next++)));
    CHECK(q.waiting() == 3);
    const size_t n = q.consume([&](const Ev& e) {
      CHECK(whole(e));
      seen.push_back(e.seq);
    });
    CHECK(n == 3 && q.waiting() == 0);
  }
  CHECK(seen.size() == 15);
  for (uint32_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == i);
  CHECK(q.dropped() == 0 && DepthLock::depth == 0);
}

void test_a_full_queue_refuses_and_counts() {
  std::printf("a_full_queue_refuses_and_counts\n");
  static leq::Queue<Ev, 4, DepthLock> q;
  for (uint32_t i = 0; i < 4; ++i) CHECK(q.post(ev(1, i)));
  CHECK(!q.post(ev(1, 4)));
  CHECK(!q.post(ev(1, 5)));
  CHECK(q.dropped() == 2 && q.waiting() == 4);
  std::vector<uint32_t> seen;
  CHECK(q.consume([&](const Ev& e) { seen.push_back(e.seq); }) == 4);
  CHECK((seen == std::vector<uint32_t>{0, 1, 2, 3}));
  CHECK(q.post(ev(1, 6)) && q.waiting() == 1);   // room again
  CHECK(q.dropped() == 2);
}

// A post under a lower limit (a scan result) is refused once that many wait;
// the room above it stays for posts at the full limit (a link's events).
void test_a_lower_limit_keeps_the_reserve() {
  std::printf("a_lower_limit_keeps_the_reserve\n");
  static leq::Queue<Ev, 6, DepthLock> q;
  for (uint32_t i = 0; i < 4; ++i) CHECK(q.post(ev(2, i), 4));
  CHECK(!q.post(ev(2, 4), 4));                    // the flooder stops at its limit
  CHECK(q.dropped() == 1);
  CHECK(q.post(ev(3, 0)) && q.post(ev(3, 1)));    // the reserve is there
  CHECK(!q.post(ev(3, 2)));                       // and the capacity is the cap
  CHECK(!q.post(ev(3, 3), 99));                   // a limit above N is N
  CHECK(q.dropped() == 3 && q.waiting() == 6);
  std::vector<uint32_t> producers;
  q.consume([&](const Ev& e) { producers.push_back(e.producer); });
  CHECK((producers == std::vector<uint32_t>{2, 2, 2, 2, 3, 3}));
}

// consume() applies what waited when it started; an apply's own post waits
// for the next consume. No lock is held while an apply runs.
void test_consume_applies_what_waited_with_no_lock_held() {
  std::printf("consume_applies_what_waited_with_no_lock_held\n");
  static leq::Queue<Ev, 8, DepthLock> q;
  CHECK(q.consume([](const Ev&) {}) == 0);       // empty: nothing
  for (uint32_t i = 0; i < 3; ++i) CHECK(q.post(ev(4, i)));
  std::vector<uint32_t> seen;
  int depth_in_apply = -1;
  const size_t n = q.consume([&](const Ev& e) {
    depth_in_apply = DepthLock::depth;
    seen.push_back(e.seq);
    CHECK(q.post(ev(4, 100 + e.seq)));           // posted while it applies
  });
  CHECK(n == 3 && depth_in_apply == 0);
  CHECK((seen == std::vector<uint32_t>{0, 1, 2}));
  CHECK(q.waiting() == 3);
  seen.clear();
  CHECK(q.consume([&](const Ev& e) { seen.push_back(e.seq); }) == 3);
  CHECK((seen == std::vector<uint32_t>{100, 101, 102}));
  CHECK(DepthLock::depth == 0);
}

// Three producers (one floods under a lower limit, as scan results do) and
// one consumer, on real threads.
void test_threads_every_event_once_in_order() {
  std::printf("threads_every_event_once_in_order\n");
  constexpr size_t kN = 16;
  constexpr size_t kFloodLimit = 12;
  constexpr uint32_t kPerProducer = 20000;
  static leq::Queue<Ev, kN, MutexLock> q;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> refused[3] = {{0}, {0}, {0}};
  // The two full-limit producers post in bursts of at most kN - kFloodLimit
  // between two consumes: then the reserve always holds them.
  std::atomic<uint32_t> consumes{0};
  std::vector<std::thread> producers;
  for (uint32_t p = 0; p < 3; ++p) {
    producers.emplace_back([&, p]() {
      const bool flooder = p == 0;
      uint32_t burst = 0;
      uint32_t seen_consumes = consumes.load();
      for (uint32_t i = 0; i < kPerProducer; ++i) {
        if (!flooder) {
          // One event per producer per consume at most: two producers, a
          // reserve of four.
          while (consumes.load() == seen_consumes && burst >= 1) std::this_thread::yield();
          if (consumes.load() != seen_consumes) {
            seen_consumes = consumes.load();
            burst = 0;
          }
          ++burst;
        }
        if (!q.post(ev(p, i), flooder ? kFloodLimit : kN)) refused[p].fetch_add(1);
        if (flooder && (i & 63) == 0) std::this_thread::yield();
      }
    });
  }
  std::vector<int64_t> last(3, -1);
  uint64_t applied[3] = {0, 0, 0};
  bool in_order = true, all_whole = true;
  std::thread consumer([&]() {
    while (!stop.load()) {
      q.consume([&](const Ev& e) {
        all_whole = all_whole && whole(e) && e.producer < 3;
        if (e.producer < 3) {
          in_order = in_order && (int64_t)e.seq > last[e.producer];
          last[e.producer] = e.seq;
          ++applied[e.producer];
        }
      });
      consumes.fetch_add(1);
      std::this_thread::yield();
    }
  });
  for (std::thread& t : producers) t.join();
  stop.store(true);
  consumer.join();
  q.consume([&](const Ev& e) {
    in_order = in_order && (int64_t)e.seq > last[e.producer];
    last[e.producer] = e.seq;
    ++applied[e.producer];
  });
  CHECK(all_whole && in_order);
  for (uint32_t p = 0; p < 3; ++p) CHECK(applied[p] + refused[p].load() == kPerProducer);
  CHECK(q.dropped() == refused[0].load() + refused[1].load() + refused[2].load());
  CHECK(refused[1].load() == 0 && refused[2].load() == 0);   // the reserve held them
  CHECK(q.waiting() == 0);
  std::printf("  flooder refused %u of %u; consumes %u\n", (unsigned)refused[0].load(),
              (unsigned)kPerProducer, (unsigned)consumes.load());
}

int main() {
  test_post_order_is_apply_order_across_the_wrap();
  test_a_full_queue_refuses_and_counts();
  test_a_lower_limit_keeps_the_reserve();
  test_consume_applies_what_waited_with_no_lock_held();
  test_threads_every_event_once_in_order();
  if (g_failures != 0) {
    std::printf("%d of %d loop event queue checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d loop event queue checks PASSED\n", g_checks);
  return 0;
}
