# Flight Rules

The standing engineering constitution for securaCV, adopted from the
flight-software audit ([docs/strategy/12](strategy/12-engineering-foundations-flight-rules.md),
§6 — rationale, NASA lineage, and the mishap history live there). Rules are
cited by ID in code review ("this loop needs an FR-4 bound"), and every rule
is deliberately **machine-checkable**: if a proposed rule can't be enforced by
a tool or a test, it gets rewritten until it can (Holzmann's meta-rule).

Scope follows criticality (doc 12 §1): **Class A** = evidence integrity
(chain append/verify, crypto, export, firmware witness chain, OTA verifier,
offline verifier). **Class B** = ingest & egress (witnessd loop, API,
bridges, adapters, firmware network). **Class C** = operator UX.

Status legend: ✅ enforced · 🟡 practiced but not yet gated · ❌ open.
Update the status column in the PR that changes it.

| ID | Rule | Enforcement | Status |
|----|------|-------------|--------|
| **FR-1** | **The build is silent.** All warnings on, warnings are errors, static analysis on every commit — Rust *and* firmware. | `cargo clippy --all-targets -- -D warnings` in CI (have); `-Werror` in `envs/platformio/common.ini` + cppcheck gate (open) | 🟡 Rust ✅ / firmware ❌ |
| **FR-2** | **Class A/B paths cannot panic.** No `unwrap`/`expect`/`panic!`/indexing on daemon or firmware steady-state paths; errors propagate to a defined handler. Release builds `panic = "abort"` — a dead thread must not leave a half-alive daemon. | clippy restriction lints per-crate (open); `[profile.release]` (have, since #909) | 🟡 |
| **FR-3** | **No `unsafe` outside audited FFI modules.** Dependency unsafe is measured and reviewed. | `#![forbid(unsafe_code)]` on app modules + cargo-geiger report (open) | ❌ (practice ✅ — unsafe already minimal, FFI-only) |
| **FR-4** | **Everything is bounded.** Fixed caps on every queue, map, buffer, and retry loop; steady-state heap growth is a defect; every loop has a provable bound or a justifying comment. | Cap-and-evict pattern (`RATE_MAX_TRACKED_IPS` is the template) + bound tests; bounded channels; review checklist | 🟡 (API/auth/webhook maps capped; audit others on touch) |
| **FR-5** | **Assert invariants — and recover.** Dense checks on the evidence path; a failed check seals a fault event and enters a defined mode, never silent continuation and never bare abort. | Failure-semantics ladder (have); cargo-mutants to prove checks bite (open) | 🟡 |
| **FR-6** | **Every return value is handled.** `let _ =` requires a justifying comment. | `unused_must_use` deny + clippy `let_underscore_must_use` (open); review | 🟡 |
| **FR-7** | **Tasks communicate by message; locks only at leaves, never nested, never poisoned into cascade.** Single owner per resource; poison-tolerant access on shared state. | Kernel core pattern is the template; adapter-host cleanup tracked (doc 12 K7) | 🟡 |
| **FR-8** | **Every mode is explicit; boot always succeeds into a reporting state.** Enumerated states incl. degraded modes; transitions are logged events; the kernel boots and reports even with its store full or corrupt — and verifies its tail before the first append. | Exhaustive `match` on mode enums; boot tail-verify (have, since #990 — `witnessd` verifies the sealed-log tail at startup and drops to a recorded safe mode on mismatch); corrupt-store boot cases in the DITL suite (open) | 🟡 |
| **FR-9** | **The watchdog proves every task is alive.** Per-task check-in; never fed from a timer ISR; a trip latches a logged cause and escalates. | `esp_task_wdt` per variant (all variants ✅ since #909) + per-variant regression check (open); witnessd watchdog thread (have, since #990); systemd `WatchdogSec` (open) | 🟡 |
| **FR-10** | **No anomaly without an observable; no resource without a margin alarm.** Every fault emits a structured event; CPU/RAM/storage/queue depth/chain length have telemetry plus margin thresholds. | Sealed heartbeats + `/status` (have); metrics on witnessd/bridges + margin alarms (open) | 🟡 |
| **FR-11** | **Scrub the record.** Chain segments re-verify continuously against anchors; mismatches latch until an operator clears them; boot verifies the tail. | Scheduled verify 24 h + Verify Now (have); boot tail-verify (have, since #990); bit-flip injection test + mismatch-latch-until-cleared (open — `chain_problem` currently self-clears on the next good verify) | 🟡 |
| **FR-12** | **Test like you deploy.** Release gate: ≥24 h day-in-the-life on the real rig with power-pulls, broker kills, clock jumps, disk-full; soak past every counter epoch; every deviation from real conditions listed with rationale. | HIL rig + fault campaign + deviation ledger (open; `v1_bench_validation_runbook.md` is the seed) | ❌ |
| **FR-13** | **One dictionary.** Topics, payload schemas, event/attestation vocabularies, and config tables come from one machine-readable source; bindings and docs are generated; CI fails on drift — including cross-language golden vectors for every signature format. | `spec/witness_dictionary.json` + `scripts/lint_dictionary_sync.py` drift gate in CI (have — event/failure/attestation/claim/modality vocabularies + signature constants across Rust/Python/JS/firmware); codegen to *replace* the hand-written copies + cross-language golden signature vectors still open (#925); envelope byte-parity fixtures are the proven template | 🟡 |
| **FR-14** | **Nothing ships unvetted or unverifiable.** Deps pass cargo-deny (+vet for Class A); releases carry signed provenance with committed SBOMs; installers verify checksums; every node reports version+config hash at startup; one version, one gate. | Version-lockstep lint (have, since #909); cargo-audit + dependabot (have); cargo-deny/vet, cosign/SLSA, pinned installer (open) | 🟡 |

Amendment process: propose the change in a PR touching this file; the PR
must show the enforcing tool or test alongside the rule text (governance
follows [docs/governance_and_invariants.md](governance_and_invariants.md) —
weakening is rejected by default).

## Backlog

The roadmap items from docs 11–13 are tracked as GitHub issues, each tagged
with its source (`doc-11` / `doc-12` / `doc-13`) and the Flight Rule it
advances:

| Issue | Rule(s) | What |
|---|---|---|
| [#918](https://github.com/kmay89/securaCV/issues/918) | FR-8/9/11 | Boot chain tail-verify + witnessd watchdog |
| [#919](https://github.com/kmay89/securaCV/issues/919) | FR-7 | Poison-tolerant adapter host + adapter-gap sealing |
| [#920](https://github.com/kmay89/securaCV/issues/920) | FR-3 | Flash-encryption/secure-boot decision (firmware key) |
| [#921](https://github.com/kmay89/securaCV/issues/921) | FR-4 | Firmware keygen entropy, reconnect jitter, socket timeout |
| [#922](https://github.com/kmay89/securaCV/issues/922) | FR-13 | Single-source the OTA verifier + reconcile display trees |
| [#923](https://github.com/kmay89/securaCV/issues/923) | FR-5 | Property tests, fuzzing, crash injection, coverage |
| [#924](https://github.com/kmay89/securaCV/issues/924) | FR-14 | Supply chain: cargo-deny/vet, digest pins, provenance |
| [#925](https://github.com/kmay89/securaCV/issues/925) | FR-13 | The Witness Dictionary + codegen + checkpoint hash |
| [#926](https://github.com/kmay89/securaCV/issues/926) | FR-11 | The Correlation Check ritual |
| [#927](https://github.com/kmay89/securaCV/issues/927) | FR-8 | Preservation holds + erasure certificates |
| [#928](https://github.com/kmay89/securaCV/issues/928) | FR-10/11 | Mesh witness cosigning + chain replication |
| [#929](https://github.com/kmay89/securaCV/issues/929) | FR-12/14 | Custody ceremony, FRE 902 certs, golden corpus |
| [#930](https://github.com/kmay89/securaCV/issues/930) | — | HA one-architecture (discovery, push, quality scale) |
| [#931](https://github.com/kmay89/securaCV/issues/931) | — | HA Gold-tier UX (repairs, events, dashboard) |
| [#933](https://github.com/kmay89/securaCV/issues/933) | — | The Evidence Pack (one-click court/insurer bundle) |
