/**
 * @file witness_history.h
 * @brief Pure backward walker over /WITNESS/records.jsonl: timeline pages
 *        deeper than the 32-record RAM ring (repo sweep F26, stage 1).
 *
 * The web timeline pages through the in-RAM witness ring (32 records). Full
 * history lives on the card, one witness_store::line_build line per record,
 * appended oldest-first by the loop task. Reading it from an HTTP request
 * means a bridge to the loop task (the storage contract makes the loop the
 * single SD owner) — designed in docs/design/witness_history_bridge.md and
 * NOT built yet. This header is the part of that design that can be proven
 * without hardware: given the file one chunk at a time, NEWEST bytes first,
 * produce the rows of one page, newest first.
 *
 * Contract:
 *   - The caller reads chunks backward: each feed() gets the bytes
 *     [pos - len, pos) where pos is the scanner's current position
 *     (next_chunk() computes the range). Any chunk size works — a line, a
 *     hex field, even a single byte may be split across chunks; the
 *     scanner stitches.
 *   - Only COMPLETE lines count: bytes after the last '\n' before the start
 *     position are a torn append (power cut) and are skipped, exactly as
 *     witness_store::tail_parse does. A line longer than RECORD_LINE_MAX is
 *     skipped without overflowing, and a line that fails
 *     witness_store::line_parse (or carries no "type") is skipped and
 *     counted — one bad line never ends the page.
 *   - before_seq bounds the page: only rows with seq < before_seq are
 *     returned (0 = no bound, start from the newest).
 *   - Resuming: next_hint() is the byte offset of the oldest returned row's
 *     line start. A client echoes it with before_seq = that row's seq, and
 *     the next page starts there — O(page), not O(distance). A hint is
 *     client input and never trusted: in hint mode the start must sit just
 *     after a '\n' (no torn bytes before it) and the first complete line
 *     found must be seq == before_seq - 1, or the scan stops with
 *     HINT_MISMATCH and the caller falls back to a scan from the file end.
 *   - The walker checks format and chain LINKAGE only (link_check: each
 *     row's prev equals the next-older row's ch, seqs contiguous). It does
 *     not verify Ed25519 signatures or recompute chain hashes, so rows
 *     from here are "from the card, chain-linked" — never "verified".
 *
 * Pure hosted C++ (no Arduino/ESP-IDF). Host test:
 * firmware/tests_host/test_witness_history.cpp, against files built with
 * witness_store::line_build. Canonical here; the PIO canary tree would
 * include it via -I ../common when the bridge lands.
 */

#ifndef WITNESS_HISTORY_H
#define WITNESS_HISTORY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "witness_store.h"

namespace witness_history {

// A page is at most the ring's size — the timeline asks for ?last=N ≤ 32.
constexpr size_t PAGE_MAX = 32;

// One read per loop pass is a bounded chunk; the walker does not care, but
// the bridge's per-pass budget is expressed in these.
constexpr size_t CHUNK = 1024;

struct HistoryRow {
  uint32_t seq;
  uint32_t tb;
  uint8_t  type;
  uint8_t  ch[32];
  uint8_t  prev[32];
  uint32_t offset;   // byte offset of this row's line start in the file
};

enum class Status : uint8_t {
  RUNNING       = 0,  // needs more (older) bytes
  PAGE_FULL     = 1,  // `want` rows filled — next_hint() resumes
  AT_START      = 2,  // reached byte 0: the page holds everything older
  HINT_MISMATCH = 3,  // hint mode and the bytes before the hint are not the
                      // record before before_seq — rescan from the end
};

struct BackScanner {
  // The line being assembled, filled right to left: its bytes are
  // line[lpos .. RECORD_LINE_MAX), and line[RECORD_LINE_MAX] is always NUL
  // so witness_store::line_parse (strstr-based) never reads past it.
  char        line[witness_store::RECORD_LINE_MAX + 1];
  size_t      lpos;
  bool        have_end;    // a '\n' terminating the current line was seen
  bool        overlong;    // the current line outgrew the buffer: skip it
  uint32_t    pos;         // file offset where the next (older) chunk ends
  uint32_t    before_seq;  // 0 = unbounded
  bool        from_hint;
  bool        first_line;  // hint mode: the next complete line is the first
  HistoryRow* rows;
  size_t      want;
  size_t      n;
  Status      status;
  uint32_t    bad_lines;      // complete lines that failed to parse
  uint32_t    torn_bytes;     // bytes of a torn append skipped at the start
  uint32_t    overlong_lines; // lines over RECORD_LINE_MAX skipped
};

/**
 * Start a page. `end_offset` is the file size (scan from the newest line) or
 * a client hint (a line start). `rows` must hold `want` (≤ PAGE_MAX) rows.
 */
inline void init(BackScanner* s, uint32_t end_offset, uint32_t before_seq,
                 bool from_hint, HistoryRow* rows, size_t want) {
  memset(s, 0, sizeof(*s));
  s->lpos = witness_store::RECORD_LINE_MAX;
  s->line[witness_store::RECORD_LINE_MAX] = '\0';
  s->pos = end_offset;
  s->before_seq = before_seq;
  s->from_hint = from_hint;
  s->first_line = true;
  s->rows = rows;
  s->want = want > PAGE_MAX ? PAGE_MAX : want;
  s->status = (s->want == 0) ? Status::PAGE_FULL
            : (end_offset == 0) ? Status::AT_START
            : Status::RUNNING;
}

/**
 * The next read: sets *start and returns its length (≤ max_len), covering
 * [*start, pos). Returns 0 when the scan is over.
 */
inline size_t next_chunk(const BackScanner* s, size_t max_len, uint32_t* start) {
  if (s->status != Status::RUNNING || s->pos == 0 || max_len == 0) {
    *start = s->pos;
    return 0;
  }
  const uint32_t len = (s->pos > max_len) ? (uint32_t)max_len : s->pos;
  *start = s->pos - len;
  return len;
}

namespace detail {

inline void reset_line(BackScanner* s) {
  s->lpos = witness_store::RECORD_LINE_MAX;
  s->overlong = false;
}

// A complete line whose first byte is at file offset `start` ends here.
inline void emit(BackScanner* s, uint32_t start) {
  const char* line = s->line + s->lpos;
  const char* end = s->line + witness_store::RECORD_LINE_MAX;
  const bool first = s->first_line;
  s->first_line = false;

  if (s->overlong) {
    s->overlong_lines++;
    if (s->from_hint && first) s->status = Status::HINT_MISMATCH;
    return;
  }
  if (line == end) {         // an empty line (a bare "\n") is not a record
    if (s->from_hint && first) s->status = Status::HINT_MISMATCH;
    return;
  }

  witness_store::TailRecord rec;
  uint32_t type = 0;
  if (!witness_store::line_parse(line, end, &rec) ||
      !witness_store::parse_u32_field(line, end, "\"type\":", 7, &type) ||
      type > 0xFF) {
    s->bad_lines++;
    if (s->from_hint && first) s->status = Status::HINT_MISMATCH;
    return;
  }
  if (s->from_hint && first &&
      (s->before_seq == 0 || rec.seq != s->before_seq - 1)) {
    s->status = Status::HINT_MISMATCH;
    return;
  }
  if (s->before_seq != 0 && rec.seq >= s->before_seq) return;

  HistoryRow& r = s->rows[s->n++];
  r.seq = rec.seq;
  r.tb = rec.tb;
  r.type = (uint8_t)type;
  memcpy(r.ch, rec.ch, 32);
  memcpy(r.prev, rec.prev, 32);
  r.offset = start;
  if (s->n >= s->want) s->status = Status::PAGE_FULL;
}

}  // namespace detail

/**
 * Feed the chunk [pos - len, pos). Returns the status after it. A chunk that
 * does not end at the scanner's position is a caller bug and is ignored
 * (status unchanged, nothing consumed).
 */
inline Status feed(BackScanner* s, const char* chunk, size_t len) {
  if (s->status != Status::RUNNING) return s->status;
  if (chunk == nullptr || len == 0 || len > s->pos) return s->status;
  const uint32_t chunk_start = s->pos - (uint32_t)len;

  for (size_t k = len; k-- > 0;) {
    const char c = chunk[k];
    if (c == '\n') {
      const uint32_t line_start = chunk_start + (uint32_t)k + 1;
      if (s->have_end) {
        detail::emit(s, line_start);
      } else {
        // Bytes between this '\n' and the scan start belong to no complete
        // line: a torn append at the file end, or — in hint mode — proof
        // the hint is not a line start.
        const uint32_t torn = (uint32_t)(witness_store::RECORD_LINE_MAX - s->lpos);
        if (torn != 0 || s->overlong) {
          s->torn_bytes += torn;
          if (s->from_hint) s->status = Status::HINT_MISMATCH;
        }
      }
      s->have_end = true;
      detail::reset_line(s);
      if (s->status != Status::RUNNING) {
        // Stopped mid-chunk: resume (a fresh page) from this line's start.
        s->pos = line_start;
        return s->status;
      }
      continue;
    }
    if (s->lpos == 0) {
      s->overlong = true;  // keep scanning for the '\n' that ends it
    } else if (!s->overlong) {
      s->line[--s->lpos] = c;
    }
  }
  s->pos = chunk_start;

  if (s->pos == 0) {
    // The file's first line has no '\n' before it.
    if (s->have_end) {
      detail::emit(s, 0);
    } else {
      const uint32_t torn = (uint32_t)(witness_store::RECORD_LINE_MAX - s->lpos);
      s->torn_bytes += torn;
      if (s->from_hint && (torn != 0 || s->overlong)) s->status = Status::HINT_MISMATCH;
    }
    if (s->status == Status::RUNNING) s->status = Status::AT_START;
  }
  return s->status;
}

inline size_t count(const BackScanner* s) { return s->n; }

/**
 * Where the next page starts: the oldest returned row's line start. With no
 * rows, the scan position (the file end, or byte 0 when exhausted).
 */
inline uint32_t next_hint(const BackScanner* s) {
  return s->n ? s->rows[s->n - 1].offset : s->pos;
}

/**
 * Chain linkage across a newest-first page: every row's prev is the
 * next-older row's ch and the seqs step down by one. True for 0 or 1 rows.
 * Linkage, not verification — no signature is checked here.
 */
inline bool link_check(const HistoryRow* rows, size_t n) {
  for (size_t i = 0; i + 1 < n; ++i) {
    if (rows[i].seq != rows[i + 1].seq + 1) return false;
    if (memcmp(rows[i].prev, rows[i + 1].ch, 32) != 0) return false;
  }
  return true;
}

/* The newer page's oldest row chains from the older page's newest row (or
 * the SD page's newest row from the ring's oldest): `newer.prev == older.ch`
 * and consecutive seqs. */
inline bool links_to(const HistoryRow& newer, const HistoryRow& older) {
  return newer.seq == older.seq + 1 && memcmp(newer.prev, older.ch, 32) == 0;
}

}  // namespace witness_history

#endif  // WITNESS_HISTORY_H
