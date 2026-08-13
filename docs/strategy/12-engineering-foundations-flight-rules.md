# Engineering Foundations — a Flight-Software Audit and the Flight Rules

**Scope:** the Rust kernel (`src/`), the ESP32 Canary firmware (`firmware/`), and the
verification/CI/release machinery (`.github/workflows/`, `spec/`, `scripts/`),
audited the way a NASA/JPL software review board audits flight software: panic
paths, fault containment, degraded modes, power-loss safety, resource margins,
watchdogs, traceability, and configuration management. Standards drawn on: the
Power of Ten (Holzmann/JPL), JPL D-60411, NPR 7150.2 / SWE handbook, the NASA
Fault Management Handbook, and Aerospace Corp's Test-Like-You-Fly guides.

**Questions this doc answers:**

> If the people who write software for spacecraft reviewed this codebase, what
> would they say we're missing? What is our code doing wrong? And which of their
> practices — adopted now, as standing rules — make this foundational rather
> than fixable-later?

Short answer: **the philosophy is already flight-flavored; the application is
uneven.** This codebase does things almost no OSS project does — privacy
invariants enforced *by the compiler* (trybuild compile-fail tests), seccomp
sandboxes around detector code, an OTA pipeline that verifies signatures against
the bytes actually written to flash before switching boot partitions, latched
failure records with hysteresis, textbook monotonic-time discipline, and a
45 KB LESSONS_LEARNED.md that reads like a mishap-report archive. A review
board would recognize the culture immediately. It would then point at five
things: (1) the API **writes a signed receipt on every read**, so routine Home
Assistant polling costs ~2,880 database writes a day of SD wear and standing
lock contention on the evidence store — and any lock held past the (implicit,
library-default) 5-second busy handler still fails a real evidence append with
no retry; (2) nothing prevents **two writers from forking the hash chain**; (3) one shipped firmware variant has **no
watchdog and an unbounded reconnect loop** — a dead broker freezes the witness
forever; (4) the **primary untrusted-input surface (adapters) compiles in CI
but its tests never execute**; and (5) the repo currently **ships two different
version numbers** with no configuration-management gate. Every one of these has
a famous mishap with its name on it (§7). The fix is a short list of defects
(§4) plus fourteen standing Flight Rules (§6) that CI enforces from now on.

---

## 1. The lens: this product *is* a flight recorder

A witness kernel has the same job as a flight data recorder: **never corrupt
the record, degrade predictably, fail loudly, run unattended, and survive the
crash it exists to document.** That makes flight-software practice unusually
transferable — not as bureaucracy, but as the engineering answer to "what does
it take for a record to be trusted after something went wrong."

NASA scales rigor by criticality class (NPR 7150.2 Appendix D) rather than
applying maximum rigor everywhere — the anti-cargo-cult mechanism we should
copy. Proposed classification:

| Class | Modules | Rigor implied |
|---|---|---|
| **A-equivalent** (evidence integrity) | chain append/verify (`src/storage.rs`, `src/lib.rs` seal paths), crypto (`src/crypto`, `signatures.rs`), export/receipts, firmware witness chain (`securacv_witness.cpp`, `securacv_crypto.cpp`), OTA verifier (`securacv_ota.cpp`), offline verifier (`viewer/verify_core.js`) | Flight Rules fully enforced; 100 % branch coverage; property tests + model checking; independent verification |
| **B-equivalent** (ingest & egress) | witnessd loop, API, bridges, adapters, firmware network/MQTT | Flight Rules enforced; behavioral tests must run in CI; fault-injection coverage |
| **C-equivalent** (operator UX) | wizard, dashboards, cards, SPA, display UIs | Standard lint/test bar |

Everything in §4 is filed against this scale.

---

## 2. What we already do that a review board would recognize

Credit where due — these are strengths to protect, and several are ahead of
industry norm:

- **Invariants in the type system.** `tests/compile_fail/` + trybuild make
  "serialize a raw frame" a *compile error*, with pinned `.stderr` goldens.
  This is the strongest form of requirement enforcement that exists.
- **Process isolation around the least-trusted code.** Detector backends run
  in a forked, seccomp-filtered child (`src/module_runtime/sandbox.rs`) with
  no-new-privs, non-dumpable, denied filesystem/network/`execve`/`ptrace`,
  and tests asserting **EPERM specifically**.
- **Failure semantics that are specified *and* tested.** StorageFull,
  StorageWriteFailed, CryptoFailure, ClockSkew, PowerLoss (reopen-detect),
  GapMissingData: each has a detector, a latched sealed record, and a test
  (`witnessd.rs:1502-1664`, `lib.rs:4316-4954`). The two unimplemented types
  are honestly marked "not emitted" instead of faked.
- **Textbook time discipline.** Monotonic `Instant` for all scheduling;
  wall-clock only for buckets; `ClockMonitor` cross-checks the two, seals
  latched `ClockSkew` on NTP steps, detects bucket regression, re-baselines
  with hysteresis (`witnessd.rs:752-828`). Pre-1970 clocks are an error, not
  a crash.
- **Crash-safe firmware chain.** SD-first/NVS-second write order with a boot
  reconciliation that only trusts an SD tail whose hash *recomputes* and whose
  signature *verifies under this device's key* (`securacv_witness.cpp:298-432`).
  Brownout counts persist immediately; graceful shutdown seals a record.
- **OTA done right.** Download to inactive slot → SHA-256 of the *flashed
  bytes* → Ed25519 over (size‖sha) against the release key → only then flip
  boot; NVS anti-rollback floor raised only after the new image passes boot
  self-test; failed self-test auto-rolls back (`securacv_ota.cpp:1582-1651`,
  `653-983`). The release workflow verifies artifacts against the committed
  public key exactly as fielded devices will.
- **Zeroization and domain separation throughout the crypto**; constant-time
  compares on both the kernel token (`subtle::ct_eq`) and firmware token
  (accumulate-XOR); OS RNG everywhere in the kernel.
- **Latched, hysteretic logging** — one record per excursion, INFO once on
  loss and once on recovery; heartbeat records carry per-bucket counter deltas
  so the sealed log itself is a health timeline.
- **Assurance breadth**: 23 workflows; real e2e (live mosquitto + real
  binaries + real SQLCipher + `log_verify`); cross-language byte-parity
  fixtures (Rust ↔ JS verifier); published-image installability gates; an ELF
  DRAM regression guard; a CSI module disable-matrix; actionlint on CI itself.
- **A living mishap archive.** `firmware/LESSONS_LEARNED.md` records real
  failures (SD mount crash-loop, BLE heap starvation, silent route-budget
  drops) *and* the counter-rules adopted. §7's case studies show this is the
  same practice NASA runs at institutional scale.

The gap is not philosophy. It is **uniform application** — the same fix
applied in one place and missed in its sibling (RateLimiter capped, auth
tracker unbounded; frigate_bridge reconnects, event_mqtt_bridge dies;
canary-sense has a bounded MQTT supervisor, canary-vision has the unbounded
loop it replaced). The Flight Rules exist to make the pattern the default.

---

## 3. Verified findings — what our code is doing wrong

Severity: **P0** = can lose/corrupt evidence or permanently silence a witness;
fix before the v1 tag. **P1** = latent trap or trust-eroding gap. **P2** =
hardening/hygiene. Every finding was verified in code at the cited location.

### 3.1 Kernel (Rust)

**K1 · P1 — Read endpoints write, and the busy policy is inherited, not
owned.** *(Corrected during review: originally filed as "P0 — appends fail
immediately on contention." rusqlite 0.31 installs a default 5-second busy
handler at connection open — verified in the pinned version's
`inner_connection.rs` — so short contention waits rather than fails; credit
to automated review on the PR for the catch.)* What remains true and worth
fixing: `export_events_for_api` (`src/lib.rs:2347`) appends a signed export
receipt — a *database write* — on **every** `GET /events`, `/digest`,
`/events/latest` (`src/api/mod.rs:621`), so a Home Assistant coordinator
polling every 30 s produces ~2,880 writes/day of SD wear and a standing
contention window on the evidence store. Any writer that holds the lock
longer than 5 s (large retention prune, WAL checkpoint on a big log, slow SD
storage) still fails a live evidence append with `SQLITE_BUSY`, and neither
the append nor its failure-record fallback retries. And the 5 s policy is an
implicit library default that no code states and no test asserts. Fix: own
the policy (explicit `busy_timeout` in the shared pragma helper), make the
append a single bounded critical section (the K2 transaction), and batch
receipts periodically instead of per-read.

**K2 · P0 — Nothing prevents a forked chain.** `append_record`
(`src/storage.rs:230-275`) is SELECT-head-then-INSERT with **no enclosing
transaction and no single-writer enforcement** (no flock/PID lock).
`frigate_bridge` opens its own writing `Kernel` (`frigate_bridge.rs:353,481`).
Run witnessd and a bridge (or two bridges) against one DB — a supported
misconfiguration one compose edit away — and two writers can read the same
`prev_hash` and fork the chain: the one catastrophic outcome for an evidence
product. Fix: `BEGIN IMMEDIATE … COMMIT` around head-read+insert, plus a
process-level write lock on the DB. Same treatment for the retention
checkpoint+delete pair (`storage.rs:278-346`), which is currently two
autocommit statements.

**K3 · P1 — The egress bridge dies on the first broker hiccup.**
`event_mqtt_bridge`'s eventloop thread does `Err(e) => { warn; break; }`
(`src/bin/event_mqtt_bridge.rs:276-279`) — the thread exits, rumqttc's
reconnection (which lives inside that iterator) never runs, and subsequent
publishes error the daemon out. Its sibling `frigate_bridge` has the correct
outer reconnect loop with exponential backoff (`frigate_bridge.rs:339-386`).
Result: any broker restart silently ends HA state updates until a supervisor
restarts the process. Fix: give it the same reconnect loop.

**K4 · P1 — No boot-time chain check and no liveness watchdog.** Startup
reads the last lifecycle phase with a plain query — no crypto — and appends
onto whatever tail exists; a corrupted/tampered tail is only caught when
someone later runs `/verify`. The main loop has no independent liveness
monitor, and with the default `panic = "unwind"` a dead thread (e.g. the API
thread) leaves a half-alive process. Fix: verify the tail (bounded to the last
checkpoint) before the first append; add a watchdog thread over main-loop
progress + systemd `WatchdogSec`; see K5.
*Resolution (PR #990, issue #918): `witnessd` now verifies the sealed-log
tail at startup and drops to a recorded safe mode on mismatch, and a
watchdog task tracks main-loop liveness. Still open from this finding:
systemd `WatchdogSec` integration (FR-9).*

**K5 · P1 — No `[profile.release]`.** Verified absent from `Cargo.toml`:
release builds run with `overflow-checks = false` (Ariane 5's exact failure
class) and `panic = "unwind"` (half-dead-process mode). Fix: `panic = "abort"`,
`overflow-checks = true`, `lto = true` — three lines.

**K6 · P1 — Unbounded growth and missing socket discipline, each one
inconsistently fixed elsewhere.** `AuthFailureTracker.entries` has no cap or
sweep (`api/mod.rs:243-305`) while its sibling `RateLimiter` is hard-capped
with eviction and a test; the webhook per-path rate map is unbounded
(`adapter/webhook.rs:122-135`) while its neighbor nonce cache is TTL-pruned;
the API has a 2 s read timeout but **no write timeout** and a single-threaded
accept loop (`api/mod.rs:399-427,826`) — one stalled reader wedges `/health`.

**K7 · P1 — The adapter host can wedge itself.** ~20 `.lock().expect()`
poison-panic sites across the adapter daemon (webhook 5, adapter_host 5,
others 2 each). One worker panic while holding a lock poisons it and every
later `.lock().expect()` panics — permanent Track-B ingest outage. The kernel
core deliberately uses poison-tolerant access; the adapter host should too.
Related: a dropped/rejected adapter claim is logged and lost — unlike the
camera path, **adapter-path gaps are never sealed as `GapMissingData`**, so an
outage on the Frigate path is invisible in the evidence record.

**P2 (kernel):** convert the export-path `unreachable!()` (`lib.rs:2617`) to
an error; add jitter to reconnect backoffs; typed failure classification
instead of string-matching (`is_crypto_error`, `lib.rs:1938`); fsync parent
directories after renames; declare MSRV; seccomp filter is a denylist that
misses `io_uring_*`/`pidfd_getfd` — move to an allowlist; add a metrics
endpoint to witnessd/bridges (only adapter_host has one).

### 3.2 Firmware (ESP32)

**F1 · P0 — `canary-vision` can freeze forever, silently.** Zero
`esp_task_wdt` matches in the entire project (verified) even though its config
sets `FEATURE_WATCHDOG 1` and the README claims "Watchdog ✅" — a dead feature
flag. Combined with `mqtt_reconnect_blocking()`
(`src/net/mqtt_mgr.cpp:407-433`, verified): `while (!mqtt.connected())` whose
only exits are broker success or WiFi loss; on live-WiFi/dead-broker it
retries forever, and because `delay(1000)` yields to IDLE, no watchdog panics.
The witness loop stops reading frames until someone power-cycles it.
`canary-sense` already contains the fix — a bounded `mqtt_supervise()` with
exponential backoff (`main.cpp:134-168`) — it was never back-ported. Also fix
the sibling gap in tooling: `regression_check.sh`'s WDT grep passes if *any*
variant has a watchdog, so per-variant checks are needed.

**F2 · P0 (decision) — The witness signing key is recoverable with a USB
cable.** `nvs_store_key()` writes the Ed25519 private key to plaintext NVS
(`securacv_crypto.cpp:306-312`); Flash Encryption and Secure Boot V2 are
commented out even in `sdkconfig.defaults.secure:35-52`, and NVS "encryption"
without flash encryption stores its own keys in the clear. `esptool
read_flash` recovers the key — directly against the stated principle that a
key which can be exported can be compelled. This is a documented Phase-2
deferral, but it ships today: either enable flash encryption + secure boot for
production builds, or state the exposure explicitly in the threat model and on
the trust page. (The app-level anti-rollback floor is NVS-erasable for the
same reason.)

**F3 · P1 — First-boot keygen runs in the weak-entropy window.** The one-time
identity keypair is generated with `esp_fill_random()` in `setup()` before
WiFi/BT is started (`securacv_crypto.cpp:136-140`; ordering
`canary/src/main.cpp:568` vs `:635`), and `bootloader_random_enable()` is
never called — Espressif guarantees RNG entropy only with RF active. Fix: one
call before keygen, or defer keygen past `esp_wifi_start()`.

**F4 · P1 — Fleet reconnect storms are designed-out for mDNS but not for
WiFi/MQTT.** Backoffs have no jitter (`securacv_mqtt.cpp:333-335`,
`securacv_network.cpp:642`) — after a shared outage the fleet retries in
lockstep, the exact storm LESSONS_LEARNED staggered mDNS for; the OTA manifest
fetch already jitters, so the pattern is known. `MQTT_SOCKET_TIMEOUT` is never
pinned, so the "worst bounded block < WDT budget" claim rests on a library
default. Plaintext `WiFiClient` MQTT means chain heads and presence traverse
the LAN unencrypted.

**F5 · P1 — The most security-critical file exists three times, and one
project's two source trees have already diverged.** `securacv_ota.cpp`
(1,763 lines) is byte-identical in `common/ota/src`, `canary-wap/arduino`, and
`canary-display/arduino` with nothing enforcing sync — a fix applied to one
copy silently misses two. `canary-display`'s `src/` and `arduino/` trees
already differ (`dash_ui.cpp` md5 mismatch) with no record of which ships. Fix:
single-source with a CI byte-identity check (the pattern `csi_event.cpp`
already proves works).

**P2 (firmware):** `-Wall -Wextra` but no `-Werror` (`envs/platformio/common.ini:23-24`);
caret-ranged deps for crypto/JSON parsers (pin exact); ArduinoJson elastic
docs in ~50 HTTP handlers as the long-uptime fragmentation vector (arena or
cap); brownout config asserted only in the ESP-IDF sdkconfig, not the shipping
Arduino builds; TLS fail-open serves plaintext outside setup mode
(`canary_wap.ino:10107-10113`) against the stated no-fallback rule; per-record
sign-then-verify doubles loop-path crypto cost.

### 3.3 Assurance, CI, and configuration management

**A1 · P0 — The untrusted-input surface is compiled but never behaviorally
tested in CI.** All six adapters are feature-gated off by default
(`src/adapter/mod.rs:26-38`) and **no CI `cargo test` enables any adapter
feature** (verified: the only feature'd test runs are pqc, ingest-ffmpeg,
rtsp). Excluded from every merge: ~42 adapter unit tests + 7 integration
files, including `adapter_cannot_bypass_enforcer.rs` and the parser fuzz-lite.
Fix is one CI job: `cargo test --features adapter-frigate,adapter-mqtt-sensor,adapter-webhook,adapter-webhook-tls,adapter-ble-presence,adapter-meshtastic,adapter-sandbox`.

**A2 · P0 — Version drift with no gate.** Verified today: `Cargo.toml`
**0.5.0** vs integration manifest **0.6.0** vs add-on config **0.6.0**. The
kernel binary reports a different version than the published image tag built
from it. `scripts/bump_version.sh` exists; nothing enforces it ran. Fix: a
five-line CI check that fails when the three disagree, and startup version
telemetry (every component logs+publishes its version and config hash — Knight
Capital's lesson).

**A3 · P1 — The device-signature wire contract has no cross-language golden
vectors.** `SCHEMA_V = 1` + canonical byte layouts are duplicated as constants
in firmware C++ (`device_signature.h:42`) and HA Python
(`signature.py:36-37`); each side tests itself in isolation, so coordinated
drift passes CI and fails only on hardware. The envelope contract already has
the cure — shared fixtures verified byte-for-byte by Rust and JS
(`tests/fixtures/envelope/*`) — replicate it: firmware-signed golden messages
committed as fixtures the Python verifier must accept.

**A4 · P1 — Supply-chain posture lags the product's own trust story.** Docker
bases tag-pinned not digest-pinned; `provenance: false` on all three publish
workflows; no cosign/SLSA attestations; SBOMs are 90-day CI artifacts (not
committed/attested) and the firmware SBOM is a hand-written heredoc pinned to
a stale version; the documented installer is `curl | bash` off mutable `main`
with no checksum; `release.yml` builds amd64-only, unsigned, and is the one
workflow that floats an action (`hacs/action@main`). A witness product's
binaries should carry the same provenance discipline as its evidence: signed
releases + attestation is table stakes (§6 FR-14).

**P2 (assurance):** no property-based tests or coverage-guided fuzzing
anywhere (parsers, export verifier, API are the targets); no coverage
measurement at all (which is why A1 was invisible); no real kill-9/torn-write
power-loss test (reopen simulation only); invariants have no IDs and no
requirements→test matrix; `conformance.sh` exists but is wired to no workflow;
Invariant VII (non-queryability) and pre-roll zeroization lack direct tests;
ruff scope is narrow (no security lints) and misses `scripts/`; `install.sh`
is not shellchecked; no cargo-deny/cargo-vet; CodeQL runs default queries
only; `.github/workflows/` is not CODEOWNERS-gated while `/spec` is.

---

## 4. FMEA of the evidence pipeline

The half-day exercise NASA runs as SFMEA, applied to our pipeline. ✓ = designed
detection/response exists today (with the mechanism); **gap** = finding above.

| Stage | Failure mode | Detected? | Contained? | Response today |
|---|---|---|---|---|
| Capture (RTSP/sensor) | source down / stalls | ✓ ingest supervisor timeouts | ✓ | one latched `GapMissingData` + backoff reconnect |
| Detect (module) | malicious/buggy backend | ✓ type system + seccomp | ✓ child process | EPERM/SIGKILL; **gap: io_uring not denied (denylist)** |
| Adapter claim (Track B) | malformed input | ✓ parsers total, fuzz-lite exists | ✓ | **gap A1: none of it runs in CI** |
| | dropped/rejected claim | **gap K7: logged, not witnessed** | — | add adapter-path `GapMissingData` |
| | adapter host wedge | **gap K7: poison cascade** | ✗ | none — permanent ingest outage |
| Append/seal | write lock held > 5 s | **gap K1: append fails, no retry** | partial | rusqlite's default 5 s handler absorbs short contention; fallback can also fail |
| | second writer | **gap K2: chain fork possible** | ✗ | none |
| | storage full/write fail | ✓ DiskMonitor preflight, latched | ✓ | sealed failure record → alarm → stderr |
| | power loss mid-append | ✓ WAL + FULL sync; lifecycle records | ✓ | PowerLoss sealed on reopen; **gap: no torn-write test** |
| | clock step | ✓ ClockMonitor dual-source | ✓ | latched ClockSkew + re-baseline |
| Store/retain | checkpoint crash window | partial | ✓ | **gap K2: two autocommit stmts; duplicate checkpoint on replay** |
| | tampered tail at rest | ✗ at boot | — | **gap K4: only found on manual /verify** |
| Verify | scheduled + on-demand | ✓ 24 h + button + boot? | ✓ | **gap K4: not at boot; latching is HA-side only** |
| Export | receipt chain | ✓ signed receipts, offline verifier | ✓ | (cost: K1 — receipts on every read) |
| Publish (MQTT/HA) | broker down | ✓ ingest bridge reconnects | ✓ | **gap K3: egress bridge dies instead** |
| | duplicate delivery | ✓ retained topics idempotent | ✓ | active-bucket republish duplicates motion triggers (P2) |
| Firmware chain | power loss mid-record | ✓ SD-first + verified reconcile | ✓ | at most the in-flight line lost |
| | dead broker | **gap F1 (vision): frozen loop** | ✗ | none on vision; bounded supervisor on sense |
| | key theft (physical) | **gap F2: plaintext NVS** | ✗ | Phase-2 deferral, undocumented in trust page |
| | counter wrap (49.7 d `millis`) | ✓ folded into hash; GPS attests time | ✓ | document consumer contract (P2) |

The table is the monitor shopping list: every **gap** row maps 1:1 to a finding
in §3 and a rule in §6.

---

## 5. The mishap mirror — famous failures, found in our repo

The strongest argument for each fix is that its absence already has a name.

| Mishap | The lesson, one line | Where it lives in this repo today |
|---|---|---|
| **Ariane 5** (1996) | unhandled arithmetic faults + reuse without revalidation | `overflow-checks = false` in release (K5); no release profile at all |
| **Mars Climate Orbiter** (1999) | interface contracts must be machine-enforced | `SCHEMA_V` duplicated as constants in C++/Python with no shared golden vectors (A3) |
| **Mars Pathfinder** (1997) | ship observability; the watchdog turns hangs into diagnosable resets | no main-loop watchdog in witnessd (K4); no metrics on witnessd/bridges; the *fix* pattern (trace in production) is why defmt-style logging stays on |
| **Toyota UA** (2013) | a watchdog that doesn't prove every task is alive is decoration | canary-vision: WDT absent while config claims it (F1); regression check satisfied by any variant |
| **Boeing 787 GCU** (248-day rollover) | uptime is an input; soak past every counter epoch | `millis()` bucket wrap at 49.7 days (documented-safe but untested); no soak/time-acceleration test anywhere (P2) |
| **Therac-25** | software must not be the sole barrier; anomaly reports are real | the offline viewer *is* our independent barrier (strength — keep its byte-parity gate); latching faults must survive operator dismissal |
| **MER Spirit sol 18** | the recorder's storage is its most likely assassin; boot must survive it | storage-full is latched & sealed (strength ✓); but a corrupt/tampered tail still boots un-checked (K4) |
| **Knight Capital** (2012) | deployment/config *is* the software; verify what's running | 0.5.0 vs 0.6.0 shipping simultaneously with no gate (A2); `curl\|bash` off mutable main (A4) |

Two rows are strengths: we already have Spirit's storage answer and Therac's
independent verifier. The others are open findings.

---

## 6. The Flight Rules — our engineering constitution

Fourteen standing rules. Each is short, checkable, CI-enforceable, traceable
to a NASA source, and scoped by criticality class (§1). "Today" is the honest
current state. Proposal: adopt these as `docs/FLIGHT_RULES.md`, cite the rule
ID in PR reviews, and land the enforcement column into CI over Phases 0–2
(§8).

| # | Rule | Lineage | Enforcement | Today |
|---|---|---|---|---|
| **FR-1** | **The build is silent.** All warnings on, warnings are errors, static analysis on every commit — Rust *and* firmware. | P10 №10; JPL LOC-1 | clippy `-D warnings` (have) + `-W pedantic` (Class A); `-Werror` in platformio common.ini; cppcheck-MISRA gate | Rust ✓ / firmware ✗ (no -Werror) |
| **FR-2** | **Class A/B paths cannot panic.** No unwrap/expect/panic/indexing on daemon or firmware steady-state paths; errors propagate to a defined handler. | P10 №5-7; SWE-134 | clippy restriction lints (`unwrap_used`, `expect_used`, `panic`, `indexing_slicing`) denied per-crate; release `panic="abort"` | kernel core ✓ by practice; adapter host ✗ (20 lock-expects); no lint gate |
| **FR-3** | **No `unsafe` outside audited FFI modules.** App crates `#![forbid(unsafe_code)]`; FFI islands listed and reviewed; dependency unsafe measured. | P10 №9 | forbid attribute + cargo-geiger report | unsafe already minimal & justified; no forbid/measurement |
| **FR-4** | **Everything is bounded.** Fixed caps on every queue, map, buffer, and retry loop; steady-state heap growth is a defect. | P10 №2-3; JPL №5 | bounded channels; cap+evict pattern (RateLimiter is the template); allocation-counter soak test | uneven: K6, F4 gaps |
| **FR-5** | **Assert invariants — and recover.** ≥2 checks/function on Class A paths; a failed check seals a fault event and enters a defined mode, never silent continuation and never bare abort. | P10 №5; NASA-HDBK-1002 | debug_asserts + runtime checked invariants; cargo-mutants proves they bite | failure-semantics ladder ✓; mutants ✗ |
| **FR-6** | **Every return value is handled.** `let _ =` requires a justifying comment. | P10 №7; MISRA Dir 4.7 | `unused_must_use` deny; clippy `let_underscore_must_use` | low silent-swallow count ✓; no gate |
| **FR-7** | **Tasks communicate by message; locks only at leaves, never nested, never poisoned into cascade.** Single owner per resource. | JPL №6/8/9 | channels/actors; poison-tolerant access pattern (kernel core is the template); loom on residuals | kernel ✓; adapter host ✗ (K7) |
| **FR-8** | **Every mode is explicit; boot always succeeds into a reporting state.** Enumerated states incl. SAFE/degraded; transitions are logged events; the kernel boots and reports even with its store full or corrupt — and verifies its tail before the first append. | SWE-134; HDBK-1002; Spirit | exhaustive `match` on mode enums; corrupt-store/disk-full boot cases in the DITL suite | firmware FSMs ✓; kernel boot-verify ✗ (K4) |
| **FR-9** | **The watchdog proves every task is alive.** Per-task check-in vote; never fed from a timer ISR; a trip latches a logged cause and escalates (task → process → reboot → safe mode). | JPL/cFS HS; Koopman; Pathfinder/Toyota | esp_task_wdt per task per variant (per-variant CI check); witnessd watchdog thread + systemd `WatchdogSec` | wap/sense/display ✓; vision ✗ (F1); witnessd ✗ (K4) |
| **FR-10** | **No anomaly without an observable; no resource without a margin alarm.** Every fault emits a structured event; CPU/RAM/storage/queue depth/chain length each have telemetry plus margin thresholds that alarm — margins, not capacity. | HDBK-1002; Spirit; GSFC GOLD | metrics endpoints on witnessd+bridges; margin config; startup version+config-hash telemetry | sealed heartbeats/status ✓; metrics asymmetric; margins implicit |
| **FR-11** | **Scrub the record.** Chain segments re-verify continuously against anchors in the background; any mismatch latches until an operator clears it; boot verifies the tail. | EDAC/scrubbing; cFS CS app; SWE-134 | scheduled verify (have, 24 h) + boot tail-verify + bit-flip injection test proving it fires | partial (K4) |
| **FR-12** | **Test like you deploy.** Before a release: ≥24 h day-in-the-life on the real rig (ESP32s + daemon + broker) with power-pulls, broker kills, clock jumps, disk-full; soak/time-acceleration past every counter epoch; every deviation from real conditions listed with rationale. | Aerospace TLYF TORs; 787 AD | HIL rig + scripted fault campaign + deviation ledger in the release checklist | e2e in CI ✓; DITL/HIL/soak ✗ |
| **FR-13** | **One dictionary.** Topics, payload schemas, event/attestation vocabularies, and config tables come from one machine-readable source; bindings and docs are generated; CI fails on drift — including cross-language golden vectors for every signature format. | cFS tables; CCSDS XTCE; MCO | schema codegen + drift check; envelope-fixture pattern extended to device signatures (A3) and the const.py/JS vocab copies | envelope ✓; device-sig + vocab ✗ |
| **FR-14** | **Nothing ships unvetted or unverifiable.** Deps pass cargo-deny (+vet for Class A); releases carry signed provenance (cosign/SLSA) with committed SBOMs; installers verify checksums; every node reports version+config hash at startup; one version, one gate. | NPR 7150.2 assurance; Knight | cargo-deny/vet; GitHub attestations; digest-pinned bases; version-sync CI check; pinned install.sh | audit/dependabot ✓; the rest ✗ (A2/A4) |

Meta-rule (Holzmann's actual point): every rule above is **machine-checkable**.
If a proposed engineering rule can't be checked by a tool or a test, rewrite
it until it can.

---

## 7. Practices to institutionalize (the foundational layer)

Beyond point fixes — the four artifacts that make rigor self-sustaining:

1. **Requirements IDs + a generated traceability matrix.** Give the
   invariants and failure semantics stable IDs (`INV-1..7`, `FS-1..8`,
   `FR-1..14`), tag tests with them (in-name or attribute), and generate
   `docs/traceability.md` in CI (NPR SWE-052). The audit above found the gaps
   by hand; the matrix finds them automatically, forever. Bonus: the matrix
   itself is a marketing artifact for an evidence product.
2. **A permanent HIL rig + DITL releases.** Two or three real canaries + a Pi
   running the add-on + mosquitto on a smart plug, driven by a scripted fault
   campaign (power pulls, broker kills, WiFi loss, clock jumps, disk fill).
   Runs continuously; a ≥24 h clean day-in-the-life is a release gate
   (`v1_bench_validation_runbook.md` already sketches Track A/B/C — this makes
   it standing infrastructure, not a one-time gate).
3. **A deviation ledger.** One file listing every way tests differ from
   reality ("no real Frigate ML in CI — nondeterministic; covered by manual
   smoke") — TLYF's "no-test rationale." Converts silent risk into reviewed
   decisions; several entries already exist implicitly in v1-roadmap.md.
4. **The OpenSSF ladder as the public assurance score.** Scorecard + Best
   Practices badge are the OSS analog of a NASA assurance audit — most of the
   checks (branch protection, pinned deps, fuzzing, SAST, signed releases) are
   the same items as A4/P2, and the badge is externally legible trust for
   exactly the audience a witness product courts.

---

## 8. Roadmap

**Phase 0 — Stop the bleeding (P0s plus the K1 hardening; days, all small):**
K2 transactional append + single-writer discipline · K1 explicit busy policy
(own the pragma) with receipt batching to follow · F1 vision watchdog +
bounded reconnect (port from sense) · A1 adapter-features test job · A2
version-sync gate + `bump_version.sh` in the release checklist · K5
`[profile.release]`.

**Phase 1 — Uniform application (P1s; weeks):**
K3 egress reconnect · K4 boot tail-verify + witnessd watchdog + systemd
WatchdogSec · K6/K7 cap-and-evict + poison-tolerant adapter host + adapter-gap
sealing · F2 flash-encryption/secure-boot production decision (and threat-model
honesty either way) · F3 entropy before keygen · F4 jitter + socket timeout ·
F5 single-source OTA file + tree-divergence CI check · A3 device-signature
golden vectors · FR-1 firmware `-Werror` · FR-2/3/6 lint gates.

**Phase 2 — Assurance depth (P2s; 1–2 months):**
proptest + cargo-fuzz on parsers/verifier (OSS-Fuzz onboarding) · kill-9/
torn-write + clock-jump + disk-full fault-injection tests · coverage
measurement with Class-A 100 % branch target · cargo-deny/vet · digest pins +
cosign/SLSA attestations + committed SBOMs + pinned installer · invariant IDs
+ traceability matrix · seccomp allowlist · Kani harness on the chain
append/verify state machine (now practical at stdlib scale).

**Phase 3 — Foundational (standing):**
HIL rig + DITL release gate + soak/time-acceleration past `millis()` wrap and
the 10-min-bucket epochs · deviation ledger · witnessd/bridge metrics with
margin alarms · `docs/FLIGHT_RULES.md` adopted and cited in review · OpenSSF
badges · annual "mishap drill" (inject a fault from §5's table; verify the
record tells the story).

Sequencing rationale: Phase 0 items all sit on the evidence path or silence a
witness — they gate the v1 tag. Phase 1 is the same fixes the codebase already
contains, applied uniformly. Phase 2 converts review-board findings into
machine checks so they can't regress. Phase 3 is what makes it foundational:
the rig, the rules, and the traceability that outlive any one contributor.

---

## 9. Sources

Repo evidence: file:line citations throughout §2–§5, independently verified
for every P0 (`src/lib.rs`, `src/storage.rs`, `src/api/mod.rs`,
`src/bin/event_mqtt_bridge.rs`, `src/bin/frigate_bridge.rs`,
`src/adapter/*`, `firmware/projects/canary-vision/src/net/mqtt_mgr.cpp`,
`firmware/common/*`, `sdkconfig.defaults.secure`, `.github/workflows/*`,
`Cargo.toml`, `custom_components/securacv/manifest.json`,
`privacy_witness_kernel/config.yaml`).

Standards: The Power of Ten (Holzmann, spinroot.com/gerard/pdf/P10.pdf); JPL
Institutional Coding Standard D-60411; NPR 7150.2D + NASA Software Engineering
Handbook (swehb.nasa.gov — SWE-052 traceability, SWE-134 safety-critical
design, SWE-189/190/219 coverage, SWE-141 IV&V); NASA Fault Management
Handbook NASA-HDBK-1002 (FDIR, fault containment, safe mode, latching);
GSFC-STD-1000 GOLD Rules (margins); Aerospace Corp TOR-2010(8591)-6 and
TOR-2014-02537 Rev A (Test Like You Fly); Koopman on watchdog practice; cFS
(github.com/nasa/cFS — HS/CS/LC app patterns) and F´ (github.com/nasa/fprime).

Mishap reports: Ariane 501 inquiry board; Mars Climate Orbiter MIB Phase I;
Reeves' Mars Pathfinder account; Barr/Koopman Toyota UA testimony; FAA AD
2015-10066 (787 GCU); Leveson & Turner Therac-25 investigation; Reeves &
Neilson MER Spirit flash anomaly; Knight Capital case studies.

Modern tooling: clippy restriction lints; cargo-deny/audit/vet/geiger/mutants;
Miri/loom/Kani; proptest/cargo-fuzz + OSS-Fuzz; defmt/probe-rs; SLSA +
Sigstore cosign; OpenSSF Scorecard & Best Practices badge; Ferrocene (the
qualified-Rust existence proof).
