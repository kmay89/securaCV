# AGENTS.md — the brief for any AI agent working in this repo

**This file is the canonical brief.** It is vendor-neutral on purpose: every
assistant that works here reads these same rules, through whichever filename its
tool looks for.

| Tool | File it reads | What that file is |
|---|---|---|
| Codex, Cursor, Jules, Aider, Zed, Devin, most others | `AGENTS.md` | **this file** |
| Claude Code | [`CLAUDE.md`](CLAUDE.md) | project rules + a pointer here |
| Gemini CLI | [`GEMINI.md`](GEMINI.md) | generated pointer |
| Qwen Code | [`QWEN.md`](QWEN.md) | generated pointer |
| GitHub Copilot | [`.github/copilot-instructions.md`](.github/copilot-instructions.md) | generated pointer |
| Cursor (rules format) | [`.cursor/rules/securacv.mdc`](.cursor/rules/securacv.mdc) | generated pointer |
| Cline | [`.clinerules`](.clinerules) | generated pointer |
| Windsurf | [`.windsurfrules`](.windsurfrules) | generated pointer |

The pointer files are **generated** from the brief block below by
`scripts/gen_agent_entrypoints.py` and byte-checked in CI, so they cannot drift.
Edit this file, then run the generator — never edit a pointer file by hand.

## Read these first

| If you're doing this | Read |
|---|---|
| Anything at all | The non-negotiables block below |
| Wondering what a word means | [`docs/GLOSSARY.md`](docs/GLOSSARY.md) — every proper noun in the project, defined once |
| Answering a user's question about the project | [`docs/FAQ.md`](docs/FAQ.md) |
| Looking for a doc | [`docs/README.md`](docs/README.md) — the CI-enforced map of every doc under `docs/` |
| Wondering which directory is the real one | [`docs/CONSOLIDATION.md`](docs/CONSOLIDATION.md) — the tree map |
| Writing code | [`docs/FLIGHT_RULES.md`](docs/FLIGHT_RULES.md) — the engineering constitution |
| Shipping or releasing | [`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md), then [`.github/RELEASE_LESSONS.md`](.github/RELEASE_LESSONS.md) |

---

<!-- BEGIN AGENT-BRIEF — generated into the vendor entrypoint files; do not
     delete these markers. Keep it short: it is copied verbatim. -->

## What this project is

**SecuraCV** is privacy-preserving witness infrastructure: it turns camera and
sensor input into **semantic events** ("large object crossed boundary") in a
signed, hash-chained log — never a searchable pile of footage. The platform is
**SecuraCV**, the device is a **Canary**, the company is **Errer Labs**. The
core is a Rust daemon (`witnessd`, in `src/`); Canaries are ESP32 firmware (in
`firmware/`).

The design rule behind everything: guarantees are **`can't`, not `won't`**. The
surveillance code was never written, so there is no setting to turn off.

## Non-negotiables

**1. Never add an identity-inferring capability.** No face recognition or
embeddings, no license-plate OCR, no person re-identification, no gait analysis,
no demographic (age/gender/race) estimation, no audio transcription of anything
witnessed — nothing in this project ever turns overheard speech into text, and
no witness pipeline grows a speech model. The one carve-out is bounded by
hardware, not by who is speaking: voice commands to the hub entering through a
dedicated voice satellite — never a Canary, which structurally cannot feed a
speech pipeline — with push-to-talk as the blessed default. Wake-word listening
is an explicit owner opt-in on that satellite and is honestly a `won't`, not a
`can't`: a false wake transiently transcribes a few seconds of room audio,
which is why command transcripts are never retained, sealed, or exported. The
full five-rule contract is in `docs/research/whisper_local_voice.md`. `ObjectClass`
is `Person | Vehicle | Animal | Package` (plus `Unknown`, for a detection the
backend could not class) — never `Face` or `LicensePlate`. This
is Invariant II (`spec/invariants.md`) and it is a rejected PR, not a config flag.

**2. Never widen the raw-frame escape hatch.** `RawFrame.data` stays private: no
public getter, no `Clone`, no `AsRef<[u8]>`. The only path to raw bytes is
`export_for_vault()` behind a `BreakGlassToken`, which needs n-of-m trustee
approval.

**3. Say "fleet," never "flock."** A group of Canaries is a **fleet**. "Flock" is
off-limits in user-facing copy, device UI strings, product and bundle names, code
identifiers, and comments — a company called Flock soured the word. The **only**
exception is the Unix `flock(2)` syscall, which is a real API name; do not rename
it. Use "fleet" (already established across the firmware, e.g. `fleet_model.h`)
or plain "your Canaries" / "the devices."

**3b. US spellings, always.** Write `color`, `center`, `meter`, `behavior`,
`analyze`, `gray`, `license`, `labeled`, `canceled`, `optimize`, `recognize`,
`catalog` — never the British forms of those words. This covers user-facing
copy, device UI strings, docs, comments and code identifiers alike, and
`scripts/lint_spelling.py` fails the build on any of them.

**The banned forms are enumerated ONCE, in that script's regex — read them
there, and do not repeat them in prose here.** This paragraph used to spell
them out as "write X, not Y", and the first repo-wide sweep duly rewrote its
own rule: the "not Y" column came back as a list of the *correct* spellings,
so the rule forbade exactly what it required. A regex in a linter is the one
place a sweep has no reason to touch.

Not ours to respell: SPDX tags and license filenames, third-party API and CSS
identifiers already spelled a particular way, and quoted external text. And
some words only *look* British to a substring match — `analysis`, `emphasis`,
`parameter`, `diameter`, `characteristic`, `realistic`, `optimistic`,
`initialism` are all correct. The linter's `ALLOW` list names them and
self-tests that the ban pattern never starts matching them; that check earned
itself on its first run, when `colou?r` matched `colored`.

**4. Don't oversell, and don't overclaim.** "Verified" means *an Ed25519
signature checked against a pinned key* — nothing looser. No performance claim
without a benchmark. Describe `DetectorBackend` as an audit boundary that must be
audited, not as something that "cryptographically enforces" anything. Where a
guarantee isn't structural yet, say so out loud.

**5. Vocabulary changes start in the dictionary.** Event types, failure types,
attestation tiers, claim kinds and modalities are duplicated as constants across
Rust, Python, JS, firmware C++ and Swift (the Apple apps' `EventVocabulary`).
`spec/witness_dictionary.json` is the single source of truth and
`scripts/lint_dictionary_sync.py` fails CI on any drift. Edit the dictionary
first, then every copy the linter names.

**6. A new doc gets a home on the map in the same commit.**
`scripts/lint_docs_index.py` fails the build if a doc under `docs/` isn't
reachable from `docs/README.md`, or if a link there is dead.

**7. Two flashers, two frontends.** The in-browser flasher
(`canary-local/assets/`) and the desktop Flasher app (`desktop/src/`) share no UI
code. A user-facing diagnostic added to one must be added to the other, or half
the users keep the vague version.

**8. Beacon and Chirp have their own hard invariants** — two-pubkey co-signing,
no automatic origination, no PII on the wire, no impersonation of official
alerts, ephemeral session keys. If you touch `firmware/projects/canary-wap/`
beacon or chirp code, read the Beacon section of `AGENTS.md` in full first.

## Before you commit

- `cargo test`, `cargo clippy` (no warnings), `cargo doc` (no warnings)
- `python3 scripts/lint_docs_index.py` if you touched anything under `docs/`
- `python3 scripts/lint_dictionary_sync.py` if you touched a vocabulary
- `python3 scripts/gen_agent_entrypoints.py` if you edited this brief
- Changed an enclosure `.scad`? Attach PNG previews of every affected part
  for the requester — the change must be seeable, not just readable
  (recipe: `docs/hardware/enclosure/README.md`, "Preview renders")
- Changed a builder-curated `.scad`, one of its `use<>` libraries, or the
  fleet figures? Refresh the website's carried copies too:
  `docs/hardware/enclosure/gen_builder_manifest.py --site <website-checkout>`
  (the site pins them by sha256; its weekly "Update everything" carry job
  catches a forgotten refresh, but a week is a long time to serve stale CAD)
- Commit format: `<type>(<scope>): <description>` — `feat`, `fix`, `docs`,
  `test`, `refactor`, `chore`

<!-- END AGENT-BRIEF -->

---

## Where do I look for X?

| Question | Answer |
|---|---|
| The kernel / the daemon | `src/` — the real Rust product, `witnessd` and its binaries |
| Device firmware | `firmware/canary/` (canonical) and `firmware/projects/<product>/` |
| The normative contracts | `spec/` — read `spec/README.md` first for each doc's maturity (🟢 stable / 🟡 draft / ⚪ spec-only) |
| The in-browser Lab | `canary-local/` — real firmware compiled to WebAssembly |
| The desktop Flasher / hub app | `desktop/` (plus `desktop/hub-core`, `desktop/hub-io`) |
| The Lab desktop app | `desktop-lab/` — a different app from the Flasher |
| Home Assistant integration | `custom_components/securacv/` + the app (add-on) wrapper `privacy_witness_kernel/` |
| Apple targets | `ios/` (iPhone/iPad), `tvos/` (the Witness Wall) |
| Architecture docs, not code | `kernel/` — docs only, despite the name |
| What a word means | `docs/GLOSSARY.md` |
| What a *thing* looks like | `canary-local/devices/figures.json` + `canary-local/figures/*.svg` — one isometric figure per device, part, board and tool, generated from the committed CAD. Spec: `docs/design/FLEET_FIGURES.md` |
| Whether something is real or still an idea | the same ledger's `confidence` — derived from evidence on disk (committed STLs · firmware config · released catalog variant), never hand-typed |
| Which tree is the real one | `docs/CONSOLIDATION.md` |
| Machine-readable vocabularies | `spec/witness_dictionary.json` |
| Which build has which feature | `firmware/build_matrix.json` (generated truth), not `firmware/FEATURES.md` (narrative, can lag) |
| Which products/envs ship | `firmware/flavors.json` |
| Board hardware-verification status | `firmware/boards/boards.json` — `verified` vs `compile-tested` |
| Release targets | `.github/release-targets.yml` — add a target there, not in workflow YAML |

## CI gates you will trip

These run on every PR. Run the relevant one locally before you push.

| Gate | What it enforces |
|---|---|
| `scripts/lint_docs_index.py` | Every doc reachable from `docs/README.md`; no dead links |
| `scripts/lint_dictionary_sync.py` | Rust/Python/JS/firmware vocabularies match `spec/witness_dictionary.json` |
| `scripts/gen_agent_entrypoints.py --check` | Vendor agent files match this file's brief block |
| `scripts/lint_no_impersonation.sh` | No red, no reserved emergency tones, no official-alert phrasing |
| `scripts/lint_build_matrix.py` | `build_matrix.json` matches `platformio.ini` + `canary_config.h` |
| `scripts/lint_feature_flags.sh` | Feature-flag hygiene |
| `scripts/lint_version_sync.sh`, `desktop/scripts/check_app_versions.py` | One version per app across `tauri.conf.json` / `package.json` / `Cargo.toml` |
| `scripts/lint_bom.py` | BOM CSVs schema-clean and wired to the generator |
| `scripts/lint_cloudkit_container.py` | No `CKContainer.default()`; the container identifier matches both entitlements files |

Full list: [`.github/workflows/lint.yml`](.github/workflows/lint.yml) and
[`docs/ci.md`](docs/ci.md).

---

## Project Identity

SecuraCV witness-kernel: Privacy-preserving video event system. Outputs semantic events ("motion in zone A") without exposing raw video or identity data.

**Core value proposition:** Tamper-evident, privacy-preserving witness artifacts.

---

## Architecture Overview

```

┌─────────────────────────────────────────────────────────────┐
│                     WITNESS-KERNEL                          │
├─────────────────────────────────────────────────────────────┤
│  Video Source ──► RawFrame ──► InferenceView ──► Backend   │
│      (private data)    (restricted view)    (audit boundary)│
│                                                  │          │
│                                       DetectionResult       │
│                                       (no identity data)    │
│                                                  │          │
│                                       CandidateEvent        │
│                                                  │          │
│                                       Sealed Log            │
│                                       (Ed25519 signed)      │
└─────────────────────────────────────────────────────────────┘

````

---

## Privacy Invariants (Non-Negotiable)

### Invariant I: No Raw Export
`RawFrame.data` is private. No public getter, no `Clone`, no `AsRef<[u8]>`.
Only path to raw bytes: `export_for_vault()` with `BreakGlassToken`.

### Invariant II: No Identity Substrate  
No face embeddings, plate strings, person re-ID vectors, demographic estimates.
`ObjectClass` enum: `Person`, `Vehicle`, `Animal`, `Package`, `Unknown` (could not class) — NOT `Face`, `LicensePlate`.

### Invariant III: Metadata Minimization
Timestamps coarsened to 10-minute buckets. Zone IDs are local only. Correlation tokens are single-use.

### Invariant IV: Local Ownership
All logs stored locally. No remote indexing. No telemetry.

### Invariant V: Break-Glass by Quorum
Raw media access requires N-of-M trustee approval.

### Invariant VI: No Retroactive Expansion
`ReprocessGuard` checks ruleset hash. New rulesets cannot reprocess old data.

### Invariant VII: Non-Queryable
No bulk search. No identity selectors.

---

## Detection Backend Guidelines

### ⚠️ CRITICAL: Security Model

**`DetectorBackend` is an audit boundary backed by a process-isolation sandbox.**

The trait is an API contract that still must be manually audited for what the kernel can't
enforce in-process:
- Cloning/caching pixel data internally
- Computing identity-linked data

But in the production pipeline `detect()` runs inside a forked, seccomp-restricted child process
(`module_runtime::execute_sandboxed`, used by `witnessd`; see `src/module_runtime/sandbox.rs`).
The child denies the filesystem, network, `execve`, `ptrace`, and `process_vm_*` syscalls, so a
malicious/buggy backend **cannot**:
- Make network calls
- Write to disk
- Shell out (`execve`) or read the parent's key material (`process_vm_readv` / `ptrace`)

Cross-frame state (e.g. motion's previous-frame hash) survives the boundary via
`export_state`/`import_state`. The sandbox is a syscall **denylist** (defense in depth), not an
airtight allowlist, and — like every kernel guarantee — assumes an uncompromised host
(`docs/root_paradox.md`). WASM sandboxing remains a future hardening option for an allowlist model.

### Backend Feature-Gate Rule

**Backends must be feature-gated unless they are pure-Rust and dependency-minimal.**

- `StubBackend` — no feature gate (pure Rust, no deps beyond sha2)
- `TractBackend` — `#[cfg(feature = "backend-tract")]`
- `OnnxBackend` — `#[cfg(feature = "backend-onnx")]`
- Any backend with native deps — must be feature-gated

### Allowed Capabilities (v1)

```rust
pub enum DetectionCapability {
    Motion,           // Frame differencing
    ObjectDetection,  // Bounding boxes
    Classification,   // Person/vehicle/animal
}
````

That's it for v1. No segmentation, pose, depth, optical flow.

### Forbidden Capabilities (NEVER implement)

* `FaceRecognition` / `FaceEmbedding`
* `LicensePlateOCR`
* `PersonReidentification`
* `DemographicEstimation` (age, gender, race)
* `AudioTranscription`

### Backend Audit Checklist

Before merging ANY new backend:

* [ ] `detect()` does not store pixels beyond the call
* [ ] No network operations (no HTTP, no sockets, no DNS)
* [ ] No disk writes (no file I/O, no temp files)
* [ ] No identity-linked computations
* [ ] No telemetry or analytics
* [ ] Dependencies audited for the above
* [ ] Feature-gated if has native dependencies

---

## Code Style

```rust
// Use anyhow for errors
use anyhow::{anyhow, Result};

// Non-exhaustive enums
#[non_exhaustive]
pub enum ObjectClass { ... }

// Derive standard traits
#[derive(Debug, Clone, PartialEq)]
pub struct Detection { ... }

// Document public APIs
/// Runs detection on a frame.
pub fn detect(&mut self, pixels: &[u8]) -> Result<DetectionResult>
```

---

## Testing Requirements

### Every PR must pass:

* `cargo test`
* `cargo clippy` (no warnings)
* `cargo doc` (no warnings)

### Feature flag testing:

* `cargo test --no-default-features`
* `cargo test --features backend-tract`

---

## Common Pitfalls

### DON'T: Add `Clone` to `RawFrame`

```rust
// BAD
#[derive(Clone)]
pub struct RawFrame { ... }
```

### DON'T: Add identity fields to `DetectionResult`

```rust
// BAD
pub struct DetectionResult {
    pub face_embedding: Vec<f32>,  // FORBIDDEN
}
```

### DON'T: Oversell security properties

```rust
// BAD
/// Cryptographically enforces privacy at the type level

// GOOD
/// Defines an audit boundary. Implementations must be manually audited.
```

### DON'T: Promise performance without benchmarks

```rust
// BAD
/// Runs YOLOv8 at 30fps

// GOOD
/// Performance varies by hardware. Benchmark before deployment.
```

### DON'T: Use `&self` when backend needs mutable state

```rust
// BAD - forces Mutex everywhere
fn detect(&self, ...) -> Result<DetectionResult>;

// GOOD - allows internal buffers
fn detect(&mut self, ...) -> Result<DetectionResult>;
```

---

## File Locations

### Core kernel

* `src/lib.rs` — main kernel, types, event handling
* `src/frame.rs` — `RawFrame`, `InferenceView`, `FrameBuffer`

### Detection

* `src/detect/mod.rs` — module exports
* `src/detect/backend.rs` — `DetectorBackend` trait
* `src/detect/result.rs` — `DetectionResult`, `Detection`
* `src/detect/registry.rs` — `BackendRegistry`
* `src/detect/backends/` — backend implementations

### Binaries

* `src/bin/witnessd.rs` — main daemon
* `src/bin/log_verify.rs` — log integrity verification

### Specs

* `spec/invariants.md` — canonical invariant definitions
* `spec/event_contract.md` — event type contracts
* `spec/threat_model.md` — threat model

---

## Release & Packaging (all app targets)

Before touching **any** app build/release workflow — the Flasher, the Lab,
or the **iPhone / iPad / tvOS / Mac** targets — read
[`.github/RELEASE_LESSONS.md`](.github/RELEASE_LESSONS.md). It's the canonical,
growable home for build/release lessons (each a real failure paid for once),
with principles that hold on every platform: dereference symlinks when copying
a payload into a bundle (`cp -RL`), pin-or-log every upstream ref, prove the
bundle with the build-only/`dry_run` path before publishing, and verify
bundled resources exist in the copy step. When you fix a release/packaging
bug, **append a dated entry there** (symptom → cause → fix → applies-to) and
generalize it to the other targets — don't fix only the one that broke.

To *ship* something, or to work out why something didn't ship, start from
[`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md) — every button, when to
press it, when not to, and the three failures that have cost real time (no OTA
signing key; a flasher pinned to a release nobody cut; an app version that was
already published, where "publishing" silently overwrites instead of releasing).
The default is **Actions → "Update everything (only what needs it)"**, whose
decision engine is unit-tested in `.github/scripts/test_release_plan.py` and
driven by the catalog in `.github/release-targets.yml` — **add a new target
there, not in workflow YAML.**

Two things that are easy to half-fix, so check both sides:
- **Two flashers, two frontends.** `canary-local/assets/` (browser) and
  `desktop/src/` (desktop app) share no UI code — a user-facing diagnostic
  belongs in both.
- **Three version files per app.** `tauri.conf.json` (names the release tag),
  `package.json`, and `Cargo.toml` (what the app reports to the user).
  `desktop/scripts/check_app_versions.py` holds them together.

---

## Dependencies Policy

### Always allowed

* `anyhow`, `thiserror` — error handling
* `sha2`, `ed25519-dalek` — cryptography
* `serde`, `serde_json` — serialization
* `rusqlite` — local storage

### Feature-gated (audit required)

* `tract-onnx` — ONNX inference
* `ort` — ONNX Runtime (native)
* `gstreamer` — video ingestion

### Forbidden

* Anything that phones home
* Anything with face recognition APIs
* Anything with telemetry

---

## Commit Message Format

```
<type>(<scope>): <description>
```

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `chore`

Examples:

```
feat(detect): add BackendRegistry for runtime backend selection
fix(frame): prevent InferenceView from leaking raw bytes
docs(agents): clarify audit boundary vs security boundary
```

````

---

## Beacon Channel Invariants (v0.2 — must be enforced)

The Beacon channel (`spec/beacon_channel_v0.md`) is the project's
harm-reduction broadcast layer. It carries life-safety advisories with
smoke-detector-grade reliability requirements. Any agent modifying Beacon
firmware (`firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.{h,cpp}`)
or the Chirp v0.2 channel (`chirp_channel.{h,cpp}`) MUST observe:

1. **No automatic origination.** Beacons are originated only on explicit
   user action. Sensors (CSI, motion, audio) may prompt a human; sensors
   must not originate.
2. **Two-pubkey rule.** Every Beacon ALERT/UPDATE/CANCEL frame requires two
   distinct Ed25519 signatures from two distinct device pubkeys, both in
   the local beacon set with `trust_level != REVOKED`. Do not introduce a
   "trusted device skips co-sign" shortcut.

   **Solo-degraded exception (spec §6.2):** the single-device origination
   path is permitted ONLY when the physical BOOT button on the device is
   held at the moment of origination (real-time GPIO check), the frame
   carries `BCN_FLAG_SOLO_ORIGIN` in its header, and `certainty` is
   `Observed`. Receivers MUST verify all three invariants before
   accepting; the spec promises this property so the UI can downweight
   solo frames a notch. Do NOT add a software-only path that bypasses
   the BOOT GPIO check.
3. **No red, no reserved emergency-broadcast tones, no reserved phrasing.**
   See `spec/beacon_cap_gateway_v0.md` §4. The
   `scripts/lint_no_impersonation.sh` CI lint enforces this; do not bypass.
4. **No PII on the wire.** No descriptions of people, no license plates, no
   GPS, no MAC addresses, no `opera_id`, no household identifiers.
   Templates only.
5. **`scope = BCN_SCOPE_PRIVATE` always.** Never `Public`, never
   `Restricted`. Lint-enforced.
6. **No restoration of `TPL_AUTH_FEDERAL_PRESENCE`** or any
   "authority/government/federal presence" template in Chirp or Beacon.
7. **No persistence of Chirp session keys.** Session keys are ephemeral by
   design — that's the privacy firewall between Chirp and Opera.
8. **NVS writes of `opera_secret` and Beacon `beacon_set` require flash
   encryption.** Both code paths refuse on FE-off devices. Do not weaken.
9. **The Beacon audit log is append-only and chain-hashed.** Do not add a
   "rotate" or "delete" operation; export-only.
10. **Drills (`msgType = Exercise`) are wire-format-distinct from real
    alerts.** Do not merge their counters or rate-limit buckets.
11. **Tamper-flagged neighbors auto-revoke (v0.5).** When a paired
    Beacon-set member sends an Opera tamper alert (`MSG_TAMPER_ALERT`),
    `beacon_channel::on_peer_tampered(device_pubkey)` automatically
    transitions that entry to `trust_level = REVOKED`. Do not introduce
    a manual-only path or add a "trust them anyway, fail loud later"
    shortcut. Recovery is operational: physically inspect the device,
    then re-pair through the standard flow.
12. **Audible self-test is NFPA-72 §14 cadence (v0.5).** The
    `PATTERN_SELFTEST_OK` chirp plays monthly during waking hours
    (06:00–22:00 local, wall clock synced) only. Do not increase the
    cadence below monthly and do not lift the night-mode suppression —
    a 3 AM beep is a worse failure mode than a missing chirp.

Modifying any of the above requires updating
`docs/audit/mesh_and_chirp_audit_v1.md` and `docs/security/THREAT_MODEL.md` in the same
PR.
