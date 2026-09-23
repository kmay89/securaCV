# Timeline history deeper than the ring — the loop-task SD read bridge

Status: **Stage 1 built (the pure walker, host-tested). Stage 2 built (F35):
the bridge's state machine is a pure, host-tested header; the ESP glue, the
`handle_witness` extension and the timeline UI compile in CI's canary legs,
with no bench pass yet** (U1: a card holding more than 32 records).
Decision: **F26: option B (staged) — maintainer to confirm.**
Scope: the canary PlatformIO tree (`firmware/canary`). Repo sweep items F26
and F35.
Last Updated: 2026-09-23

## The gap

Before F35, the web timeline paged through `GET /api/witness?last=N&before=SEQ`,
and `handle_witness` (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`)
served only the in-RAM ring: 32 records (`WITNESS_RECORD_RING_SIZE`,
`securacv_witness.cpp`), portMUX-guarded. At one record a second that is
about half a minute of history. "Load More" stopped at the ring's oldest
record even though every signed record is also on the card, one line each
in `/WITNESS/records.jsonl` (`witness_store::line_build`, appended by
`sd_append_record` on the loop task). `/api/export` does not read the card
either — it returns metadata only. So the only way to look further back
was to pull the card or use the export/unseal tools.

The obstacle is not the file format. It is the threading contract in
`securacv_storage.h`: the Arduino loop task is the single owner of all SD
state (witness appends included); the only other reader is USB MSC, whose
raw-sector access is why teardown is policy-gated. An HTTP handler runs on
the httpd task, so it may not open the file itself.

## Options that were weighed

| | Option | Verdict |
| --- | --- | --- |
| A | Leave deep history to the export and unseal tools (status quo) | honest, but the owner's own dashboard stops at 32 |
| **B** | **A loop-task read bridge**: the HTTP handler posts one request and waits, bounded; the loop walks the file backward a chunk at a time across passes | **recommended — staged** |
| C | Let the httpd task read the card under a new mutex | rejected: breaks the single-owner contract the mount worker, remount recovery and the MSC gating all assume |

## Stage 1 — the walker (built)

`firmware/common/witness/witness_history.h`, host test
`firmware/tests_host/test_witness_history.cpp` (in `make -C firmware/tests_host`).
Pure C++, no Arduino. Given the file one chunk at a time, **newest bytes
first**, it produces one page of rows newest-first:

- **Any chunk size.** A line, a hex field or a single byte may straddle
  chunks; the scanner assembles each line right-to-left in a
  `RECORD_LINE_MAX` buffer. The test drives chunk sizes from 1 byte to the
  whole file and requires identical pages.
- **Complete lines only.** Bytes after the last `\n` are a torn append (a
  power cut mid-write) and are skipped, the same rule
  `witness_store::tail_parse` uses at boot. A line over `RECORD_LINE_MAX`
  is skipped without overflow; a line that fails
  `witness_store::line_parse` (or carries no `type`) is skipped and
  counted. One bad line never ends a page.
- **Bounded work.** A page stops at its `want`-th row (≤ 32). The test pins
  the read count: a 32-row page of real ~390-byte lines costs exactly the
  13 one-KiB reads that cover it.
- **An untrusted resume hint.** `next_hint()` is the byte offset of the
  oldest returned row. The client echoes it with `before` = that row's seq,
  so page N+1 costs O(page), not O(distance into the file). The walker
  never trusts it: in hint mode the start must sit just after a `\n`, and
  the first complete line must be `seq == before - 1`, or the scan stops
  with `HINT_MISMATCH` and the bridge falls back to scanning from the file
  end. A hint pointing mid-line, at the wrong record, or into the first line
  is refused in the test.
- **Linkage, not verification.** `link_check()` confirms each row's `prev`
  is the next-older row's `ch` and seqs step by one; `links_to()` joins
  pages (and joins the card's newest row to the ring's oldest). Neither
  checks an Ed25519 signature or recomputes a chain hash.

## Stage 2 — the bridge (built, F35)

The state machine is the pure header
`firmware/common/witness/witness_history_bridge.h`, host test
`firmware/tests_host/test_witness_history_bridge.cpp` (in
`make -C firmware/tests_host`). The firmware glue is
`firmware/canary/lib/securacv_witness/src/securacv_witness_history.{h,cpp}`,
behind `FEATURE_SD_STORAGE`: one static slot, the SD calls, the task delay.

**One slot, one request at a time.**

```
struct Request  { uint32_t before_seq; uint32_t hint; bool has_hint; uint8_t want; };
struct Response { Result result; uint8_t first, n; bool more, hint_refused; Link joins;
                  uint32_t next_hint, skipped; Link linked[32]; HistoryRow rows[32]; };
struct Slot     { uint32_t busy, req_lock, req_gen, done_gen, abandoned_gen;  // __atomic only
                  Request req; Response resp; /* loop-task walk state */ };
```

- `begin()` / `wait()` / `end()` — **HTTP task** (the glue's
  `witness_history_request()` / `witness_history_release()`). `begin()`
  claims `busy` with a compare-and-set (a second request gets
  `503 history_busy`) and publishes the request under a new generation.
  `wait()` polls for **that** generation's page every 10 ms until
  **3000 ms** (`WAIT_MS`) have passed on `millis()` (the delay is
  `vTaskDelay`). The budget is time on the clock, not the sum of the sleeps
  asked for, so a sleep that runs long because a higher-priority task held
  the core counts for as long as it ran. On a page, the handler
  builds its JSON straight from the slot and then calls `end()`; on a timeout
  it answers `504 history_timeout` and `end(gave_up)` marks the generation
  abandoned. Either way `busy` is released at once.
- `service()` — **loop task**, called from `loop()` right after
  `storage_periodic_check()`, every pass. It adopts the newest published
  generation (a newer one replaces a walk in progress), drops a walk whose
  waiter gave up without reading another byte for it, and answers `NO_CARD`
  without touching SD when no card is mounted. While a background mount is
  in flight it touches nothing and tries again next pass. It picks the
  walker's start **before the first read**: a hint larger than the file
  size is refused outright (the walker cannot see the size) and the page is
  scanned from the file end, exactly as on `HINT_MISMATCH`; a hint of `0` is
  the start of the file, so the walker answers `AT_START` with no rows and
  no read; any other hint is handed to the walker, which re-checks it on the
  bytes before it. Then it reads **at most 4 × 1 KiB per pass**
  (`READS_PER_PASS` × `READ_LEN`), feeding the walker; on a terminal status
  it fills the response and publishes it under the generation it walked.
  The per-pass cap is meant to leave the task watchdog and the sensing
  cadence alone, with a deep page taking a few passes rather than one long
  read. That is design intent. Bench U1 still has to show it, by measuring
  the loop time of each pass on a large `records.jsonl`. A pass costs more
  than its 4 KiB: FATFS fast seek is off in the 2.0.17 core
  (`CONFIG_FATFS_USE_FASTSEEK` unset), and the file is reopened every pass.
  So the pass's first seek, and any seek back into an earlier cluster,
  walks the cluster chain from the start of the file. That cost grows with
  the file's size.
- **A late completion never answers the next request.** A waiter accepts
  only a page published under its own generation (`done_gen == gen`), and
  a walk whose request was superseded or abandoned is not published at all.
  The host test drives the races deterministically — the old walk finishing
  in the very pass during which its waiter gives up and the next request is
  published (the HTTP side runs inside a read callback), and two waiters in
  a row giving up while one walk runs — and then through real threads (three
  HTTP tasks, one loop task; every accepted page equals a single-threaded
  run of the same request; ThreadSanitizer-clean when built with it).
- The storage contract's paragraph in `securacv_storage.h` gained the line:
  reads are loop-task-only too, and this bridge is the one HTTP-side
  consumer.

Where the build differs from the first draft of this design, and why:

- **Atomics, not FreeRTOS semaphores.** The draft named a `busy` mutex and a
  `done` binary semaphore. The built slot moves `busy`, `req_gen`,
  `done_gen` and `abandoned_gen` by GCC `__atomic` builtins (the pattern
  `mesh_session`'s REST slot already uses) and the HTTP side polls, so the
  whole handshake compiles on the host and is tested there. A small
  `req_lock` guards the request body while either side copies it; a side
  that finds it held never waits (`begin()` answers busy, the loop tries
  next pass), so neither task can stall the other.
- **The file is opened once per pass, not once per request**, and closed
  before the pass returns, so no handle is held across a pass in which the
  loop appends, remounts or tears down. A page records the file size and
  the mount generation it started on; a remount, a card swap or a shrunken
  file mid-page ends it as an I/O error (`500 history_read_failed`).
- **A timed-out walk is dropped, not finished**: the abandon mark stops the
  loop reading for a page nobody will read.
- **Pages read up to two records they do not return**, so every row's
  linkage is checked (below), and a page returns at most 30 rows
  (`PAGE_ROWS_MAX`).

Memory: the slot is about 3.2 KiB (32 rows × 80 B, the walker's 512-byte
line buffer, the response's flags) plus the loop's 1 KiB read buffer, all
static `.bss` — no heap. CI's `release` / `release_ha` OTA-slot size guards
see the code.

**The endpoint** extends `handle_witness` — no new route, so
`max_uri_handlers` does not move (`check_route_security.py`'s budget count
is unchanged). When `before` is at or below the ring's oldest seq (or the
ring is empty and `before` is set), it calls the bridge, passing `?hint=`
when present; a build without `FEATURE_SD_STORAGE` keeps answering from the
ring alone. `rate_limit_check(req, true)`: an SD page counts as an action.
A card page is `{"ok":true,"source":"sd","records":[…],"next_hint":N,
"more":bool,"joins":bool,"hint_refused":bool,"skipped":N,"total":ring}`,
oldest to newest like the ring page. Each row carries `seq`, `type_name`
(`record_type_name(type)`), `chain_hash` hex, `time_bucket`, `source:"sd"`
and `linked`:

- `linked: true` — the row chains from the next older record on the card
  (its `prev` is that record's `ch` and the seqs step by one,
  `links_to()`); `false` — it does not (a gap or a break); `null` — the
  card holds no older readable record. Every page reads one row below its
  oldest so no returned row is left unchecked, and the join between pages
  is covered the same way.
- `joins` (pages scanned from the file end) — the card's copy of the record
  AT `before`, which the client already shows, chains from the page's
  newest row; `false` when it does not or the card does not hold it. A hint
  page needs no join: the previous page's oldest row was already checked
  against this page's newest.
- `more` — an older record remains on the card; the ring page carries
  `more` too (older ring records, or on a card build anything before the
  ring's oldest).

Errors: `503 history_busy`, `504 history_timeout` (`http_send_error` gained
the 504 mapping), `503 no_card`, `500 history_read_failed`.

**The UI** (`loadMoreTimeline` in `securacv_webui.cpp`) passes `&hint=` from
the previous card page, shows Load More while the device says `more`, and
badges `source:"sd"` rows **"from card, chain-linked"** — never the ring's
"Verified", because nothing on this path checks a signature (32 Ed25519
verifications per page on the loop task would be the stall the per-pass cap
exists to prevent). A row whose `linked` is false says so, the card's oldest
record says that, a `joins:false` page flags its newest row, and a divider
marks where the card rows begin. Busy and timeout leave Load More in place
with a note; "no card" retires it. `firmware/tests_host/test_canary_timeline.test.js`
lifts those functions out of the raw string and pins that behavior.

## What stays honest

- A stalled loop (a camera peek, a remount in flight) makes a page time
  out and say so. The wait gives up at its first poll at or past 3 s on
  `millis()`, so it overruns by what one 10 ms sleep overran, not by
  further sleeps. How long a preempted httpd task really takes to wake is
  for bench U1 to measure, along with page latency.
- Reading while the loop appends is safe only because the reader IS the
  loop task: the walker sees either the old end or the new one, never a
  half-written line it would trust (a torn tail is skipped by rule).
- Rows from the card are labeled chain-linked, never verified: linkage
  alone does not show a row is unaltered — only its signature and a
  recomputed chain hash do. The off-device verifier
  (`tools/verify_witness_log.py`) remains the way to verify them.

## Proof, today and later

- Host: `make -C firmware/tests_host` runs the walker suite, the bridge
  suite (the handshake, the generation counter, the bounded wait, the
  per-pass budget, the hint refusals, linkage; deterministic and threaded)
  and the timeline UI test.
- CI: the canary PlatformIO legs compile the glue, the endpoint and the UI
  (`firmware.yml`, the canary build matrix); `check_route_security.py`
  holds the route table.
- Bench (U1): latency and the timeout path need a card holding more than 32
  records on a real unit — a page read across passes while the loop appends,
  a card pulled mid-page, and a stalled loop answering 504 — plus, on a large
  `records.jsonl`, the loop time of each pass and how far a preempted wait
  overruns its 3 s.
