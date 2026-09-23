// Host test for firmware/common/witness/witness_history.h (repo sweep F26,
// stage 1): the pure backward walker the loop-task SD bridge will run over
// /WITNESS/records.jsonl. Every file here is built with the firmware's own
// witness_store::line_build, so the walker is tested against the exact bytes
// the device appends.
//
// Run: make -C firmware/tests_host

#include "witness_history.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// Deterministic chain: record i's ch is a function of i, its prev is record
// i-1's ch (zeros for the first) — the linkage the walker checks. (No real
// hashing: the walker checks linkage and format, not SHA-256.)
static void ch_for(uint32_t i, uint8_t out[32]) {
  for (int j = 0; j < 32; ++j) out[j] = (uint8_t)((i * 131u + (uint32_t)j * 17u + 5u) & 0xFF);
}

struct Built {
  std::string file;
  std::vector<uint32_t> line_start;  // by seq index
};

// Records seq first..first+count-1, type = seq % 7 + 1, tb = seq % 144.
static Built build_file(uint32_t first, uint32_t count) {
  Built b;
  char buf[ws::RECORD_LINE_MAX];
  for (uint32_t k = 0; k < count; ++k) {
    const uint32_t seq = first + k;
    uint8_t ph[32], prev[32], ch[32], sig[64];
    for (int j = 0; j < 32; ++j) ph[j] = (uint8_t)(seq ^ (uint32_t)j);
    if (seq == 0) std::memset(prev, 0, 32); else ch_for(seq - 1, prev);
    ch_for(seq, ch);
    for (int j = 0; j < 64; ++j) sig[j] = (uint8_t)(seq * 3u + (uint32_t)j);
    const size_t n = ws::line_build(buf, sizeof(buf), seq, seq % 144, (uint8_t)(seq % 7 + 1),
                                    ph, prev, ch, sig);
    CHECK(n > 0);
    b.line_start.push_back((uint32_t)b.file.size());
    b.file.append(buf, n);
  }
  return b;
}

struct Page {
  wh::Status status;
  std::vector<wh::HistoryRow> rows;
  uint32_t hint;
  uint32_t bad_lines, torn_bytes, overlong_lines;
  size_t reads;
};

// Drive one page the way the bridge will: next_chunk -> read -> feed.
static Page run_page(const std::string& file, uint32_t end, uint32_t before_seq,
                     bool from_hint, size_t want, size_t chunk) {
  wh::HistoryRow rows[wh::PAGE_MAX];
  wh::BackScanner s;
  wh::init(&s, end, before_seq, from_hint, rows, want);
  Page p{};
  uint32_t start = 0;
  for (size_t len; (len = wh::next_chunk(&s, chunk, &start)) != 0;) {
    wh::feed(&s, file.data() + start, len);
    p.reads++;
  }
  p.status = s.status;
  p.rows.assign(rows, rows + wh::count(&s));
  p.hint = wh::next_hint(&s);
  p.bad_lines = s.bad_lines;
  p.torn_bytes = s.torn_bytes;
  p.overlong_lines = s.overlong_lines;
  return p;
}

static bool rows_are(const Page& p, uint32_t newest, size_t n) {
  if (p.rows.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    const wh::HistoryRow& r = p.rows[i];
    const uint32_t seq = newest - (uint32_t)i;
    uint8_t ch[32];
    ch_for(seq, ch);
    if (r.seq != seq || r.tb != seq % 144 || r.type != seq % 7 + 1) return false;
    if (std::memcmp(r.ch, ch, 32) != 0) return false;
  }
  return true;
}

static void test_newest_page() {
  const int f0 = g_fail;
  const Built b = build_file(0, 200);
  const Page p = run_page(b.file, (uint32_t)b.file.size(), 0, false, 32, wh::CHUNK);
  CHECK(p.status == wh::Status::PAGE_FULL);
  CHECK(rows_are(p, 199, 32));
  CHECK(wh::link_check(p.rows.data(), p.rows.size()));
  CHECK(p.hint == b.line_start[168]);                 // the oldest returned row's line
  CHECK(p.rows.back().offset == b.line_start[168]);
  CHECK(p.bad_lines == 0 && p.torn_bytes == 0);
  // Bounded work: exactly the chunks that cover the page (~390-byte lines,
  // so 32 rows are ~12.5 KiB = 13 reads of 1 KiB) and not one more — the
  // page ends at the '\n' before its oldest row.
  const size_t span = b.file.size() - (b.line_start[168] - 1);
  CHECK(p.reads == (span + wh::CHUNK - 1) / wh::CHUNK);
  report("newest_page", f0);
}

static void test_hint_pages_walk_to_the_start() {
  const int f0 = g_fail;
  const Built b = build_file(0, 200);
  uint32_t end = (uint32_t)b.file.size(), before = 0;
  bool hint = false;
  std::vector<uint32_t> seen;
  wh::HistoryRow prev_oldest{};
  bool have_prev = false;
  for (int page = 0; page < 20; ++page) {
    const Page p = run_page(b.file, end, before, hint, 32, wh::CHUNK);
    CHECK(p.status == wh::Status::PAGE_FULL || p.status == wh::Status::AT_START);
    CHECK(wh::link_check(p.rows.data(), p.rows.size()));
    if (have_prev && !p.rows.empty()) CHECK(wh::links_to(prev_oldest, p.rows.front()));
    for (const auto& r : p.rows) seen.push_back(r.seq);
    if (!p.rows.empty()) { prev_oldest = p.rows.back(); have_prev = true; }
    if (p.status == wh::Status::AT_START) {
      CHECK(p.rows.size() == 200 % 32);                 // the short last page
      break;
    }
    end = p.hint;
    before = p.rows.back().seq;
    hint = true;
  }
  CHECK(seen.size() == 200);
  for (size_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == 199 - i);
  report("hint_pages_walk_to_the_start", f0);
}

static void test_chunk_size_independence() {
  const int f0 = g_fail;
  const Built b = build_file(1000, 60);
  const Page ref = run_page(b.file, (uint32_t)b.file.size(), 0, false, 32, wh::CHUNK);
  CHECK(rows_are(ref, 1059, 32));
  // Chunks that split lines, hex fields and single bytes everywhere.
  for (size_t chunk : {1u, 2u, 7u, 64u, 333u, 511u, 512u, 513u, 4096u, 100000u}) {
    const Page p = run_page(b.file, (uint32_t)b.file.size(), 0, false, 32, chunk);
    if (!rows_are(p, 1059, 32) || p.hint != ref.hint) {
      std::fprintf(stderr, "FAIL: chunk=%zu disagrees\n", chunk);
      g_fail++;
    }
  }
  report("chunk_size_independence", f0);
}

static void test_torn_final_line_skipped() {
  const int f0 = g_fail;
  Built b = build_file(0, 50);
  const std::string torn = "{\"v\":1,\"seq\":50,\"tb\":50,\"type\":2,\"ph\":\"ab";  // power cut
  const std::string file = b.file + torn;
  const Page p = run_page(file, (uint32_t)file.size(), 0, false, 5, 100);
  CHECK(p.status == wh::Status::PAGE_FULL);
  CHECK(rows_are(p, 49, 5));
  CHECK(p.torn_bytes == torn.size());
  report("torn_final_line_skipped", f0);
}

static void test_before_seq_without_hint() {
  const int f0 = g_fail;
  const Built b = build_file(0, 200);
  const Page p = run_page(b.file, (uint32_t)b.file.size(), 100, false, 32, wh::CHUNK);
  CHECK(p.status == wh::Status::PAGE_FULL);
  CHECK(rows_are(p, 99, 32));                           // strictly older than 100
  const Page q = run_page(b.file, (uint32_t)b.file.size(), 5, false, 32, wh::CHUNK);
  CHECK(q.status == wh::Status::AT_START);
  CHECK(rows_are(q, 4, 5));
  report("before_seq_without_hint", f0);
}

static void test_bad_hints_rejected() {
  const int f0 = g_fail;
  const Built b = build_file(0, 100);
  const uint32_t good = b.line_start[60];
  // The good hint: before_seq 60 at line 60's start.
  const Page ok = run_page(b.file, good, 60, true, 10, wh::CHUNK);
  CHECK(ok.status == wh::Status::PAGE_FULL);
  CHECK(rows_are(ok, 59, 10));
  // Mid-line (a torn view): refused.
  const Page mid = run_page(b.file, good + 17, 60, true, 10, wh::CHUNK);
  CHECK(mid.status == wh::Status::HINT_MISMATCH);
  CHECK(mid.rows.empty());
  // A real line start, but the wrong seq for it: refused, not trusted.
  const Page wrong = run_page(b.file, b.line_start[40], 60, true, 10, wh::CHUNK);
  CHECK(wrong.status == wh::Status::HINT_MISMATCH);
  CHECK(wrong.rows.empty());
  // before_seq 0 with a hint makes no sense: refused.
  const Page zero = run_page(b.file, good, 0, true, 10, wh::CHUNK);
  CHECK(zero.status == wh::Status::HINT_MISMATCH);
  // A hint inside the file's first line (no '\n' before it): refused.
  const Page first = run_page(b.file, 9, 1, true, 10, wh::CHUNK);
  CHECK(first.status == wh::Status::HINT_MISMATCH);
  // A hint past the end of the file is the caller's to refuse (it knows the
  // size); the walker never reads outside the chunk it is handed.
  report("bad_hints_rejected", f0);
}

static void test_corrupt_and_overlong_lines() {
  const int f0 = g_fail;
  Built b = build_file(0, 30);
  // Corrupt record 20's ch hex (a non-hex byte) and insert an overlong
  // garbage line after record 10.
  std::string file = b.file;
  const size_t ch_at = file.find("\"ch\":\"", b.line_start[20]) + 6;
  file[ch_at] = 'G';
  const std::string junk(ws::RECORD_LINE_MAX + 200, 'x');
  file.insert(b.line_start[11], junk + "\n");
  const Page p = run_page(file, (uint32_t)file.size(), 0, false, 32, 97);
  CHECK(p.status == wh::Status::AT_START);
  CHECK(p.rows.size() == 29);                           // 30 records, one corrupt
  CHECK(p.bad_lines == 1);
  CHECK(p.overlong_lines == 1);
  bool saw20 = false;
  for (const auto& r : p.rows) saw20 |= (r.seq == 20);
  CHECK(!saw20);
  CHECK(!wh::link_check(p.rows.data(), p.rows.size())); // the gap is visible
  report("corrupt_and_overlong_lines", f0);
}

static void test_edges() {
  const int f0 = g_fail;
  // Empty file: nothing to read, nothing returned.
  const Page empty = run_page(std::string(), 0, 0, false, 32, wh::CHUNK);
  CHECK(empty.status == wh::Status::AT_START && empty.rows.empty() && empty.reads == 0);
  // One record, no trailing junk.
  const Built one = build_file(7, 1);
  const Page p1 = run_page(one.file, (uint32_t)one.file.size(), 0, false, 32, 5);
  CHECK(p1.status == wh::Status::AT_START && rows_are(p1, 7, 1));
  CHECK(p1.rows[0].offset == 0);
  // A file that is one torn line and nothing else.
  const std::string torn = "{\"v\":1,\"seq\":0";
  const Page pt = run_page(torn, (uint32_t)torn.size(), 0, false, 32, 4);
  CHECK(pt.status == wh::Status::AT_START && pt.rows.empty() && pt.torn_bytes == torn.size());
  // want is capped at PAGE_MAX; want 0 reads nothing.
  const Built b = build_file(0, 80);
  const Page big = run_page(b.file, (uint32_t)b.file.size(), 0, false, 500, wh::CHUNK);
  CHECK(big.rows.size() == wh::PAGE_MAX);
  const Page none = run_page(b.file, (uint32_t)b.file.size(), 0, false, 0, wh::CHUNK);
  CHECK(none.rows.empty() && none.reads == 0);
  // A chunk that does not end at the scan position is ignored.
  wh::HistoryRow rows[4];
  wh::BackScanner s;
  wh::init(&s, (uint32_t)b.file.size(), 0, false, rows, 4);
  CHECK(wh::feed(&s, b.file.data(), b.file.size() + 1) == wh::Status::RUNNING);
  CHECK(wh::count(&s) == 0);
  report("edges", f0);
}

static void test_link_check() {
  const int f0 = g_fail;
  const Built b = build_file(0, 10);
  const Page p = run_page(b.file, (uint32_t)b.file.size(), 0, false, 10, wh::CHUNK);
  CHECK(wh::link_check(p.rows.data(), p.rows.size()));
  CHECK(wh::link_check(p.rows.data(), 1));
  CHECK(wh::link_check(p.rows.data(), 0));
  std::vector<wh::HistoryRow> r = p.rows;
  r[3].prev[0] ^= 1;                                     // broken linkage
  CHECK(!wh::link_check(r.data(), r.size()));
  r = p.rows;
  r[4].seq += 1;                                         // a seq gap
  CHECK(!wh::link_check(r.data(), r.size()));
  CHECK(wh::links_to(p.rows[0], p.rows[1]));
  CHECK(!wh::links_to(p.rows[1], p.rows[0]));
  report("link_check", f0);
}

int main() {
  test_newest_page();
  test_hint_pages_walk_to_the_start();
  test_chunk_size_independence();
  test_torn_final_line_skipped();
  test_before_seq_without_hint();
  test_bad_hints_rejected();
  test_corrupt_and_overlong_lines();
  test_edges();
  test_link_check();
  if (g_fail) {
    std::fprintf(stderr, "\n%d WITNESS_HISTORY CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL WITNESS_HISTORY TESTS PASSED\n");
  return 0;
}
