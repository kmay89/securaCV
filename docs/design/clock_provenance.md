# Clock provenance — the device clock measured against every anchor (design, as built)

**Status: built.** `spec/quorum_unseal_v2.md` §4: "Each anchor seals a
**clock-provenance event** (the measured device-clock vs TSA `genTime`
offset) — the direct rebuttal to timestamp challenges." This is the account
of how that was built, what was rejected and why, and what a third party
checks. The operator's view is [`../timestamping.md`](../timestamping.md),
"Clock provenance"; the verifier's is [`../log_verify.md`](../log_verify.md)
and [`../court_export.md`](../court_export.md).

## The decision in one paragraph

`witnessd` — the process whose wall clock stamps every `time_bucket` and the
only long-running holder of the device key — polls its own `tsa_anchors`
table every 5 s (a local read; no network) and, for every anchor row past a
persistent cursor, seals **one device-signed, hash-chained record** into a
new never-pruned ledger, `clock_provenance`: the device's 10-minute bucket
at observation, the anchor row id, `sha256(token_der)`, and — when the row
appeared between two polls of a running daemon — the whole-second, clamped
`observed_offset_s = device wall clock − genTime` with the measured
`poll_gap_s` that bounds how old the row could have been. Rows found at the
first poll after a daemon start get a binding-only record (both measurements
`null`). The ledger's head is the fifth anchor subject,
`clock_provenance_head`, so every record is fixed in external time by the
next scheduled `anchor-all`. `log_anchor` stays an observer: no key, no new
columns, nothing new written. A third party with the database or the court
kit, the device public key, `sha256sum` and `openssl` re-derives every
stored fact and checks every signature.

## Verifier first: what each stored fact is checked against

| Stored fact | Check | Source of truth |
|---|---|---|
| Which token | `sha256sum` of the token bytes equals `token_sha256` | `tsa_anchors.token_der` / the kit's `anchors/*.der` |
| The TSA's time | `openssl ts -reply -in <token> -token_in -text` → `Time stamp` | the countersignature |
| The device's reading | `genTime + observed_offset_s` lies in `[time_bucket.start_epoch_s, +600)` (an offset at the ±3600 clamp is a bound, not a reading) | arithmetic on record and token |
| Promptness | `poll_gap_s`: the row did not exist one completed poll earlier | the record |
| The device said it | `entry_hash = SHA256(prev_hash ‖ payload_json)`; Ed25519 over `SHA256(u32le(32) ‖ "securacv:pwk:clock-provenance:v1" ‖ entry_hash)` | device signature, chain link |
| Predates a dispute | a later `clock_provenance_head` anchor covers a later head | the next TSA token |
| Anchor not deleted or swapped | a record naming a missing anchor id, or a row whose token hashes differently, is a contradiction | record vs `tsa_anchors` |

## Measurement

Per anchor row *r* at poll time:

- `W` — the witnessd wall clock in whole seconds, read **once** per poll and
  shared by every row of that poll. `G` — the token's `genTime`
  (`tsa::gen_time_unix`, fractional seconds truncated:
  `20260610123324.5Z → 1781094804`).
- `observed_offset_s = clamp(W − G, −3600, +3600)` (i32);
  `poll_gap_s = clamp(ceil(seconds since the previous COMPLETED poll), 1, 3600)`
  (u32) on the monotonic clock.
- The bound: with δ = device clock − true time,
  δ ∈ [offset − poll_gap_s − d, offset), where *d* (issuance-to-storage delay,
  about a second on a hub that anchors online) is not measured and is stated
  as a residual. A negative offset means the device read behind by at least
  |offset| whatever the delay.

Mechanics (`src/bin/witnessd.rs`, `AnchorObserver`, beside `ClockMonitor`
and ticked right after it): poll interval 5 s, a failed poll backs off 60 s,
is sealed as a `StorageWriteFailed` failure record, counts into the
heartbeat's failure delta, and is **not** a completed poll (so the next gap
measures the whole stall). A stalled main loop widens `poll_gap_s` honestly
instead of asserting a 5 s bound. Safe mode returns before the main loop, so
nothing is appended there. The tick is composed of pieces the tests drive
with injected instants (`due`, `poll_gap_s`, `poll`, `record_failure`,
`tick_at`).

## The record — these fields and nothing else

| Field | Type | Why |
|---|---|---|
| `schema` | `"securacv/clock_provenance/v1"` | discriminator |
| `time_bucket` | `TimeBucket` (10 min, from `W`) | the device clock at observation, kernel granularity |
| `anchor_id` | i64 | locator; the binding is `token_sha256` |
| `token_sha256` | 64 lowercase hex over `token_der` | binds imprint, `genTime`, TSA identity; `sha256sum`-checkable |
| `observed_offset_s` | i32 or `null` (clamped ±3600) | the measurement; positive = device ahead; `null` = binding only |
| `poll_gap_s` | u32 or `null` (1..=3600) | the row was at most this old when first seen; `null` with the offset |
| `kernel_version` | string | which build measured |

Not stored: `genTime`, the anchor's subject or subject hash, TSA identity or
URL, sub-seconds, the anchoring host's clock, hostnames, boot ids, uptime,
paths. Whole seconds, not a band — bands are derived at display time
(`within one bucket (600 s)` / `BEYOND …`).

The wire shape is byte-pinned (`serde_json::to_string`, declared order, both
`Option`s as explicit `null`), and the stored `payload_json` literal is what
is hashed and signed — no verifier re-serializes it:

```json
{"schema":"securacv/clock_provenance/v1","time_bucket":{"start_epoch_s":1781094600,"size_s":600},"anchor_id":7,"token_sha256":"<64 hex>","observed_offset_s":2,"poll_gap_s":5,"kernel_version":"0.0.0-test"}
```

A binding-only record carries `"observed_offset_s":null,"poll_gap_s":null`.

## Storage and binding

- Table `clock_provenance`, written only through `Kernel` (so only by
  `witnessd`), created by `Kernel::ensure_schema` and never pruned. Chain
  shape is the export-receipt ledger's exactly: genesis `prev_hash` is 32
  zero bytes, `entry_hash = SHA256(prev_hash ‖ payload_json)`, signed under
  `DOMAIN_CLOCK_PROVENANCE = "securacv:pwk:clock-provenance:v1"` (32 bytes, so
  the length-prefixed signing message starts `20 00 00 00`), optional PQ
  columns, `created_at = time_bucket.start_epoch_s`.
- Cursor: kernel-private `clock_provenance_cursor` (one row, `id = 1`,
  `last_anchor_id`). Every append and the cursor advance share ONE
  `BEGIN IMMEDIATE` transaction, so a crash can neither duplicate a record
  nor skip a row. The first poll on a database with no cursor row sets it to
  `MAX(tsa_anchors.id)` (0 without the table) and seals nothing:
  pre-upgrade anchors are never backfilled.
- A row whose `token_der` is not bytes gets no record and the cursor moves
  past it; a token, or a `genTime`, that does not parse gets a binding-only
  record (the hash binding holds; the measurement cannot).
- The head is anchored as `clock_provenance_head`, appended last to
  `AnchorSubject::HEADS` (the request and classification order of the first
  four is unchanged). Every generic reader — `ledger_head`, `hash_in_ledger`,
  `classify_hash`, `ledger_position`, the empty-ledger sentinel, the policy
  planner — works for any table with `id, entry_hash`.

Frozen DDL (pinned by a unit test, in the source and in `sqlite_master`):

```sql
CREATE TABLE IF NOT EXISTS clock_provenance (id INTEGER PRIMARY KEY AUTOINCREMENT, created_at INTEGER NOT NULL, payload_json TEXT NOT NULL, prev_hash BLOB NOT NULL, entry_hash BLOB NOT NULL, signature BLOB NOT NULL, pq_signature BLOB, pq_scheme TEXT);
CREATE TABLE IF NOT EXISTS clock_provenance_cursor (id INTEGER PRIMARY KEY CHECK (id = 1), last_anchor_id INTEGER NOT NULL);
```

## Verification and surfacing

**`log_anchor verify`** (read-only, key-free, creates nothing):

1. Before the anchor loop it walks the whole ledger under the database's
   device key lineage (the pinned key plus every validated rotation, PQ key
   when stored, Compat mode — the `court_export` pattern) and prints one
   ledger line. A failing ledger prints `FAIL`, suppresses the per-anchor
   clock lines and fails the run after the loop.
2. Under every anchor's `OK` / `UNVERIFIED` / `FAIL` block, one indented
   `clock:` line, plus `note:` / `WARNING:` lines where they apply.
3. A record whose anchor id has no row, or whose row's token hashes
   differently, is a `FAIL` line and fails the run.
4. The threshold is `max_clock_offset_s` in the anchor policy (default one
   bucket, 600 s; at least 1). Without `--policy` the structural threshold
   is 600 s and a breach is a printed `WARNING` (no exit code); with
   `--policy` breaches count into `NOT SATISFIED`.
5. Delayed imports are named, not judged: `lag_s = anchor bucket start − G`;
   a record is *prompt* when `lag_s <= 1200`; an offset is evaluated when
   prompt OR negative; a delayed positive offset prints `not evaluated`.
6. Host-clock note: `host_gap_s = (G + offset) − anchor bucket start`,
   expected in `0..1200`; anything else is a `note:` that the anchoring
   host's clock and the device's disagree.
7. Structural check for every `chain_head` anchor whose head is a live
   sealed row, record or no record: `n = sealed_row.created_at − G`; `n > 1`
   means the device read at least `n` s ahead when it sealed that row — a
   `note:` within the threshold, a `WARNING` beyond it, and under `--policy`
   a breach only when the anchor's own line is `OK`.
8. ClockSkew correlation for every `BEYOND` line: the sealed log's retained
   `ClockSkew` failure records within one bucket of the record's bucket are
   named — "retained", because retention prunes failure records.
9. Step check after the loop: consecutive measured records whose offsets
   differ by more than 600 s get a note naming whether a ClockSkew record is
   retained between their buckets (inclusive).

Bail order: `{n} anchor(s) failed verification` → `clock provenance ledger
failed verification` → `{n} clock-provenance record(s) name anchors that
were deleted or replaced` → the policy's `NOT SATISFIED`. The exact lines are
listed in [`../timestamping.md`](../timestamping.md), "Clock provenance".

**`run_full_verify`** (`log_verify`, witnessd's boot verify, `POST /verify`):
after the export-receipt walk, `verify::verify_clock_provenance_with` walks
the ledger under the lineage keys; a chain or signature failure is a VERDICT
failure (`FailedLedger::ClockProvenance`, serialized `clock_provenance`,
named "clock-provenance ledger"). At the end of the anchors block each record
is cross-checked and produces WARNINGS only: `clock provenance orphan: …` for
a missing or replaced token, `clock offset: …` for a prompt-or-negative
offset beyond one bucket. A missing record is never a warning (pre-upgrade
anchors have none). The orphan check runs even with no anchors table and
skips a record whose anchor row exists but is malformed (already warned).
`VerifyReport.clock_provenance_verified` is `None` when the ledger has no
rows (so an un-recorded database serializes exactly as before), `Some(n)` on
a successful walk, `None` on a failed report. No warning carries a time or a
full hash: the report is published retained over MQTT.

**`court_export`**: the ledger is walked under the lineage the kit already
derived; every packaged token's record (by `anchor_id`, and only when its
`token_sha256` equals the hash of the packaged bytes) is written as
`clock_provenance/clock-NNNN.json` beside the verbatim signed bytes
`clock-NNNN.payload.json`; `CUSTODY_AND_CONTROL.md` gains a "Time source"
section, `VERIFICATION.md` a step 3b (`sha256sum`, `xxd`, `openssl pkeyutl
-rawin`), `README.md` an item 7, and `MANIFEST.json` the additive keys
`clock_provenance_ledger` (`"self-consistent; identity unverified"` |
`"verification failed"` | `"absent"`) and a per-anchor `clock_provenance`
object or `null`. The manifest never says "verified". A ledger that does not
verify packages no record (fail closed). `clock_provenance_head` anchor rows
are never packaged.

## ClockSkew and `audit_chain_timeline` — unchanged

witnessd's clock monitor still seals `ClockSkew` from its own
monotonic-vs-wall drift; a clock-provenance offset never seals one. The
correlation lives only in `log_anchor verify` (steps 8–9).
`FailureType` and `spec/witness_dictionary.json` are untouched; the frozen
timeline-audit strings are untouched.

## Invariant III

Every new field is a 10-minute bucket, a whole-second clamped difference, a
whole-second clamped duration, a row id, a hash or a version string. The only
precise instant a reader can derive, `G + offset`, is the time of an
anchoring act the TSA already fixed to the second, in a token the database
already stores. Nothing about events changes: an event's bucket under a
device offset is the same width shifted — a bias correction adds no
precision. Records are 1:1 with anchor rows, and `anchor-all` is
constant-count, so record timing tracks the anchoring schedule, not
incidents. `witnessd` opens no connection for this. The ledger is local and
reaches a third party only inside a court kit, for the anchors that kit
packages.

**The Invariant II residual, named so nobody lowers the quantization later.**
A sequence of offsets is a fingerprint of one device's clock skew. It is
bounded by the 1 s quantization, at the anchor cadence (not the event
cadence), by the ±3600 s clamp, by local storage, and by kit-scoped
disclosure. Sub-second offsets, a faster cadence, or publishing the ledger
would widen it.

## Compatibility

Frozen formats untouched: `ExportReceipt`; the `tsa_anchors` DDL and
`ANCHOR_EXTRA_COLUMNS`; `TimestampToken`; `checkpoint_message`; the earlier
kit manifest keys (the new ones are additive); `VerifyReport` gains one
additive `Option`; `ClockConfigFile` and the witnessd config are unchanged.

| Mix | Behavior |
|---|---|
| old database, new binaries | zero rows: the report field is omitted, `log_verify` prints `no clock-provenance records`, `log_anchor verify` prints `clock provenance: no records`, the kit says `absent`; `anchor-all`'s default subjects anchor the `clock_provenance_head` sentinel until the ledger has a row; anchors that predate the upgrade are never recorded |
| new database, old binaries | a `clock_provenance_head` row reads as a digest anchor (the documented fallthrough for unknown subject literals); older verifiers ignore the table |
| anchor policy carrying `max_clock_offset_s`, older `log_anchor` | refused (unknown key; the policy format denies unknown fields) — add the key only after every policy reader is upgraded |
| `anchor-all` request count | four subjects per TSA per run become five; the count stays constant run to run |
| MQTT verify report | unchanged until the first record exists |

## Rejected (and why, so they stay rejected)

- **Host-clock bracket readings on the anchor row**: unsigned facts from a
  foreign host laundered into a signed payload, and two new precise instants.
- **A sealed-log `record_type`**: retention deletes sealed rows by cutoff
  regardless of type; unknown types hard-fail `deserialize_compat` readers;
  it would travel to the API, envelopes, the viewer and tvOS.
- **Unsigned columns on `tsa_anchors` bound by the next checkpoint**:
  `checkpoint_message` is frozen v3, and checkpoints happen only on prune.
- **An anchoring-principal key**: a second pin for every verifier.
- **A config key for the cadence**: `ClockConfigFile` denies unknown fields,
  so an older witnessd would refuse the config.
- **An operator-declared `clock_source`**: a declaration is not a measurement.
- **A constant poll interval sealed as the bound**: false whenever the main
  loop stalls — `poll_gap_s` is measured instead.
- **Unbounded offsets**: clamped at ±3600 s.
- **Backfilling pre-upgrade rows**: a clock that did not observe a row cannot
  say how old it was; the cursor starts at `MAX(tsa_anchors.id)`.
- **witnessd sealing a `ClockSkew` failure from an offset**: two detectors
  for one failure type, one of them a function of an operator's anchoring
  schedule.
- **Changing `audit_chain_timeline`**: its strings are frozen, and
  cross-ledger reasoning cannot be re-derived after retention.

## Where the code is

`src/clock_provenance.rs` (read side: rows, the record↔anchor link, the
arithmetic, display bands, ClockSkew helpers), `src/lib.rs`
(`ClockProvenanceRecord`, `Kernel::append_clock_provenance`,
`Kernel::observe_anchors_for_clock_provenance`), `src/verify.rs`
(`verify_clock_provenance_with`), `src/verify_runner.rs` (the verdict and
the warnings), `src/tsa.rs` (`AnchorSubject::ClockProvenanceHead`,
`anchor_tokens_after`), `src/anchor_policy.rs` (`max_clock_offset_s`),
`src/bin/witnessd.rs` (`AnchorObserver`), `src/bin/log_anchor.rs`
(`ClockContext`), `src/bin/court_export.rs` (packaging).
