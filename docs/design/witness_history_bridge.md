# Timeline history deeper than the ring — the loop-task SD read bridge

Status: **Stage 1 built (the pure walker, host-tested). Stage 2 designed,
NOT built** (the bridge, the endpoint, the UI).
Scope: the canary PlatformIO tree (`firmware/canary`). Repo sweep item F26.
Last Updated: 2026-09-23

## The gap

The web timeline pages through `GET /api/witness?last=N&before=SEQ`, and
`handle_witness` (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`)
serves only the in-RAM ring: 32 records (`WITNESS_RECORD_RING_SIZE`,
`securacv_witness.cpp`), portMUX-guarded. "Load More" stops at the ring's
oldest record even though every signed record is also on the card, one line
each in `/WITNESS/records.jsonl` (`witness_store::line_build`, appended by
`sd_append_record` on the loop task). `/api/export` does not read the card
either — it returns metadata only. So the only way to look further back
today is to pull the card or use the export/unseal tools.

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

## Stage 2 — the bridge (designed, not built)

Lives beside `sd_append_record` in `firmware/canary/lib/securacv_witness`
(or a new `securacv_witness_history.cpp` in that lib), behind
`FEATURE_SD_STORAGE`.

**One mailbox, one request at a time.**

```
struct HistoryRequest  { uint32_t gen; uint32_t before_seq; uint32_t hint; bool from_hint; uint8_t want; };
struct HistoryResponse { uint32_t gen; HistoryRow rows[32]; uint8_t n; uint32_t next_hint; Status status; };
SemaphoreHandle_t busy;   // mutex: one outstanding request
SemaphoreHandle_t done;   // binary: the loop finished request `gen`
```

- `witness_history_request(req, timeout_ms)` — **HTTP task.** Take `busy`
  with a zero wait (else answer `503 history_busy`); bump `gen`; publish the
  request; wait on `done` for at most **3000 ms** (else release `busy`,
  answer `504 history_timeout`, and leave the loop to finish and discard —
  the generation counter means a late completion can never satisfy the
  *next* request); copy the response out; release `busy`.
- `witness_history_service()` — **loop task**, called from `loop()` right
  after `storage_periodic_check()`, and only when `storage_is_mounted()` and
  `!storage_mount_in_flight()`. Open `/WITNESS/records.jsonl` once per
  request, seek to the hint (if it passes a one-line re-parse) or the file
  size, and read **at most 4 × 1 KiB per pass**, feeding the walker; on a
  terminal status, copy the rows into the response and give `done`. The
  per-pass cap keeps the task watchdog and the sensing cadence untouched: a
  deep page takes a few passes, never one long stall.
- The storage contract's paragraph in `securacv_storage.h` gains one line:
  reads are loop-task-only too, and this bridge is the one HTTP-side
  consumer.

Memory: 32 rows × 80 B + the walker's 512-byte line buffer + two
semaphores — about 3 KiB, which the CI `release` / `release_ha` size guards
will see.

**The endpoint** extends `handle_witness` — no new route, so
`max_uri_handlers` does not move. When `before` is at or below the ring's
oldest seq (or the ring is empty and `before` is set), it calls the bridge
with `?hint=` when present. Rows carry `seq`, `type_name`
(`record_type_name(type)`), `chain_hash` hex, `time_bucket`, `source:"sd"`
and `linked` (from `link_check`/`links_to`), plus top-level `next_hint`.
`rate_limit_check(req, true)`: an SD page counts as an action.

**The UI** (`loadMoreTimeline` in `securacv_webui.cpp`): pass `&hint=` from
the previous response, stop when `n == 0`, and badge `source:"sd"` rows
**"from card, chain-linked"** — never the ring's "Verified", because nothing
on this path checks a signature (32 Ed25519 verifications per page on the
loop task would be the stall the per-pass cap exists to prevent). A row whose
`linked` is false says so.

## What stays honest

- A stalled loop (a camera peek, a remount in flight) makes a page time
  out at 3 s and say so; it never blocks the httpd task longer than that.
- Reading while the loop appends is safe only because the reader IS the
  loop task: the walker sees either the old end or the new one, never a
  half-written line it would trust (a torn tail is skipped by rule).
- Rows from the card are labeled chain-linked, never verified: linkage
  alone does not show a row is unaltered — only its signature and a
  recomputed chain hash do. The off-device verifier
  (`tools/verify_witness_log.py`) remains the way to verify them.

## Proof, today and later

- Now: `make -C firmware/tests_host` runs the walker suite.
- Stage 2: CI's canary PlatformIO legs compile the bridge; latency and the
  timeout path need a card holding more than 32 records on a bench unit
  (U1).
