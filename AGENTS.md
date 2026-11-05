# AGENTS.md — the brief for any AI agent working in this repo

**This file is the canonical brief.** It is vendor-neutral on purpose: every
assistant that works here reads these same rules, through whichever filename
its tool looks for.

| Tool | File it reads |
|---|---|
| Codex, Cursor, Jules, Aider, Zed, Devin, most others | `AGENTS.md` — this file |
| Claude Code | [`CLAUDE.md`](CLAUDE.md) — imports this file and adds the load-by-need map |
| Gemini CLI, Qwen Code | [`GEMINI.md`](GEMINI.md), [`QWEN.md`](QWEN.md) — generated |
| GitHub Copilot | [`.github/copilot-instructions.md`](.github/copilot-instructions.md) — generated |
| Cursor (rules), Cline, Windsurf | [`.cursor/rules/securacv.mdc`](.cursor/rules/securacv.mdc), [`.clinerules`](.clinerules), [`.windsurfrules`](.windsurfrules) — generated |

The generated files are written from the block between the AGENT-BRIEF markers
below by `scripts/gen_agent_entrypoints.py` and byte-checked in CI. Edit this
file, run the generator; never edit a generated file by hand.

**The rules for one tree live in that tree**, in its own `AGENTS.md`, and load
only when you work there: [`src/`](src/AGENTS.md), [`firmware/`](firmware/AGENTS.md),
[`firmware/projects/canary-wap/`](firmware/projects/canary-wap/AGENTS.md),
[`firmware/projects/canary-display/`](firmware/projects/canary-display/AGENTS.md),
[`canary-local/`](canary-local/AGENTS.md),
[`docs/hardware/enclosure/`](docs/hardware/enclosure/AGENTS.md) and
[`.github/`](.github/AGENTS.md). What loads at session start is metered
(`scripts/agent_context_budget.py --check`, a CI gate): a rule that applies to
one tree goes in that tree's brief, not here.

## Read these first

| If you're doing this | Read |
|---|---|
| Anything at all | The non-negotiables below, then the brief of the tree you are changing |
| Answering a person's question about what something is | [`docs/GLOSSARY.md`](docs/GLOSSARY.md) — every proper noun defined once; [`docs/FAQ.md`](docs/FAQ.md) — the questions people ask. Code work does not need them |
| Looking for a doc | [`docs/README.md`](docs/README.md) — the CI-enforced map of every doc under `docs/` |
| Wondering which directory is the real one | [`docs/CONSOLIDATION.md`](docs/CONSOLIDATION.md) — the tree map |
| Writing code | [`docs/FLIGHT_RULES.md`](docs/FLIGHT_RULES.md) — the engineering constitution |
| Editing an input of a generated file | [`docs/GENERATED_FILES.md`](docs/GENERATED_FILES.md) — which edit moves which committed output, and the order |
| Shipping or releasing | [`.github/AGENTS.md`](.github/AGENTS.md), then [`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md) |

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
`can't`, which is why command transcripts are never retained, sealed, or
exported; the five-rule contract is `docs/research/whisper_local_voice.md`.
`ObjectClass` is `Person | Vehicle | Animal | Package` (plus `Unknown`, for a
detection the backend could not class) — never `Face` or `LicensePlate`. This
is Invariant II (`spec/invariants.md`) and it is a rejected PR, not a config
flag.

**2. Never widen the raw-frame escape hatch.** `RawFrame.data` stays private: no
public getter, no `Clone`, no `AsRef<[u8]>`. The only path to raw bytes is
`export_for_vault()` behind a `BreakGlassToken`, which needs n-of-m trustee
approval.

**3. Say "fleet," never "flock."** A group of Canaries is a **fleet**. "Flock" is
off-limits in user-facing copy, device UI strings, product and bundle names, code
identifiers, and comments — a company called Flock soured the word. The **only**
exception is the Unix `flock(2)` syscall, a real API name; do not rename it.
`scripts/lint_fleet_word.py` fails the build.

**3b. US spellings, always.** Write `color`, `center`, `behavior`, `analyze`,
`license`, `catalog` — never the British forms — in copy, device UI strings,
docs, comments and code identifiers alike. `scripts/lint_spelling.py` fails
the build; **the banned forms are enumerated once, in its regex, and never
repeated in prose** (a sweep once rewrote the rule that spelled them out into
its own opposite). Words that only *look* British to a substring match
(`analysis`, `parameter`, `characteristic`, …) are in its `ALLOW` list. Not
ours to respell: SPDX tags, third-party identifiers, quoted external text.

**4. Don't oversell, and don't overclaim.** "Verified" means *an Ed25519
signature checked against a pinned key* — nothing looser. No performance claim
without a benchmark, and no benchmark number committed to the tree
(`scripts/lint_bench_rows.py`). Describe `DetectorBackend` as an audit boundary
that must be audited, not as something that "cryptographically enforces"
anything. Where a guarantee isn't structural yet, say so out loud.

**5. Vocabulary changes start in the dictionary.** Event types, failure types,
attestation tiers, claim kinds and modalities are duplicated as constants across
Rust, Python, JS, firmware C++ and Swift. `spec/witness_dictionary.json` is the
single source of truth and `scripts/lint_dictionary_sync.py` fails CI on any
drift. Edit the dictionary first, then every copy the linter names.

**6. A new doc gets a home on the map in the same commit.**
`scripts/lint_docs_index.py` fails the build if a doc under `docs/` isn't
reachable from `docs/README.md`, or if a link there is dead.

**7. Two flashers, two frontends.** The in-browser flasher
(`canary-local/assets/`) and the desktop Flasher app (`desktop/src/`) share no UI
code. A user-facing diagnostic added to one must be added to the other, or half
the users keep the vague version.

**8. Beacon and Chirp have their own hard invariants** — two-pubkey co-signing,
no automatic origination, no PII on the wire, no impersonation of official
alerts, ephemeral session keys. They are stated in full in
`firmware/projects/canary-wap/AGENTS.md`; read it before touching beacon or
chirp code.

## Before you commit

- `cargo test`, `cargo clippy` (no warnings), `cargo doc` (no warnings), and
  `cargo test --no-default-features`, when `src/` moved.
- `python3 scripts/lint_docs_index.py` if you touched anything under `docs/`;
  `python3 scripts/lint_dictionary_sync.py` if you touched a vocabulary;
  `python3 scripts/gen_agent_entrypoints.py` if you edited this brief.
- Edited an input of a generated file (a `.scad`, a device manifest, a
  catalog JSON, the firmware version, a portal page)? `docs/GENERATED_FILES.md`
  names the generator and the order. For anything CAD,
  `python3 scripts/regen_cad.py --previews <dir>` runs the whole chain, and
  every `.scad` change ships PNG previews of every affected part to the
  requester (`docs/hardware/enclosure/AGENTS.md`).
- Touched an input of the firmware SBOM (`firmware/flavors.json`, a
  `platformio.ini`, a library manifest, a `sketch.yaml` pin, a core-version
  row, the firmware version)? `python3 scripts/gen_firmware_sbom.py`, commit
  `sbom/sbom-firmware.cdx.json`.
- Changed something the website carries (the build matrix, a kernel-status
  tile, `viewer/verify_core.js`, the fleet figures, a builder `.scad`)?
  `python3 scripts/carry_to_site.py --site <checkout>` or
  `docs/hardware/enclosure/gen_builder_manifest.py --site <checkout>`;
  `--check` says whether the carry is current.
- Commit format: `<type>(<scope>): <description>` — `feat`, `fix`, `docs`,
  `test`, `refactor`, `chore`.

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
| The business backend (orders, support inbox, books, status page, warranty, invoices) | `backoffice/` — one Node process, one SQLite file; `backoffice/README.md` is the operator's guide, `docs/strategy/31-…` the reasoning |
| Home Assistant integration | `custom_components/securacv/` + the app (add-on) wrapper `privacy_witness_kernel/` |
| Apple targets | `ios/` (iPhone/iPad), `tvos/` (the Witness Wall) |
| Architecture docs, not code | `kernel/` — docs only, despite the name |
| What a word means | `docs/GLOSSARY.md` |
| What a *thing* looks like | `canary-local/devices/figures.json` + `canary-local/figures/*.svg` — one isometric figure per device, part, board and tool, generated from the committed CAD. Spec: `docs/design/FLEET_FIGURES.md` |
| Whether something is real or still an idea | the same ledger's `confidence` — derived from evidence on disk (committed STLs · firmware config · released catalog variant), never hand-typed |
| One device, every fact joined — board, envs, emulator twin, figure, CAD, flasher product | `devices/<slug>/device.json` — one manifest per Canary, holding the ids the seven other files use and, as owner, the board knobs its case is cut around (`cad.params`, written into the `.scad` by `gen_cad_params.py`); `scripts/lint_device_manifests.py` proves every join. How-to: `devices/README.md` |
| Which tree is the real one | `docs/CONSOLIDATION.md` |
| Machine-readable vocabularies | `spec/witness_dictionary.json` |
| Which build has which feature | `firmware/build_matrix.json` (generated truth), not `firmware/FEATURES.md` (narrative, can lag) |
| Which products/envs ship | `firmware/flavors.json` |
| Board hardware-verification status | `firmware/boards/boards.json` — `verified` vs `compile-tested` |
| Release targets | `.github/release-targets.yml` — add a target there, not in workflow YAML |
| The firmware SBOM | `sbom/sbom-firmware.cdx.json`, generated from the build inputs by `scripts/gen_firmware_sbom.py` and committed; how it is derived: `sbom/README.md` |
| The broker-transport decision (plain / CA / pin / lab, fail-closed) | `firmware/common/network/mqtt_transport_logic.h` — one header every product applies; per-variant truth in `docs/FIRMWARE_VARIANT_AUDIT.md` |
| The fleet roll-call (`GET /api/fleet`) | `src/fleet_peers.rs` + `src/api/mod.rs` on the hub, `firmware/common/fleet_selfreport/` on a Canary WAP or Display; the contract is `tvos/discovery/DISCOVERY.md` |
| The hub plan (`--with …`) | `canary-local/devices/hub_seed.json`, the plan, generated by `canary-local/tools/gen_hub_seed.py`; `canary-local/tools/hub_seed_apply.py` executes it (`--dry-run` narrates a step) and `hub_host_provision.sh` is the host runner |
| The household-key checker (roster, challenges, card/phone/FIDO2 verifiers, the signed household posture statement, the LAN API) | `household/` — a standalone crate (`household-keys`, binary `household_checker`), never linked into witnessd; start with `household/README.md`. CI: `.github/workflows/household.yml` |
| The witness page contract (`GET /api/v1/witness`) | `spec/witness_api_v1.md`; its fixture `spec/fixtures/witness_page_v1.json` (written by `spec/fixtures/gen_witness_page_v1.py`, `--check` in CI) is byte-compared by a firmware host test and decoded by a Swift XCTest |

## CI gates you will trip

These run on every PR. Run the relevant one locally before you push.

| Gate | What it enforces |
|---|---|
| `scripts/lint_docs_index.py` | Every doc reachable from `docs/README.md`; no dead links |
| `scripts/lint_md_links.py` | Every relative link and `#anchor` in every tracked `.md` resolves — including the ~160 files outside `docs/` |
| `scripts/lint_fleet_word.py` | A group of Canaries is a *fleet* — the banned bird-group word fails the build (rule 3, now deterministic) |
| `scripts/lint_spelling.py` | US spellings everywhere (rule 3b); the banned forms live in its regex and nowhere else |
| `scripts/lint_no_binaries.py` | No compiled program, object or library is tracked (ELF, Mach-O, PE, `ar`). A hand build beside its source (`g++ test_x.cpp -o test_x` in a `tests_host/`) is the usual cause: build with the Makefile, which writes to `build/` |
| `scripts/lint_previews.py` | Every enclosure preview PNG the README, the docs or the catalogs embed exists, none on disk is unreferenced, and each has a `render.sh` recipe (CI renders with `--no-png`, so this is the only gate on them) |
| `scripts/lint_glossary_status.py` | The glossary's device-line Status column equals the verdict `figures.json` derives (or an honest non-ladder label); a new table row needs a `ROW_MAP` entry in the same commit |
| `scripts/lint_dictionary_sync.py` | Rust/Python/JS/firmware vocabularies match `spec/witness_dictionary.json` |
| `scripts/gen_agent_entrypoints.py --check` | Vendor agent files match this file's brief block |
| `scripts/lint_no_impersonation.sh` | No red, no reserved emergency tones, no official-alert phrasing |
| `scripts/lint_build_matrix.py` | `build_matrix.json` matches `platformio.ini` + `canary_config.h`; every product lane resolves to one `devices/<slug>/device.json`, whose envs are real |
| `scripts/lint_device_manifests.py` | Every `devices/<slug>/device.json` validates and its env / board / figure / flasher / emulator / CAD joins hold; every `flavors.json` build env is claimed or explained. Since wave 2 the generators read the manifests (`gen_flash.py`, `gen_figures.mjs`), so a stale join also moves a generated file; since wave 3 it also runs the `cad.params` check below |
| `docs/hardware/enclosure/gen_cad_params.py --check` | Every manifest-owned CAD knob equals the literal in its case `.scad` (a registry reference resolved first) — in `lint.yml` for a manifest edit and in `enclosure.yml` for a `.scad` edit that bypassed the manifest; `scripts/regen_cad.py --check` runs it first and every generator downstream of it after |
| `scripts/lint_feature_flags.sh` | Feature-flag hygiene: every Cargo feature referenced and registered, no `FUTURE_*` transport or tamper type advertised, and every firmware `FEATURE_*` flag read by code (it runs `firmware/scripts/check_config_feature_flags.py`) |
| `scripts/lint_version_sync.sh`, `desktop/scripts/check_app_versions.py` | One version per app across `tauri.conf.json` / `package.json` / `Cargo.toml` |
| `scripts/lint_bom.py` | BOM CSVs schema-clean and wired to the generator |
| `scripts/lint_bench_rows.py` | No benchmark table the harness prints is committed to any Markdown file (rule 4; `docs/BENCHMARKS.md`) |
| `scripts/gen_firmware_sbom.py --check --validate` | The committed firmware SBOM matches the build inputs, validates against CycloneDX 1.5, and the workflow core rows agree with the `sketch.yaml` profiles |
| `scripts/lint_cloudkit_container.py` | No `CKContainer.default()`; the container identifier matches both entitlements files |

Full list: [`.github/workflows/lint.yml`](.github/workflows/lint.yml) and
[`docs/ci.md`](docs/ci.md).

