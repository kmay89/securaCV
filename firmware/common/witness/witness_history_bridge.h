/**
 * @file witness_history_bridge.h
 * @brief The loop-task SD read bridge behind the timeline's card pages
 *        (repo sweep F35, F26 stage 2): one request slot between the httpd
 *        task and the Arduino loop task, a generation counter, a per-pass
 *        read budget and the resume-hint rules.
 *
 * Why a bridge: the storage contract (securacv_storage.h) makes the loop
 * task the single owner of all SD state, so GET /api/witness — which runs on
 * the httpd task — may not open /WITNESS/records.jsonl itself. It posts one
 * request here and waits, bounded; the loop walks the file backward with
 * witness_history.h a few reads per pass and publishes the page. Design and
 * decision record: docs/design/witness_history_bridge.md.
 *
 * Everything that decides something lives in this header so the handshake is
 * proven on the host (firmware/tests_host/test_witness_history_bridge.cpp,
 * deterministic interleavings plus a threaded run). The firmware glue
 * (firmware/canary/lib/securacv_witness/src/securacv_witness_history.cpp)
 * supplies only the SD calls, the task delay and one static Slot.
 *
 * The handshake:
 *   httpd task  begin()  claims the slot (0 = another request is outstanding:
 *                        the handler answers 503 history_busy), publishes the
 *                        request under a new generation;
 *               wait()   polls for THAT generation's page for at most
 *                        WAIT_MS (the handler answers 504 history_timeout);
 *               end()    frees the slot — after building the answer from the
 *                        page, or after the wait ran out (`gave_up`: the loop
 *                        drops the walk at its next pass, reading nothing
 *                        more for it).
 *   loop task   service() adopts the newest published generation (a newer
 *                        one replaces a walk in progress), then reads at most
 *                        READS_PER_PASS x READ_LEN bytes and, on a terminal
 *                        walker status, publishes the page under the
 *                        generation it was walked for.
 *
 * A late completion never answers the next request: a waiter accepts only a
 * page published under its own generation, and a walk whose request was
 * superseded or abandoned is not published at all.
 *
 * Ownership (who may touch what, and when):
 *   busy, req_lock, req_gen, done_gen, abandoned_gen  shared; moved only by
 *       GCC __atomic builtins (acquire/release), the pattern mesh_session's
 *       REST slot uses. 32-bit, so the Xtensa S32C1I compare-and-set covers
 *       them on the ESP32 and the ESP32-S3 alike.
 *   req   written by begin() and copied by the loop, each under req_lock; a
 *         side that finds the lock held does not wait (begin() answers busy,
 *         the loop tries again next pass), so neither task can stall the
 *         other.
 *   resp  written by the loop only while it walks a generation it adopted;
 *         read by the httpd side only once done_gen equals its own
 *         generation, while it still holds busy — and the loop adopts a new
 *         generation only after busy was released, so the two never overlap.
 *   everything else  loop task only.
 *
 * What a page says (rows are "from the card, chain-linked" — never
 * "verified": nothing here checks an Ed25519 signature or recomputes a chain
 * hash, so linkage is all a row can claim):
 *   rows[first .. first + n)  newest first; each row's seq < before_seq.
 *   linked[i]   LINKED when rows[i] chains from the next older record on the
 *               card (its prev is that record's ch and the seqs step by one),
 *               BROKEN when it does not (a gap or a break), NONE when the card
 *               holds no older readable record. Every page reads one row
 *               below its oldest so no returned row is left unchecked.
 *   joins       pages scanned from the file end also read the card's copy of
 *               the record AT before_seq (the one the client already shows):
 *               LINKED when it chains from the page's newest row, BROKEN when
 *               it does not or the card does not hold it, NONE when nothing
 *               older is on the card. A hint page needs no join: the previous
 *               page's oldest row was already checked against this page's
 *               newest.
 *   more        an older record remains on the card.
 *   next_hint   the oldest returned row's line start. The client echoes it
 *               as ?hint= with ?before= that row's seq, so the next page
 *               costs O(page), not O(distance into the file).
 *
 * The resume hint is client input and never trusted: a hint past the file's
 * size is refused before the first read, and any other hint is re-checked by
 * the walker on the bytes before it (HINT_MISMATCH); both fall back to a scan
 * from the file end and say so (hint_refused). A hint of 0 is the file's
 * start: an empty page, no read.
 *
 * Pure hosted C++11 (no Arduino / FreeRTOS): the Arduino-ESP32 2.0.17 core
 * compiles the canary tree as gnu++11.
 */

#ifndef WITNESS_HISTORY_BRIDGE_H
#define WITNESS_HISTORY_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "witness_history.h"

namespace witness_history_bridge {

// The httpd task's bounded wait for a page, and its poll step.
constexpr uint32_t WAIT_MS      = 3000;
constexpr uint32_t WAIT_STEP_MS = 10;

// The loop task's budget per pass: at most this many reads of READ_LEN bytes.
// A deep page takes a few passes, never one long stall — the task watchdog
// and the sensing cadence stay where they were.
constexpr size_t READS_PER_PASS = 4;
constexpr size_t READ_LEN       = witness_history::CHUNK;  // 1 KiB

// A page reads up to two records it does not return (the anchor above it on
// a scan from the end, the neighbor below its oldest row), so it returns at
// most PAGE_MAX - 2 rows.
constexpr size_t PAGE_ROWS_MAX = witness_history::PAGE_MAX - 2;

struct Request {
  uint32_t before_seq;  // exclusive bound; rows older than this (> 0)
  uint32_t hint;        // the client's resume hint (a byte offset), untrusted
  bool     has_hint;
  uint8_t  want;        // rows to return; begin() clamps it to 1..PAGE_ROWS_MAX
};

enum class Result : uint8_t {
  OK       = 0,  // a page (possibly empty: no history file, or nothing older)
  NO_CARD  = 1,  // no card mounted — nothing was read
  IO_ERROR = 2,  // a read failed, or the card or file changed under the page
};

enum class Link : int8_t {
  NONE   = -1,  // nothing older on the card to check against
  BROKEN = 0,
  LINKED = 1,
};

// What the loop task can find out about the card for this pass.
enum class Card : uint8_t {
  READY  = 0,  // mounted, no mount in flight
  BUSY   = 1,  // a background mount owns the SD object: touch nothing, retry
  ABSENT = 2,  // not mounted and nothing in flight
};

struct Response {
  Result   result;
  uint8_t  first;         // rows[first .. first + n) are the page
  uint8_t  n;
  bool     more;
  bool     hint_refused;  // a hint was given and refused: scanned from the end
  Link     joins;
  uint32_t next_hint;
  uint32_t skipped;       // unreadable lines skipped (bad + overlong)
  Link     linked[witness_history::PAGE_MAX];  // indexed like rows[]
  witness_history::HistoryRow rows[witness_history::PAGE_MAX];
};

struct Slot {
  // Shared by both tasks: __atomic builtins only.
  uint32_t busy;           // 1 while an httpd request is outstanding
  uint32_t req_lock;       // 1 while one side copies `req`
  uint32_t req_gen;        // the newest published request (0 = none yet)
  uint32_t done_gen;       // the generation whose page sits in `resp`
  uint32_t abandoned_gen;  // a waiter of this generation gave up
  Request  req;
  Response resp;
  // Loop task only.
  uint32_t work_gen;       // the last generation adopted
  bool     active;         // work_gen is being walked
  bool     started;        // the file was opened and the start chosen
  bool     from_hint;      // the walk runs in hint mode
  Request  work;
  uint32_t file_size;      // the file's size when the walk started
  uint32_t card_token;     // the mount the walk started on
  uint32_t reads;          // reads spent on work_gen (tests, diagnostics)
  witness_history::BackScanner scan;
};

/** An all-zero Slot is the idle state (a static one needs no call). */
inline void init(Slot* s) { memset(s, 0, sizeof(*s)); }

namespace detail {

inline uint32_t load(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void store(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline bool cas(uint32_t* p, uint32_t from, uint32_t to) {
  return __atomic_compare_exchange_n(p, &from, to, /*weak=*/false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

}  // namespace detail

// ── httpd task ──────────────────────────────────────────────────────────────

/**
 * Claim the slot and publish `r`. Returns the request's generation (never 0),
 * or 0 when another request is outstanding — the handler answers
 * 503 history_busy and must not call end().
 */
inline uint32_t begin(Slot* s, const Request& r) {
  if (!detail::cas(&s->busy, 0, 1)) return 0;
  if (!detail::cas(&s->req_lock, 0, 1)) {
    // The loop is copying the previous request this instant (its waiter gave
    // up while the loop was stalled). Busy, not a wait: never stall httpd.
    detail::store(&s->busy, 0);
    return 0;
  }
  s->req = r;
  if (s->req.want == 0) s->req.want = 1;
  if (s->req.want > PAGE_ROWS_MAX) s->req.want = (uint8_t)PAGE_ROWS_MAX;
  uint32_t gen = __atomic_load_n(&s->req_gen, __ATOMIC_RELAXED) + 1;
  // Skip 0 (never a generation) and whatever done_gen holds, so a page left
  // from 2^32 requests ago can never pass for this one.
  while (gen == 0 || gen == detail::load(&s->done_gen)) ++gen;
  detail::store(&s->req_gen, gen);
  detail::store(&s->req_lock, 0);
  return gen;
}

/** The page for `gen`, or nullptr while it is not ready. Valid until end(). */
inline const Response* poll(const Slot* s, uint32_t gen) {
  return (gen != 0 && detail::load(&s->done_gen) == gen) ? &s->resp : nullptr;
}

/**
 * Wait at most `budget_ms` for the page of `gen`, sleeping `step_ms` at a time
 * through `sleep_ms(ms)` (vTaskDelay on the device). nullptr when the budget
 * ran out.
 */
template <class SleepFn>
inline const Response* wait(const Slot* s, uint32_t gen, uint32_t budget_ms,
                            uint32_t step_ms, SleepFn sleep_ms) {
  if (step_ms == 0) step_ms = 1;
  uint32_t waited = 0;
  for (;;) {
    const Response* page = poll(s, gen);
    if (page != nullptr) return page;
    if (waited >= budget_ms) return nullptr;
    const uint32_t d = (budget_ms - waited < step_ms) ? budget_ms - waited : step_ms;
    sleep_ms(d);
    waited += d;
  }
}

/**
 * Done with generation `gen`: after the answer was built from its page, or
 * with `gave_up` after the wait ran out (the loop then drops the walk).
 * Frees the slot for the next request.
 */
inline void end(Slot* s, uint32_t gen, bool gave_up) {
  if (gave_up) detail::store(&s->abandoned_gen, gen);
  detail::store(&s->busy, 0);
}

// ── loop task ───────────────────────────────────────────────────────────────

namespace detail {

// Take the newest published request if it is not the one being walked. A
// newer generation replaces a walk in progress: that walk's waiter is gone
// (a new request could only be published after it released busy).
inline void adopt(Slot* s) {
  if (load(&s->req_gen) == s->work_gen) return;
  if (!cas(&s->req_lock, 0, 1)) return;  // begin() is publishing: next pass
  const uint32_t gen = __atomic_load_n(&s->req_gen, __ATOMIC_RELAXED);
  s->work = s->req;
  store(&s->req_lock, 0);
  s->work_gen = gen;
  s->active = true;
  s->started = false;
  s->from_hint = false;
  s->reads = 0;
}

// Publish the page under the generation it was walked for — unless that
// request was superseded or abandoned while this pass ran: its waiter is
// gone, and nobody else accepts another generation's page anyway.
inline void publish(Slot* s) {
  s->active = false;
  if (load(&s->req_gen) != s->work_gen) return;
  if (load(&s->abandoned_gen) == s->work_gen) return;
  store(&s->done_gen, s->work_gen);
}

inline void fail(Slot* s, Result why) {
  Response& r = s->resp;
  r.result = why;
  r.first = 0;
  r.n = 0;
  r.more = false;
  r.joins = Link::NONE;
  r.next_hint = 0;
  publish(s);
}

// Scan from the file end: rows with seq <= before_seq, so the card's copy of
// the record AT before_seq (the anchor) is read too, plus one neighbor below
// the page.
inline void start_from_end(Slot* s) {
  s->from_hint = false;
  const uint32_t b = s->work.before_seq;
  const uint32_t bound = (b == 0 || b == UINT32_MAX) ? 0 : b + 1;
  witness_history::init(&s->scan, s->file_size, bound, false, s->resp.rows,
                        (size_t)s->work.want + 2);
}

// Pick the walker's start before the first read. The walker cannot see the
// file size, so a hint past it is refused here; any other hint goes to the
// walker, which re-checks it on the bytes before it. A hint of 0 is the start
// of the file: the walker answers AT_START with no rows and no read.
inline void plan(Slot* s) {
  Response& r = s->resp;
  r.result = Result::OK;
  r.first = 0;
  r.n = 0;
  r.more = false;
  r.hint_refused = false;
  r.joins = Link::NONE;
  r.next_hint = 0;
  r.skipped = 0;
  const Request& q = s->work;
  if (q.has_hint && q.hint <= s->file_size) {
    s->from_hint = true;
    witness_history::init(&s->scan, q.hint, q.before_seq, true, r.rows,
                          (size_t)q.want + 1);
  } else {
    r.hint_refused = q.has_hint;
    start_from_end(s);
  }
}

inline Link link_of(bool ok) { return ok ? Link::LINKED : Link::BROKEN; }

inline void finish_page(Slot* s) {
  Response& r = s->resp;
  const Request& q = s->work;
  const size_t cnt = witness_history::count(&s->scan);
  size_t first = 0;
  if (!s->from_hint && q.before_seq != 0) {
    if (cnt > 0 && r.rows[0].seq == q.before_seq) first = 1;  // the anchor
    if (cnt > first) {
      r.joins = (first == 1) ? link_of(witness_history::links_to(r.rows[0], r.rows[1]))
                             : Link::BROKEN;  // the card does not hold before_seq
    }
  }
  size_t n = cnt - first;
  if (n > q.want) n = q.want;
  for (size_t i = first; i < first + n; ++i) {
    r.linked[i] = (i + 1 < cnt) ? link_of(witness_history::links_to(r.rows[i], r.rows[i + 1]))
                                : Link::NONE;
  }
  r.first = (uint8_t)first;
  r.n = (uint8_t)n;
  r.more = cnt > first + n;
  r.next_hint = n ? r.rows[first + n - 1].offset : 0;
  r.skipped = s->scan.bad_lines + s->scan.overlong_lines;
  r.result = Result::OK;
  publish(s);
}

}  // namespace detail

/**
 * One loop pass. `io` is the SD adapter, `buf` a READ_LEN scratch buffer:
 *
 *   Card     card();                  READY / BUSY / ABSENT (see Card)
 *   uint32_t card_token();            changes on every (re)mount
 *   bool     open(uint32_t* size);    open the history file for this pass
 *   size_t   read(uint32_t off, char* buf, size_t len);  bytes read at off
 *   void     close();
 *
 * The file is opened once per pass and closed before returning; nothing is
 * opened when there is no request, no card, or a mount in flight.
 */
template <class Io>
inline void service(Slot* s, Io& io, char* buf) {
  detail::adopt(s);
  if (!s->active) return;
  if (detail::load(&s->abandoned_gen) == s->work_gen) {
    s->active = false;  // the waiter gave up: read nothing more for it
    return;
  }
  const Card card = io.card();
  if (card == Card::BUSY) return;
  if (card == Card::ABSENT) {
    detail::fail(s, Result::NO_CARD);
    return;
  }

  uint32_t size = 0;
  if (!s->started) {
    s->started = true;
    s->card_token = io.card_token();
    if (!io.open(&size)) {  // no history file: an empty page
      s->file_size = 0;
      detail::plan(s);
      detail::finish_page(s);
      return;
    }
    s->file_size = size;
    detail::plan(s);
  } else {
    if (io.card_token() != s->card_token) {  // remounted or swapped mid-page
      detail::fail(s, Result::IO_ERROR);
      return;
    }
    if (!io.open(&size)) {
      detail::fail(s, Result::IO_ERROR);
      return;
    }
    if (size < s->file_size) {  // the file shrank: not the file we started on
      io.close();
      detail::fail(s, Result::IO_ERROR);
      return;
    }
  }

  for (size_t k = 0; k < READS_PER_PASS; ++k) {
    uint32_t start = 0;
    const size_t len = witness_history::next_chunk(&s->scan, READ_LEN, &start);
    if (len == 0) break;
    if (io.read(start, buf, len) != len) {
      io.close();
      detail::fail(s, Result::IO_ERROR);
      return;
    }
    s->reads++;
    witness_history::feed(&s->scan, buf, len);
  }
  io.close();

  switch (s->scan.status) {
    case witness_history::Status::RUNNING:
      return;  // more (older) bytes next pass
    case witness_history::Status::HINT_MISMATCH:
      s->resp.hint_refused = true;  // untrusted and wrong: scan from the end
      detail::start_from_end(s);
      return;
    case witness_history::Status::PAGE_FULL:
    case witness_history::Status::AT_START:
      detail::finish_page(s);
      return;
  }
}

}  // namespace witness_history_bridge

#endif  // WITNESS_HISTORY_BRIDGE_H
