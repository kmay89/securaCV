// Host test: loop_snapshot.h, what the loop task publishes for other tasks
// to read whole (sweep F110: the mesh status routes). The REAL header,
// compiled as is; the device's portMUX lock is replaced by none (one
// thread), a lock that lets the test act as the loop task between a
// reader's critical sections (deterministic interleavings), or a std::mutex
// (the threaded tests, also run under -fsanitize=thread by
// `make tsan-loop-snapshot`).
//
// What it pins:
//   - Value: nothing to read before the first publish; a read is the last
//     value published, whole; publishing the same bytes takes no lock;
//   - Log: records are read in storage order, at most `cap`, none without
//     storage; a read the loop task changes the log under (an append that
//     wraps over a record the read already copied, a clear) starts over and
//     returns one moment's records, never a mix; a read the loop task keeps
//     changing ends with one copy under one hold;
//   - real threads (one writer, four readers): every Value read is one
//     published value, whole, and every Log read is one moment's records
//     (whole records, consecutive sequence numbers, as many as were held).
//
// Run: ./test_loop_snapshot

#include "loop_snapshot.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ls = loop_snapshot;

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

// Counts its holds; from the `at`-th lock() on it first runs `hook` (once,
// or on every hold when `every`), which plays the loop task between two of
// a reader's critical sections. The hook's own holds run no hook.
struct HookLock {
  static unsigned locks;
  static unsigned at;
  static std::function<void()> hook;
  static bool every;
  static bool in_hook;
  void lock() {
    ++locks;
    if (in_hook || !hook || locks < at) return;
    std::function<void()> h = hook;
    if (!every) hook = nullptr;
    in_hook = true;
    h();
    in_hook = false;
  }
  void unlock() {}
};
unsigned HookLock::locks = 0;
unsigned HookLock::at = 0;
std::function<void()> HookLock::hook;
bool HookLock::every = false;
bool HookLock::in_hook = false;

void reset_hook() {
  HookLock::locks = 0;
  HookLock::at = 0;
  HookLock::hook = nullptr;
  HookLock::every = false;
  HookLock::in_hook = false;
}

struct MutexLock {
  std::mutex m;
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
};

// A value whose fields all say which publish it came from.
struct View {
  uint32_t gen;
  uint8_t count;
  uint32_t items[16];
  char name[24];
};

View view_of(uint32_t gen) {
  View v;
  memset(&v, 0, sizeof v);
  v.gen = gen;
  v.count = (uint8_t)(gen % 17);
  for (int i = 0; i < 16; ++i) v.items[i] = gen;
  snprintf(v.name, sizeof v.name, "pass-%u", (unsigned)gen);
  return v;
}

bool whole(const View& v) {
  if (v.count != v.gen % 17) return false;
  for (int i = 0; i < 16; ++i) {
    if (v.items[i] != v.gen) return false;
  }
  char want[24] = {};
  snprintf(want, sizeof want, "pass-%u", (unsigned)v.gen);
  return memcmp(v.name, want, sizeof want) == 0;
}

// A record that says its own sequence number in every byte it carries.
struct Rec {
  uint32_t seq;
  uint32_t body[20];
};

Rec rec_of(uint32_t seq) {
  Rec r;
  r.seq = seq;
  for (int i = 0; i < 20; ++i) r.body[i] = seq;
  return r;
}

bool rec_whole(const Rec& r) {
  for (int i = 0; i < 20; ++i) {
    if (r.body[i] != r.seq) return false;
  }
  return true;
}

std::vector<uint32_t> seqs(const Rec* r, size_t n) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < n; ++i) out.push_back(r[i].seq);
  return out;
}

// ── Value ────────────────────────────────────────────────────────────────

static void test_value_reads_the_last_publish_whole() {
  std::printf("test_value_reads_the_last_publish_whole\n");
  ls::Value<View, NoLock> v;
  View out = view_of(99);
  CHECK(!v.read(&out));
  CHECK(out.gen == 99);                      // untouched
  CHECK(v.publish(view_of(1)));
  CHECK(v.read(&out) && out.gen == 1 && whole(out));
  CHECK(v.publish(view_of(2)));
  CHECK(v.read(&out) && out.gen == 2 && whole(out));
}

static void test_value_same_bytes_take_no_lock() {
  std::printf("test_value_same_bytes_take_no_lock\n");
  reset_hook();
  ls::Value<View, HookLock> v;
  CHECK(v.publish(view_of(5)));
  const unsigned after_first = HookLock::locks;
  CHECK(!v.publish(view_of(5)));
  CHECK(HookLock::locks == after_first);     // compared, not copied
  CHECK(v.publish(view_of(6)));
  CHECK(HookLock::locks == after_first + 1);
}

// ── Log ──────────────────────────────────────────────────────────────────

static void test_log_reads_in_storage_order() {
  std::printf("test_log_reads_in_storage_order\n");
  Rec storage[4];
  ls::Log<Rec, 4, NoLock> log;
  Rec out[8];
  CHECK(!log.append(rec_of(1)));             // nowhere to keep it
  CHECK(log.read(out, 8) == 0);
  log.attach(storage);
  for (uint32_t s = 1; s <= 3; ++s) CHECK(log.append(rec_of(s)));
  CHECK(log.read(out, 8) == 3);
  CHECK((seqs(out, 3) == std::vector<uint32_t>{1, 2, 3}));
  CHECK(log.append(rec_of(4)) && log.append(rec_of(5)));   // 5 wraps over 1
  CHECK(log.count() == 4);
  CHECK(log.read(out, 8) == 4);
  CHECK((seqs(out, 4) == std::vector<uint32_t>{5, 2, 3, 4}));   // slot order
  CHECK(log.read(out, 2) == 2);
  CHECK((seqs(out, 2) == std::vector<uint32_t>{5, 2}));
  log.clear();
  CHECK(log.read(out, 8) == 0 && log.count() == 0);
  for (int i = 0; i < 4; ++i) CHECK(storage[i].seq == 0);   // zeroed
}

// The loop task wraps over two records between a reader's copies: slot 0
// was copied as record 1, then records 5 and 6 replace slots 0 and 1. A
// read that kept going would return 1 and 6 together, which the log never
// held at one moment.
static void test_log_read_starts_over_when_the_log_changes() {
  std::printf("test_log_read_starts_over_when_the_log_changes\n");
  reset_hook();
  Rec storage[4];
  ls::Log<Rec, 4, HookLock> log;
  log.attach(storage);
  for (uint32_t s = 1; s <= 4; ++s) log.append(rec_of(s));
  HookLock::locks = 0;
  HookLock::at = 3;                          // 1: gen and count, 2: slot 0, 3: slot 1
  HookLock::hook = [&] {
    log.append(rec_of(5));
    log.append(rec_of(6));
  };
  Rec out[4];
  CHECK(log.read(out, 4) == 4);
  CHECK((seqs(out, 4) == std::vector<uint32_t>{5, 6, 3, 4}));
  for (const Rec& r : out) CHECK(rec_whole(r));
}

// A clear between two copies: zeroed records were never what the log held.
static void test_log_read_across_a_clear_is_empty() {
  std::printf("test_log_read_across_a_clear_is_empty\n");
  reset_hook();
  Rec storage[4];
  ls::Log<Rec, 4, HookLock> log;
  log.attach(storage);
  for (uint32_t s = 1; s <= 3; ++s) log.append(rec_of(s));
  HookLock::locks = 0;
  HookLock::at = 3;
  HookLock::hook = [&] { log.clear(); };
  Rec out[4];
  CHECK(log.read(out, 4) == 0);
}

// The loop task changes the log before every one of the reader's copies:
// the read still ends, with one moment's records, copied under one hold.
static void test_log_read_the_loop_task_keeps_changing_ends_whole() {
  std::printf("test_log_read_the_loop_task_keeps_changing_ends_whole\n");
  reset_hook();
  Rec storage[4];
  ls::Log<Rec, 4, HookLock> log;
  log.attach(storage);
  uint32_t next = 1;
  for (; next <= 4; ++next) log.append(rec_of(next));
  HookLock::locks = 0;
  HookLock::at = 2;
  HookLock::every = true;
  HookLock::hook = [&] { log.append(rec_of(next++)); };
  Rec out[4];
  CHECK(log.read(out, 4) == 4);
  reset_hook();
  std::vector<uint32_t> got = seqs(out, 4);
  std::sort(got.begin(), got.end());
  CHECK(got[3] == next - 1);                 // the newest four, at the last change
  for (int i = 1; i < 4; ++i) CHECK(got[i] == got[i - 1] + 1);
  for (const Rec& r : out) CHECK(rec_whole(r));
}

// ── Real threads ─────────────────────────────────────────────────────────

static void test_threads_value_reads_are_whole() {
  std::printf("test_threads_value_reads_are_whole\n");
  ls::Value<View, MutexLock> v;
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, torn{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      View out;
      while (!stop.load()) {
        if (v.read(&out)) {
          ++reads;
          if (!whole(out)) ++torn;
        }
      }
    });
  }
  for (uint32_t gen = 1; gen <= 200000; ++gen) {
    v.publish(view_of(gen));
    if (gen % 3 == 0) v.publish(view_of(gen));   // the same bytes again
  }
  stop = true;
  for (std::thread& t : readers) t.join();
  View last;
  CHECK(v.read(&last) && last.gen == 200000);
  CHECK(reads.load() > 0);
  CHECK(torn.load() == 0);
}

static void test_threads_log_reads_are_one_moment() {
  std::printf("test_threads_log_reads_are_one_moment\n");
  static Rec storage[8];
  ls::Log<Rec, 8, MutexLock> log;
  log.attach(storage);
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, bad{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      Rec out[8];
      while (!stop.load()) {
        const size_t n = log.read(out, 8);
        ++reads;
        std::vector<uint32_t> got = seqs(out, n);
        for (size_t i = 0; i < n; ++i) {
          if (!rec_whole(out[i]) || out[i].seq == 0) ++bad;
        }
        // One moment's records: consecutive sequence numbers (a clear
        // empties the log, so it never holds a gap).
        std::sort(got.begin(), got.end());
        for (size_t i = 1; i < got.size(); ++i) {
          if (got[i] != got[i - 1] + 1) ++bad;
        }
      }
    });
  }
  for (uint32_t seq = 1; seq <= 200000; ++seq) {
    log.append(rec_of(seq));
    if (seq % 997 == 0) log.clear();
  }
  stop = true;
  for (std::thread& t : readers) t.join();
  CHECK(reads.load() > 0);
  CHECK(bad.load() == 0);
}

int main() {
  test_value_reads_the_last_publish_whole();
  test_value_same_bytes_take_no_lock();
  test_log_reads_in_storage_order();
  test_log_read_starts_over_when_the_log_changes();
  test_log_read_across_a_clear_is_empty();
  test_log_read_the_loop_task_keeps_changing_ends_whole();
  test_threads_value_reads_are_whole();
  test_threads_log_reads_are_one_moment();
  if (g_failures != 0) {
    std::printf("loop_snapshot: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d loop_snapshot checks PASSED\n", g_checks);
  return 0;
}
