// Host test for firmware/common/witness/witness_history_bridge.h (repo sweep
// F35, F26 stage 2): the httpd-task / loop-task handshake behind the
// timeline's card pages — one outstanding request, the generation counter, the
// bounded wait, the per-pass read budget, the resume-hint rules and the
// linkage a page reports — driven through deterministic interleavings, then
// through real threads. Every file is built with the firmware's own
// witness_store::line_build, so the bridge reads the exact bytes the device
// appends.
//
// Run: make -C firmware/tests_host

#include "witness_history_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace wb = witness_history_bridge;
namespace wh = witness_history;
namespace ws = witness_store;

static int g_fail = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      g_fail++;                                                              \
    }                                                                        \
  } while (0)

static void report(const char* name, int fails_before) {
  std::printf("%s %s\n", g_fail == fails_before ? "PASS" : "FAIL", name);
}

// ── Files ──────────────────────────────────────────────────────────────────

// Deterministic chain: record i's ch is a function of i, its prev is record
// i-1's ch — the linkage the bridge reports (no real hashing: the bridge
// checks linkage and format, not SHA-256).
static void ch_for(uint32_t i, uint8_t out[32]) {
  for (int j = 0; j < 32; ++j) out[j] = (uint8_t)((i * 131u + (uint32_t)j * 17u + 5u) & 0xFF);
}

static std::string line_for(uint32_t seq, bool break_prev = false) {
  char buf[ws::RECORD_LINE_MAX];
  uint8_t ph[32], prev[32], ch[32], sig[64];
  for (int j = 0; j < 32; ++j) ph[j] = (uint8_t)(seq ^ (uint32_t)j);
  ch_for(seq - 1, prev);
  if (break_prev) prev[0] ^= 0x5A;
  ch_for(seq, ch);
  for (int j = 0; j < 64; ++j) sig[j] = (uint8_t)(seq * 3u + (uint32_t)j);
  const size_t n = ws::line_build(buf, sizeof(buf), seq, seq % 144, (uint8_t)(seq % 5),
                                  ph, prev, ch, sig);
  CHECK(n > 0);
  return std::string(buf, n);
}

struct Built {
  std::string file;
  std::vector<uint32_t> start;  // line start by seq (0 where absent)
};

// Records first..last inclusive; `skip` leaves one seq out (a card gap);
// `broken` gives one record a prev that does not match its predecessor.
static Built build(uint32_t first, uint32_t last, uint32_t skip = 0, uint32_t broken = 0) {
  Built b;
  b.start.assign(last + 2, 0);
  for (uint32_t seq = first; seq <= last; ++seq) {
    if (seq == skip) continue;
    b.start[seq] = (uint32_t)b.file.size();
    b.file += line_for(seq, seq == broken);
  }
  return b;
}

// ── The SD stand-in ────────────────────────────────────────────────────────

struct MemIo {
  std::string file;
  wb::Card state = wb::Card::READY;
  uint32_t token = 1;
  bool exists = true;
  bool is_open = false;
  size_t opens = 0, reads = 0, pass_reads = 0, max_len = 0;
  size_t fail_on_read = 0;          // the Nth read (1-based) comes back short
  uint32_t first_read_end = 0;      // off + len of the first read of a walk
  std::function<void(size_t)> on_read;  // the httpd task, mid-pass

  wb::Card card() { return state; }
  uint32_t card_token() { return token; }
  bool open(uint32_t* size) {
    CHECK(!is_open);
    if (!exists) return false;
    is_open = true;
    opens++;
    *size = (uint32_t)file.size();
    return true;
  }
  size_t read(uint32_t off, char* buf, size_t len) {
    CHECK(is_open);
    CHECK(len <= wb::READ_LEN);
    CHECK((size_t)off + len <= file.size());
    reads++;
    pass_reads++;
    if (len > max_len) max_len = len;
    if (reads == 1) first_read_end = off + (uint32_t)len;
    std::memcpy(buf, file.data() + off, len);
    if (on_read) on_read(reads);
    if (fail_on_read != 0 && reads == fail_on_read) return len - 1;
    return len;
  }
  void close() {
    CHECK(is_open);
    is_open = false;
  }
};

static char g_buf[wb::READ_LEN];

// One loop pass, holding the per-pass contract: at most READS_PER_PASS reads,
// and the file is closed again before the pass returns.
static void pass(wb::Slot* s, MemIo& io) {
  io.pass_reads = 0;
  wb::service(s, io, g_buf);
  CHECK(io.pass_reads <= wb::READS_PER_PASS);
  CHECK(!io.is_open);
}

static wb::Request req(uint32_t before, uint8_t want, bool has_hint = false, uint32_t hint = 0) {
  wb::Request r;
  std::memset(&r, 0, sizeof(r));
  r.before_seq = before;
  r.want = want;
  r.has_hint = has_hint;
  r.hint = hint;
  return r;
}

struct Page {
  bool got = false;
  wb::Result result = wb::Result::OK;
  std::vector<uint32_t> seqs;       // newest first
  std::vector<wb::Link> linked;
  wb::Link joins = wb::Link::NONE;
  bool more = false, hint_refused = false;
  uint32_t next_hint = 0, skipped = 0;
  size_t passes = 0, reads = 0;
};

static Page copy_page(const wb::Response* p) {
  Page out;
  out.got = true;
  out.result = p->result;
  for (size_t i = p->first; i < (size_t)p->first + p->n; ++i) {
    out.seqs.push_back(p->rows[i].seq);
    out.linked.push_back(p->linked[i]);
  }
  out.joins = p->joins;
  out.more = p->more;
  out.hint_refused = p->hint_refused;
  out.next_hint = p->next_hint;
  out.skipped = p->skipped;
  return out;
}

// begin -> loop passes until the page lands -> copy -> end.
static Page run(wb::Slot* s, MemIo& io, const wb::Request& r, size_t max_passes = 200) {
  const uint32_t gen = wb::begin(s, r);
  CHECK(gen != 0);
  CHECK(wb::poll(s, gen) == nullptr);
  const size_t reads0 = io.reads;
  io.first_read_end = 0;
  Page out;
  size_t passes = 0;
  while (passes < max_passes) {
    pass(s, io);
    passes++;
    if (const wb::Response* p = wb::poll(s, gen)) {
      out = copy_page(p);
      break;
    }
  }
  wb::end(s, gen, false);
  out.passes = passes;
  out.reads = io.reads - reads0;
  return out;
}

static bool seqs_are(const Page& p, uint32_t newest, size_t n) {
  if (p.seqs.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (p.seqs[i] != newest - (uint32_t)i) return false;
  return true;
}

static bool all_linked(const Page& p) {
  for (wb::Link l : p.linked)
    if (l != wb::Link::LINKED) return false;
  return true;
}

// ── Tests ──────────────────────────────────────────────────────────────────

// The first card page under the ring: scanned from the file end, the card's
// copy of the record at `before` read as the anchor, one neighbor below.
static void test_first_card_page() {
  const int f0 = g_fail;
  const Built b = build(1, 200);
  MemIo io;
  io.file = b.file;
  wb::Slot s;
  wb::init(&s);
  const Page p = run(&s, io, req(170, 20));
  CHECK(p.got && p.result == wb::Result::OK);
  CHECK(seqs_are(p, 169, 20));
  CHECK(all_linked(p));
  CHECK(p.joins == wb::Link::LINKED);
  CHECK(p.more);
  CHECK(!p.hint_refused);
  CHECK(p.next_hint == b.start[150]);
  CHECK(p.skipped == 0);
  // Bounded work: the reads cover the file end down to the '\n' before the
  // neighbor (record 149) and not one more, 1 KiB at a time, at most four a
  // pass — so the page took exactly ceil(reads / 4) passes.
  const size_t span = b.file.size() - (b.start[149] - 1);
  CHECK(p.reads == (span + wb::READ_LEN - 1) / wb::READ_LEN);
  CHECK(p.passes == (p.reads + wb::READS_PER_PASS - 1) / wb::READS_PER_PASS);
  CHECK(io.max_len == wb::READ_LEN);
  CHECK(io.first_read_end == b.file.size());
  CHECK(io.opens == p.passes);  // once per pass, never held across passes
  report("first_card_page", f0);
}

// Hint pages walk the rest of the file O(page) each, every row checked
// against its older neighbor, down to the card's first record.
static void test_hint_pages_to_the_start() {
  const int f0 = g_fail;
  const Built b = build(1, 200);
  MemIo io;
  io.file = b.file;
  wb::Slot s;
  wb::init(&s);
  Page p = run(&s, io, req(170, 20));
  std::vector<uint32_t> seen(p.seqs);
  size_t pages = 1;
  while (p.more) {
    const uint32_t before = p.seqs.back();
    const uint32_t hint = p.next_hint;
    p = run(&s, io, req(before, 20, true, hint));
    pages++;
    CHECK(p.got && p.result == wb::Result::OK);
    CHECK(!p.hint_refused);
    CHECK(p.joins == wb::Link::NONE);  // the previous page checked the join
    CHECK(!p.seqs.empty() && p.seqs.front() == before - 1);
    // O(page): 21 lines of ~390 bytes plus the '\n' before them.
    CHECK(p.reads <= (21 * 400 + wb::READ_LEN - 1) / wb::READ_LEN + 1);
    seen.insert(seen.end(), p.seqs.begin(), p.seqs.end());
    if (pages > 20) break;
  }
  CHECK(seen.size() == 169);
  for (size_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == 169 - i);
  // The last page ends at the card's first record: nothing older to link.
  CHECK(!p.more);
  CHECK(p.seqs.back() == 1);
  CHECK(p.linked.back() == wb::Link::NONE);
  for (size_t i = 0; i + 1 < p.linked.size(); ++i) CHECK(p.linked[i] == wb::Link::LINKED);
  report("hint_pages_to_the_start", f0);
}

// A hint is client input: past the file it is refused before any read, a
// wrong one is refused by the walker; both fall back to a scan from the end
// and return the same page as no hint at all. A hint of 0 reads nothing.
static void test_hint_refusals() {
  const int f0 = g_fail;
  const Built b = build(1, 120);
  MemIo io;
  io.file = b.file;
  wb::Slot s;
  wb::init(&s);
  const Page ref = run(&s, io, req(90, 10));
  CHECK(seqs_are(ref, 89, 10));

  io.reads = 0;
  const Page past = run(&s, io, req(90, 10, true, (uint32_t)b.file.size() + 1));
  CHECK(past.hint_refused);
  CHECK(io.first_read_end == b.file.size());  // the first read was the file end
  CHECK(past.seqs == ref.seqs && past.next_hint == ref.next_hint);
  CHECK(past.joins == wb::Link::LINKED);

  // A hint exactly at the file end is a line start: accepted, and wrong for
  // before 90, so the walker refuses it.
  const Page at_end = run(&s, io, req(90, 10, true, (uint32_t)b.file.size()));
  CHECK(at_end.hint_refused && at_end.seqs == ref.seqs);

  const Page mid = run(&s, io, req(90, 10, true, b.start[90] + 17));
  CHECK(mid.hint_refused && mid.seqs == ref.seqs);

  const Page wrong = run(&s, io, req(90, 10, true, b.start[60]));
  CHECK(wrong.hint_refused && wrong.seqs == ref.seqs);

  const Page good = run(&s, io, req(90, 10, true, b.start[90]));
  CHECK(!good.hint_refused && good.seqs == ref.seqs);
  CHECK(good.reads < ref.reads);  // O(page), not O(distance from the end)

  io.reads = 0;
  const Page zero = run(&s, io, req(90, 10, true, 0));
  CHECK(zero.got && zero.seqs.empty() && !zero.more && !zero.hint_refused);
  CHECK(zero.reads == 0);
  report("hint_refusals", f0);
}

// One outstanding request: a second one is refused while the first holds the
// slot, and taken the moment it is released.
static void test_one_outstanding_request() {
  const int f0 = g_fail;
  const Built b = build(1, 60);
  MemIo io;
  io.file = b.file;
  wb::Slot s;
  wb::init(&s);
  const uint32_t g1 = wb::begin(&s, req(50, 5));
  CHECK(g1 != 0);
  CHECK(wb::begin(&s, req(40, 5)) == 0);          // 503 history_busy
  for (int i = 0; i < 50 && !wb::poll(&s, g1); ++i) pass(&s, io);
  CHECK(wb::poll(&s, g1) != nullptr);
  CHECK(wb::begin(&s, req(40, 5)) == 0);          // still held while answering
  wb::end(&s, g1, false);
  const uint32_t g2 = wb::begin(&s, req(40, 5));
  CHECK(g2 != 0 && g2 != g1);
  CHECK(wb::poll(&s, g2) == nullptr);              // g1's page is not g2's
  for (int i = 0; i < 50 && !wb::poll(&s, g2); ++i) pass(&s, io);
  const wb::Response* p = wb::poll(&s, g2);
  CHECK(p != nullptr && p->n == 5 && p->rows[p->first].seq == 39);
  wb::end(&s, g2, false);
  // begin() clamps want to 1..PAGE_ROWS_MAX.
  const Page big = run(&s, io, req(59, 200));
  CHECK(big.seqs.size() == wb::PAGE_ROWS_MAX);
  const Page none = run(&s, io, req(59, 0));
  CHECK(none.seqs.size() == 1);
  report("one_outstanding_request", f0);
}

// A waiter that gives up takes its walk with it: the loop reads nothing more
// for it, and the slot is free at once.
static void test_abandoned_walk_stops_reading() {
  const int f0 = g_fail;
  const Built b = build(1, 200);
  MemIo io;
  io.file = b.file;
  wb::Slot s;
  wb::init(&s);
  const uint32_t g1 = wb::begin(&s, req(100, 20));
  pass(&s, io);
  CHECK(io.reads == wb::READS_PER_PASS);  // a deep page: not done in one pass
  CHECK(wb::poll(&s, g1) == nullptr);
  wb::end(&s, g1, true);                   // 504 history_timeout
  const size_t reads = io.reads, opens = io.opens;
  pass(&s, io);
  pass(&s, io);
  CHECK(io.reads == reads && io.opens == opens);
  CHECK(wb::poll(&s, g1) == nullptr);
  const Page p = run(&s, io, req(100, 3));
  CHECK(seqs_are(p, 99, 3));
  report("abandoned_walk_stops_reading", f0);
}

// The generation counter: a request published while an older walk is still
// running replaces it, and a completion that lands after the next request was
// published never answers that request.
static void test_late_completion_never_answers_the_next_request() {
  const int f0 = g_fail;
  const Built b = build(1, 200);

  // (a) The loop is mid-walk when the waiter gives up and the next request
  // arrives: the next pass drops the old walk and walks the new one.
  {
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g1 = wb::begin(&s, req(100, 20));
    pass(&s, io);
    wb::end(&s, g1, true);
    const uint32_t g2 = wb::begin(&s, req(30, 4));
    CHECK(g2 != 0 && g2 != g1);
    const wb::Response* p = nullptr;
    for (int i = 0; i < 100 && !(p = wb::poll(&s, g2)); ++i) {
      pass(&s, io);
      CHECK(wb::poll(&s, g1) == nullptr);
    }
    CHECK(p != nullptr && p->n == 4 && p->rows[p->first].seq == 29);
    wb::end(&s, g2, false);
  }

  // (b) The old walk finishes in the very pass during which its waiter gives
  // up and the next request is published (the httpd task runs on the other
  // core, mid-pass): that page is not published, and the next request's
  // waiter keeps waiting for its own.
  {
    MemIo dry;
    dry.file = b.file;
    wb::Slot d;
    wb::init(&d);
    const size_t reads_g1 = run(&d, dry, req(190, 3)).reads;  // g1's last read

    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g1 = wb::begin(&s, req(190, 3));
    uint32_t g2 = 0;
    io.on_read = [&](size_t n) {
      if (n == reads_g1) {
        wb::end(&s, g1, true);
        g2 = wb::begin(&s, req(30, 4));
      }
    };
    for (int i = 0; i < 20 && g2 == 0; ++i) pass(&s, io);
    io.on_read = nullptr;
    CHECK(g2 != 0 && g2 != g1);
    CHECK(s.done_gen != g1);                 // the superseded page was dropped
    CHECK(wb::poll(&s, g1) == nullptr);
    CHECK(wb::poll(&s, g2) == nullptr);      // and it never passes for g2's
    const wb::Response* p = nullptr;
    for (int i = 0; i < 100 && !(p = wb::poll(&s, g2)); ++i) pass(&s, io);
    CHECK(p != nullptr && p->n == 4 && p->rows[p->first].seq == 29);
    wb::end(&s, g2, false);
  }

  // (c) The same race, but the waiter only gives up (no next request yet):
  // the finished page is still not published for an abandoned generation.
  {
    MemIo dry;
    dry.file = b.file;
    wb::Slot d;
    wb::init(&d);
    const size_t reads_g1 = run(&d, dry, req(190, 3)).reads;

    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g1 = wb::begin(&s, req(190, 3));
    bool gave_up = false;
    io.on_read = [&](size_t n) {
      if (n == reads_g1) {
        wb::end(&s, g1, true);
        gave_up = true;
      }
    };
    for (int i = 0; i < 20 && !gave_up; ++i) pass(&s, io);
    CHECK(gave_up);
    CHECK(wb::poll(&s, g1) == nullptr);
    CHECK(!s.active);
  }

  // (d) Two waiters give up while one walk runs (the loop stalled through
  // both waits), and a third request is published before the walk ends: the
  // abandon mark now names the second request, so only the generation check
  // keeps the first walk's page from being published.
  {
    MemIo dry;
    dry.file = b.file;
    wb::Slot d;
    wb::init(&d);
    const size_t reads_g1 = run(&d, dry, req(190, 3)).reads;

    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g1 = wb::begin(&s, req(190, 3));
    uint32_t g3 = 0;
    io.on_read = [&](size_t n) {
      if (n == reads_g1) {
        wb::end(&s, g1, true);
        const uint32_t g2 = wb::begin(&s, req(40, 2));
        wb::end(&s, g2, true);
        g3 = wb::begin(&s, req(30, 4));
      }
    };
    for (int i = 0; i < 20 && g3 == 0; ++i) pass(&s, io);
    io.on_read = nullptr;
    CHECK(g3 != 0);
    CHECK(s.done_gen != g1);
    const wb::Response* p = nullptr;
    for (int i = 0; i < 100 && !(p = wb::poll(&s, g3)); ++i) pass(&s, io);
    CHECK(p != nullptr && p->n == 4 && p->rows[p->first].seq == 29);
    wb::end(&s, g3, false);
  }

  // (e) The same double give-up early in a deep walk: the next pass drops
  // the old walk and reads for the newest request only — its page costs
  // exactly what it costs alone.
  {
    MemIo dry;
    dry.file = b.file;
    wb::Slot d;
    wb::init(&d);
    const Page alone = run(&d, dry, req(190, 3));

    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g1 = wb::begin(&s, req(30, 20));  // ~170 lines from the end
    pass(&s, io);
    wb::end(&s, g1, true);
    const uint32_t g2 = wb::begin(&s, req(40, 2));
    wb::end(&s, g2, true);
    const uint32_t g3 = wb::begin(&s, req(190, 3));
    const size_t reads0 = io.reads;
    size_t passes = 0;
    const wb::Response* p = nullptr;
    for (int i = 0; i < 100 && !(p = wb::poll(&s, g3)); ++i) {
      pass(&s, io);
      passes++;
    }
    CHECK(p != nullptr && p->n == 3 && p->rows[p->first].seq == 189);
    CHECK(io.reads - reads0 == alone.reads);
    CHECK(passes == alone.passes);
    wb::end(&s, g3, false);
  }
  report("late_completion_never_answers_the_next_request", f0);
}

// The httpd task's wait is bounded by WAIT_MS whatever the loop does.
static void test_wait_is_bounded() {
  const int f0 = g_fail;
  const Built b = build(1, 200);
  wb::Slot s;
  wb::init(&s);

  // A stalled loop (a camera peek, a remount): the wait gives up at exactly
  // the budget, in WAIT_STEP_MS sleeps.
  const uint32_t g1 = wb::begin(&s, req(100, 20));
  uint32_t slept = 0, sleeps = 0;
  const wb::Response* p = wb::wait(&s, g1, wb::WAIT_MS, wb::WAIT_STEP_MS,
                                   [&](uint32_t ms) { slept += ms; sleeps++; });
  CHECK(p == nullptr);
  CHECK(slept == wb::WAIT_MS);
  CHECK(sleeps == wb::WAIT_MS / wb::WAIT_STEP_MS);
  wb::end(&s, g1, true);

  // A healthy loop: one pass per sleep, the page long before the budget.
  MemIo io;
  io.file = b.file;
  const uint32_t g2 = wb::begin(&s, req(100, 20));
  slept = 0;
  p = wb::wait(&s, g2, wb::WAIT_MS, wb::WAIT_STEP_MS, [&](uint32_t ms) {
    slept += ms;
    pass(&s, io);
  });
  CHECK(p != nullptr && p->n == 20 && p->rows[p->first].seq == 99);
  CHECK(slept < wb::WAIT_MS);
  wb::end(&s, g2, false);

  // An odd budget still ends exactly on it.
  const uint32_t g3 = wb::begin(&s, req(100, 20));
  slept = 0;
  CHECK(wb::wait(&s, g3, 25, 10, [&](uint32_t ms) { slept += ms; }) == nullptr);
  CHECK(slept == 25);
  wb::end(&s, g3, true);
  report("wait_is_bounded", f0);
}

// The card as the loop sees it: absent (answered at once, nothing opened), a
// mount in flight (nothing touched until it resolves), and every way it can
// change under a page.
static void test_card_states() {
  const int f0 = g_fail;
  const Built b = build(1, 200);

  {  // No card: NO_CARD on the first pass, no open, no read.
    MemIo io;
    io.file = b.file;
    io.state = wb::Card::ABSENT;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(100, 10));
    CHECK(p.got && p.result == wb::Result::NO_CARD && p.passes == 1);
    CHECK(io.opens == 0 && io.reads == 0);
  }
  {  // A mount in flight: no progress, nothing opened; then it lands.
    MemIo io;
    io.file = b.file;
    io.state = wb::Card::BUSY;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g = wb::begin(&s, req(100, 10));
    for (int i = 0; i < 5; ++i) pass(&s, io);
    CHECK(io.opens == 0 && io.reads == 0 && wb::poll(&s, g) == nullptr);
    io.state = wb::Card::READY;
    for (int i = 0; i < 50 && !wb::poll(&s, g); ++i) pass(&s, io);
    const wb::Response* p = wb::poll(&s, g);
    CHECK(p != nullptr && p->result == wb::Result::OK && p->n == 10);
    wb::end(&s, g, false);
  }
  {  // No history file: an empty page that says nothing is older.
    MemIo io;
    io.exists = false;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(100, 10, true, 5000));
    CHECK(p.got && p.result == wb::Result::OK && p.seqs.empty() && !p.more);
  }
  {  // Remounted (or another card) mid-page.
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g = wb::begin(&s, req(100, 20));
    pass(&s, io);
    io.token = 2;
    pass(&s, io);
    const wb::Response* p = wb::poll(&s, g);
    CHECK(p != nullptr && p->result == wb::Result::IO_ERROR && p->n == 0);
    wb::end(&s, g, false);
  }
  {  // The file shrank mid-page: not the file the page started on.
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const uint32_t g = wb::begin(&s, req(100, 20));
    pass(&s, io);
    io.file.resize(io.file.size() / 2);
    pass(&s, io);
    const wb::Response* p = wb::poll(&s, g);
    CHECK(p != nullptr && p->result == wb::Result::IO_ERROR);
    wb::end(&s, g, false);
  }
  {  // A short read.
    MemIo io;
    io.file = b.file;
    io.fail_on_read = 6;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(100, 20));
    CHECK(p.got && p.result == wb::Result::IO_ERROR && p.seqs.empty());
    CHECK(io.reads == 6);
  }
  report("card_states", f0);
}

// The loop appends records between passes (it is the same task): the page is
// the one the file held when the walk started.
static void test_appends_between_passes() {
  const int f0 = g_fail;
  const Built b = build(1, 200);
  MemIo ref_io;
  ref_io.file = b.file;
  wb::Slot r;
  wb::init(&r);
  const Page ref = run(&r, ref_io, req(150, 25));

  MemIo io;
  io.file = b.file;
  wb::Slot s;
  wb::init(&s);
  const uint32_t g = wb::begin(&s, req(150, 25));
  size_t next = 201;
  const wb::Response* p = nullptr;
  for (int i = 0; i < 100 && !(p = wb::poll(&s, g)); ++i) {
    pass(&s, io);
    if (next <= 240) io.file += line_for((uint32_t)next++);
  }
  CHECK(p != nullptr);
  if (p != nullptr) {
    const Page got = copy_page(p);
    CHECK(got.seqs == ref.seqs && got.linked == ref.linked && got.next_hint == ref.next_hint);
  }
  wb::end(&s, g, false);
  report("appends_between_passes", f0);
}

// What linkage a page reports: a record whose prev does not match, a gap on
// the card, and a card whose history starts mid-chain.
static void test_linkage_and_gaps() {
  const int f0 = g_fail;
  {  // Record 160's prev is wrong: 160 is not linked, its neighbors are.
    const Built b = build(1, 200, 0, 160);
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(170, 20));
    CHECK(seqs_are(p, 169, 20));
    for (size_t i = 0; i < p.seqs.size(); ++i)
      CHECK(p.linked[i] == (p.seqs[i] == 160 ? wb::Link::BROKEN : wb::Link::LINKED));
    CHECK(p.joins == wb::Link::LINKED);
  }
  {  // The card lacks record 170 (it was out then): the join is broken, and
     // 175 does not chain from 169 across the gap.
    const Built b = build(1, 200, 170);
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(170, 5));
    CHECK(seqs_are(p, 169, 5));
    CHECK(p.joins == wb::Link::BROKEN);
    CHECK(all_linked(p));
    const Page q = run(&s, io, req(172, 3));
    CHECK(q.seqs.size() == 3 && q.seqs[0] == 171 && q.seqs[1] == 169);
    CHECK(q.linked[0] == wb::Link::BROKEN);  // 171 -> 169: a seq gap
    CHECK(q.joins == wb::Link::LINKED);      // 172 chains from 171
  }
  {  // The card's history starts at 50 (inserted late) and is short: the
     // oldest record on it has nothing older to link to.
    const Built b = build(50, 60);
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(61, 30));
    CHECK(seqs_are(p, 60, 11));
    CHECK(!p.more);
    CHECK(p.linked.back() == wb::Link::NONE);
    CHECK(p.joins == wb::Link::BROKEN);      // 61 is not on the card
    const Page empty = run(&s, io, req(50, 5));
    CHECK(empty.seqs.empty() && !empty.more && empty.joins == wb::Link::NONE);
  }
  {  // A torn final line and a corrupt one are skipped and counted.
    Built b = build(1, 40);
    std::string file = b.file;
    const size_t at = file.find("\"ch\":\"", b.start[35]) + 6;
    file[at] = 'G';
    file += "{\"v\":1,\"seq\":41,\"tb\":";
    MemIo io;
    io.file = file;
    wb::Slot s;
    wb::init(&s);
    const Page p = run(&s, io, req(38, 5));
    CHECK(p.seqs.size() == 5 && p.seqs[0] == 37 && p.seqs[1] == 36 && p.seqs[2] == 34);
    CHECK(p.linked[1] == wb::Link::BROKEN);  // 36 -> 34 across the bad line
    CHECK(p.skipped == 1);
  }
  report("linkage_and_gaps", f0);
}

// Real threads: one loop task servicing the slot while three httpd tasks
// post, wait, give up and repost. Every page a waiter accepts must be the
// page of its own request — exactly what a single-threaded run returns.
static void test_threads() {
  const int f0 = g_fail;
  const Built b = build(1, 400);
  const uint32_t file_size = (uint32_t)b.file.size();

  // The single-threaded reference for every request the threads can make.
  struct Want { uint32_t before; uint8_t want; };
  std::vector<Want> asks;
  for (uint32_t before = 30; before <= 400; before += 37)
    for (uint8_t want = 1; want <= 30; want += 7) asks.push_back({before, want});
  std::vector<Page> refs;
  {
    MemIo io;
    io.file = b.file;
    wb::Slot s;
    wb::init(&s);
    for (const Want& a : asks) refs.push_back(run(&s, io, req(a.before, a.want)));
  }

  wb::Slot s;
  wb::init(&s);
  std::atomic<bool> stop(false);
  std::thread loop([&] {
    MemIo io;
    io.file = b.file;
    static char buf[wb::READ_LEN];
    while (!stop.load()) {
      wb::service(&s, io, buf);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  });

  std::atomic<int> pages(0), busy(0), timeouts(0), wrong(0);
  auto httpd = [&](unsigned seed) {
    for (int k = 0; k < 150; ++k) {
      seed = seed * 1103515245u + 12345u;
      const size_t idx = (seed >> 8) % asks.size();
      const bool impatient = ((seed >> 4) % 5) == 0;
      const uint32_t gen = wb::begin(&s, req(asks[idx].before, asks[idx].want));
      if (gen == 0) {
        busy++;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        continue;
      }
      const wb::Response* p = wb::wait(&s, gen, impatient ? 1 : 2000, 1, [](uint32_t ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      });
      if (p == nullptr) {
        timeouts++;
        wb::end(&s, gen, true);
        continue;
      }
      const Page got = copy_page(p);
      wb::end(&s, gen, false);
      const Page& ref = refs[idx];
      if (got.seqs != ref.seqs || got.linked != ref.linked || got.joins != ref.joins ||
          got.more != ref.more || got.next_hint != ref.next_hint)
        wrong++;
      pages++;
    }
  };
  std::thread h1(httpd, 1u), h2(httpd, 2u), h3(httpd, 3u);
  h1.join();
  h2.join();
  h3.join();
  stop = true;
  loop.join();

  std::printf("  threads: %d pages, %d busy, %d gave up, file %u bytes\n",
              pages.load(), busy.load(), timeouts.load(), (unsigned)file_size);
  CHECK(wrong.load() == 0);
  CHECK(pages.load() > 0);
  report("threads", f0);
}

int main() {
  test_first_card_page();
  test_hint_pages_to_the_start();
  test_hint_refusals();
  test_one_outstanding_request();
  test_abandoned_walk_stops_reading();
  test_late_completion_never_answers_the_next_request();
  test_wait_is_bounded();
  test_card_states();
  test_appends_between_passes();
  test_linkage_and_gaps();
  test_threads();
  if (g_fail) {
    std::fprintf(stderr, "\n%d WITNESS_HISTORY_BRIDGE CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL WITNESS_HISTORY_BRIDGE TESTS PASSED\n");
  return 0;
}
