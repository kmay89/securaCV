<!-- GENERATED FILE — DO NOT EDIT.
     Source: AGENTS.md (the block between its AGENT-BRIEF markers).
     Regenerate: python3 scripts/gen_agent_entrypoints.py
     CI fails if this file drifts from AGENTS.md. -->

# QWEN.md — SecuraCV agent brief

The rules below are shared by every AI assistant working in this
repository; Qwen Code reads them from this file.

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

---

## The rest of the brief

This file carries the non-negotiables only. The full brief — the repo map, the
"where do I look for X" index, the CI gates, the detection-backend audit rules,
the code style, and the Beacon/Chirp channel invariants — lives in
[`AGENTS.md`](AGENTS.md). Read it before any non-trivial change.

Also worth having open:

- [`docs/GLOSSARY.md`](docs/GLOSSARY.md) — every proper noun in the project,
  defined once. Read this before answering a question about what something is.
- [`docs/FAQ.md`](docs/FAQ.md) — the questions users actually ask, answered.
- [`docs/README.md`](docs/README.md) — the CI-enforced map of every doc.
- [`docs/CONSOLIDATION.md`](docs/CONSOLIDATION.md) — which similarly-named
  directory is the real one.
- [`docs/FLIGHT_RULES.md`](docs/FLIGHT_RULES.md) — the engineering
  constitution.
