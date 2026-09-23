# Benchmarks — measure, never promise

**No number this harness prints is ever published.** Not on this page, not in a
README, not in a doc comment, not on the website. That is [AGENTS.md](../AGENTS.md) rule 4
("no performance claim without a benchmark") taken to its end: a benchmark is
something you *run*, on a machine you can name, and the number belongs to that
run. A number copied into a document outlives the machine, the commit and the
conditions that produced it, and from then on it is a claim.

What the tree carries instead is the **harness** — the code that produces a
number on demand, [`tests/bench_harness.rs`](../tests/bench_harness.rs) — and
this page: what each row measures, how to run it, and where the output goes.

## Run it

```sh
cargo test --release --test bench_harness -- --ignored --nocapture --test-threads=1
```

- `--release` is not optional. A debug build measures the optimizer's absence;
  the harness prints `DEBUG BUILD` in its header so such a table cannot pass
  for a release run.
- `--ignored`: every benchmark is `#[ignore]`. A plain `cargo test` compiles
  the harness (so a signature change breaks the build instead of rotting it)
  but never runs it.
- `--nocapture` so the tables reach stdout; `--test-threads=1` so no two rows
  share the cores (the harness serializes them itself as well).
- `SECURACV_BENCH_N` sets N (default 10 000): the event count for the
  whole-log rows and the call count for the per-call rows.
  `SECURACV_BENCH_REPEATS` sets the passes over a built log or envelope
  (default 5).
- The sandbox row is Linux-only (see below); on other systems it does not
  exist, and `--ignored` lists one fewer test.

Detection latency is a different harness: `detect_eval` reports per-frame mean
/ p50 / p95 / p99 / max for a detector against a labeled dataset
([`eval/README.md`](../eval/README.md)). The Frigate learnings
([§3](frigate_pi5_learnings.md#3-where-securacv-stands-today-read-from-the-tree-not-the-roadmap))
ask for the sandbox's fork/pipe/waitpid, decode and inference as separate line
items. Two of the three now have a timer in the tree — inference in
`detect_eval`, the sandbox boundary here — and decode has none. The rest of
that doc's phase-1 harness
([§7, "Bench before believing"](frigate_pi5_learnings.md#7-suggested-sequence):
frames gated and end-to-end latency per camera, on a Pi 5) waits on the
phase-1 pipeline and the hardware; nothing here stands in for it.

## What each row measures

Every row prints one Markdown table line through the crate's own
`eval::metrics::latency_stats` — count, mean, p50, p95, p99 and max, in
milliseconds (microseconds for the two per-call rows, see below) — under a
`host:` line naming the OS, the architecture, the build profile and N.

| row | what one sample is | what it deliberately leaves out |
|---|---|---|
| `append_event_checked` | One event through the checked path witnessd uses: module allowlist, contract enforcement, hash chaining, Ed25519 signing, the SQLCipher write. N events into one fresh on-disk database; a trailer line gives the loop's wall time and events per second. | Building the candidate (done outside the window); any frame work. |
| `run_full_verify` | One full pass of the walk `log_verify` performs with an out-of-band public key — hash chain, every signature, checkpoint lineage — over a log of N events. `repeats` passes. | Opening the database; the high-water-mark file. |
| `build_evidence_envelope_for_api` | One envelope assembled through the API path — export artifact, export receipt (sealed, as the real path does), all four ledgers, whole-envelope digest — over N events. | Serialization to bytes; disk. |
| `verify_envelope` | One offline verification of the last envelope built: structure, ledgers, every signature, digest. | The bytes path (`verify_envelope_bytes`), which adds the JSON parse and a digest over the presented bytes. |
| `ContractEnforcer::enforce` | One conforming candidate through the contract: confidence bounds, zone allowlist, bucket coarsening, the correlation-token rule. | Anything that touches storage. |
| `TimeBucket::now_10min` | One coarse clock read — the call each witnessd loop iteration starts with. | — |
| `execute_sandboxed (no-op module)` | One trip across the sandbox boundary with a module whose `process` does nothing: fork, seccomp filter, result pipe, waitpid, and the stub backend's state export (child) and import (parent), on one synthetic 640×480 frame. The backend is primed once first, so it carries state the way a running witnessd backend does. | Inference and decode. The row is the boundary's own cost — the per-frame fork the Frigate learnings call an unmeasured hypothesis. Linux only, by the same `cfg(target_os = "linux")` the sandbox module carries: Linux is the only platform with a sandbox implementation, and elsewhere `execute_sandboxed` refuses ("sandbox unavailable"), so there is no boundary to time. |

The two per-call rows (`enforce`, `now_10min`) time one call per sample and
print microseconds to three decimals (nanosecond steps); every other row
prints milliseconds to four. Rounded to the fourth decimal of a millisecond, a
single call's cost could read the same before and after a change, and two
tables put side by side (the only comparison, below) would show nothing. The
cost of `Instant::now()` itself is inside every sample, which matters most for
the shortest calls.

## Reading a table honestly

- **One host, one run.** The only comparison that means anything is two runs
  on the same machine, same profile, same N, ideally back to back. A laptop
  table says nothing about a Raspberry Pi 5, and CI runs the harness on an
  x86 GitHub runner, not on a Pi.
- **The append row touches the disk by design.** It writes a real SQLCipher
  database in a temp directory, so it inherits that filesystem's sync
  behavior. A tmpfs run and an SD-card run are different experiments.
- **No thresholds.** The harness asserts only correctness: each row completed
  with the expected count, and the verify rows actually verified. A
  regression is something a person notices by putting two tables side by
  side; nothing here turns red on a slow machine.
- **Percentiles from five samples are five samples.** The `repeats` rows
  report what they have; p95 and p99 of five passes are the slowest pass.

## Where a run's output goes

1. **The terminal**, for the person who ran it.
2. **The CI artifact.** The `bench-harness` job of
   [`rust.yml`](../.github/workflows/rust.yml) runs the harness once in
   release and uploads its stdout as `bench-harness-stdout`, also when a row
   fails. That run proves the harness still runs and hands a reviewer a table
   to read beside a change that claims to be faster; it has no threshold and
   compares nothing. The run itself is bounded at 15 minutes, apart from the
   compile, so a hung row fails the job instead of holding it.
3. **The PR or issue**, as a comment with the `host:` line intact — when a
   change is about speed, the before/after tables belong in its review thread.

Nowhere else. **No number from a run is committed anywhere in the tree**: not
this page, not a README, not a doc comment. The guard is
[`scripts/lint_bench_rows.py`](../scripts/lint_bench_rows.py), a step of the
**Repo Lints** workflow's `scripts/lint_*.sh` job
([`lint.yml`](../.github/workflows/lint.yml)), which runs on every pull request
whatever it touches. It fails if a measured row — one of the harness's row
names followed by a number, the shape the tables print, blockquoted or not —
or the append row's `total:` trailer lands in any Markdown file in the
repository. It reads the row names from `ROW_NAMES` in the harness, and the
harness refuses to print a row whose name is not listed there, so a new row is
under the guard from its first run. It catches a pasted table, not a number
retyped into prose; that part is review. The guard reads this repository
only: the website is bound by its own never-overclaim rule, and no test there
looks for a pasted table, so on the site it is kept out by review. If a
document needs to say something about speed, it links to the artifact or the
thread and says `measured on <host> in <run>`.

## What this does not cover

- Detector inference (`detect_eval`), video decode, RTSP and MQTT transport,
  the API server, the break-glass ceremony. Each is a separate harness or not
  yet timed by anything in the tree.
- Memory, power, or anything in the device line's firmware — those belong to a
  bench session ([`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md)).
