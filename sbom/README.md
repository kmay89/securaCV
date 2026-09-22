# SBOM Generation

This directory holds the Software Bill of Materials (SBOM) story for SecuraCV:
how each document is produced, and — for the firmware — the committed document
itself. The output format is **CycloneDX 1.5 JSON**.

## The three documents

| Ecosystem | File | How it is produced |
|-----------|------|--------------------|
| Rust (kernel) | `sbom-rust.cdx.json` | `cargo-cyclonedx`, in `.github/workflows/sbom.yml` |
| Node.js (device-api, SPA) | `sbom-node.cdx.json` | `@cyclonedx/cdxgen`, in `sbom.yml` |
| C/C++ (ESP32 firmware) | [`sbom-firmware.cdx.json`](sbom-firmware.cdx.json) | **generated and committed** by [`scripts/gen_firmware_sbom.py`](../scripts/gen_firmware_sbom.py); byte-gated on every PR |

The Rust and Node documents are produced fresh by `sbom.yml` on every push to
`main` that touches a dependency manifest, on pull requests, and on every
published release (where all three are attached as release assets). They are
not committed.

## The firmware SBOM

### Why it is generated, and why it is committed

The firmware document used to be a template typed into the workflow: it named
Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7 and a hand-picked set of IDF components,
and it kept saying so after the shipped `canary-display` and `canary-wap`
images had moved to the pioarduino core 3.3.8 / ESP-IDF 5.5.4. A bill of
materials that cannot move with the build is a false compliance record with a
timestamp on it.

`scripts/gen_firmware_sbom.py` derives the document from the files the build
reads, and the result is committed here so that a change to any input shows up
as a reviewable diff — and so that `python3 scripts/gen_firmware_sbom.py
--check` (run by `lint.yml` on every PR) can refuse an input that moved without
the document. `sbom.yml` additionally proves the generator's PlatformIO-ini
resolver against PlatformIO's own `pio project config` for every build env
(`--verify-with-pio`) before it re-derives the document for the artifact.

### What is derived

| From | What the document takes |
|------|-------------------------|
| `firmware/flavors.json` | the products (`canary`, `canary-wap`, `canary-vision`, `canary-display`, `canary-sense`), their project dir, the envs CI builds and the subset that ships |
| each project's `platformio.ini` + its `extra_configs` | the **resolved** config of every build env: `platform`, `framework`, `board`, `lib_deps`, `lib_extra_dirs` — through `extends` chains and `${section.option}` interpolation, the same way PlatformIO reads them |
| `firmware/envs/platformio/platforms.ini` | which pin section each env's platform literal is ([`firmware/PLATFORMS.md`](../firmware/PLATFORMS.md)) |
| `firmware/common/*/library.json` / `library.properties`, `firmware/canary/lib/*/library.json` | the first-party libraries (name, version, license) the images are built from |
| `.github/workflows/*.yml` | every `esp32:esp32` core version the Arduino-CLI build path pins through `.github/actions/setup-arduino-esp32` (a literal, a matrix axis, or the action's "latest"), with each row's library pins and the sketch it goes on to compile |
| `firmware/projects/*/arduino/*/sketch.yaml` | the other Arduino axis: the core (`esp32:esp32 (X.Y.Z)`) and library pins of every sketch profile, parsed from YAML — the files' comments quote core numbers too |
| `firmware/canary/include/canary_config.h` | `FIRMWARE_VERSION` — the one release train (`scripts/lint_fw_version_sync.sh` holds the other five copies to it) |

Each product is a `firmware` component; each PlatformIO platform package a
`platform` component; each Arduino core and ESP-IDF release a `framework`
component; every `lib_deps` entry and every first-party library a `library`
component. The `dependencies` graph joins them: product → platform → Arduino
core → ESP-IDF, product → libraries. Which envs a platform or library serves is
in each component's `securacv:envs` property; which build paths reach an
Arduino core — PlatformIO platform sections, workflow rows, sketch profiles —
is in the `framework` component's `securacv:build_paths`.

### The two Arduino axes agree, or nothing generates

The Arduino-CLI path pins its `esp32:esp32` core twice: in the workflows
(the composite action's `core-version` input) and in each sketch's
`sketch.yaml` profiles. `firmware.yml` used to ask, in a comment, that the two
be kept in lockstep. The generator now asserts it, and a disagreement fails
generation — so `lint.yml`'s `--check` goes red on the PR that introduces
it — naming the row, the profile and the remedy:

1. every exact core-version a workflow row pins is pinned by a sketch
   profile of a product that row builds;
2. every core a sketch profile pins is a pin of a workflow row that builds
   that product, **or** the Arduino core inside that product's PlatformIO
   platform (`PLATFORM_FACTS`) — a sketch may track either build path (both
   rules are scoped per product: one product's profile never answers for
   another's row);
3. every library a workflow row pins on its core line (GFX, lvgl and NimBLE
   split their majors along the core boundary) is pinned to the same version
   by every profile, of a product that row builds, on that core. Which product
   a row builds is read off the job's `run:` blocks (the `.ino` it compiles,
   or the sketch dir it `cd`s into) — the release jobs install the WAP's core
   and both display cores in one job.

Not asserted, on purpose: a workflow row on the action's "latest" has no
version to agree with, and a sketch may pin libraries its row leaves floating.
That leaves one recorded residual, not a drift: every WAP workflow row builds
on "latest" while the WAP sketch pins 3.3.8 — the core inside
`platform_core3`, the PlatformIO path the sketch tracks. Rule 2 is what makes
that legal; pinning those rows would be a release decision
([`firmware/PLATFORMS.md`](../firmware/PLATFORMS.md)), not a lint's.

### What is still declared by hand

Two facts are not in any file of ours, and are typed into the generator once,
where a reviewer can see them:

- **`PLATFORM_FACTS`** — for each platform literal in `platforms.ini`, the
  Arduino core and ESP-IDF release that platform package bundles (numbers from
  `firmware/PLATFORMS.md`). It is keyed by the exact literal, so a pin bump
  fails generation with "no PLATFORM_FACTS for …" instead of carrying the old
  numbers forward. Whether a literal is an exact pin or a floating spec is in
  the `platform` component's `securacv:exact_pin` property; a floating spec
  has no `version`, only `securacv:version_spec`, and its core is what that
  line ships today, not a promise. Since 2026-09-22 every section pins
  exactly (canary's `espressif32 @ ^7.0.0` was the last float —
  `firmware/PLATFORMS.md`), so the document carries no such component today.
- **The two framework licenses** (LGPL-2.1-or-later for arduino-esp32,
  Apache-2.0 for ESP-IDF).

And two things are deliberately **not** claimed:

- **The libraries bundled inside ESP-IDF** (FreeRTOS, mbedTLS, lwIP, cJSON).
  Their versions are fixed by the IDF release, which nothing in this tree pins
  independently; the old template's numbers for them were a guess dressed as a
  record. The `esp-idf` component says so in `securacv:bundled_components`.
- **Resolved library versions.** PlatformIO keeps no lockfile. A `lib_deps`
  entry with an exact spec (`GFX Library for Arduino@1.6.6`) gets a `version`;
  a range (`ArduinoJson@^7.0.0`) gets only `securacv:version_spec` and
  `securacv:resolved: at build time`. The version a given build linked is in
  that build's log, not here.

### Determinism

Components, dependencies and properties are sorted; the `serialNumber` is a
UUIDv5 of the content; there is **no timestamp** in the committed file. The
artifact `sbom.yml` uploads (and attaches to releases) is the same document
with one added field, `metadata.timestamp`, the generation instant — passed
as `--timestamp now`, the only non-deterministic thing the generator will emit
and never into the committed copy.

### Regenerating

```bash
python3 scripts/gen_firmware_sbom.py                       # rewrites sbom/sbom-firmware.cdx.json
python3 scripts/gen_firmware_sbom.py --check --validate    # what lint.yml runs
python3 scripts/gen_firmware_sbom.py --validate FILE       # an artifact's bytes (sbom.yml)
python3 scripts/gen_firmware_sbom.py --verify-with-pio     # needs `pio` on PATH
```

Run the first after changing a platform pin, a `lib_deps` line, a library
manifest, an Arduino core pin in a workflow, a `sketch.yaml` profile, or the
firmware version, and commit the result in the same change. `--validate`
needs `pip install 'cyclonedx-python-lib[json-validation]==11.12.0'` (the
generator names that spec when the import fails, and the unit tests hold
every workflow's pip line and this page to that one string); it is read-only,
like `--check`, and says so: a `--out` or `--timestamp` beside it without
`--check` is refused up front rather than dropped for a green exit and no
file — to validate an artifact, write it, then run `--validate FILE` on it.
Tests: `scripts/tests/test_gen_firmware_sbom.py`.

## Retrieving the SBOMs

All three are attached as build artifacts on each `sbom.yml` run, and as
release assets on every published release:

1. Go to the **Actions** tab in the repository
2. Select the **SBOM Generation** workflow
3. Click the most recent run
4. Download the `sbom-artifacts` artifact

## Verification

To verify a CycloneDX SBOM:

```bash
# Install the CycloneDX CLI
npm install -g @cyclonedx/cyclonedx-cli

# Validate
cyclonedx validate --input-file sbom-rust.cdx.json --input-format json
```

The firmware document is held to the CycloneDX 1.5 JSON schema in **strict
mode** (`cyclonedx-python-lib`'s `JsonStrictValidator`: no unknown keys,
format checking on) as a CI gate, twice: `lint.yml` runs
`python3 scripts/gen_firmware_sbom.py --check --validate` on the committed
copy on every PR, and `sbom.yml` runs `--validate sbom-firmware.cdx.json` on
the timestamped artifact — the exact bytes it uploads and attaches to a
release — before its `jq` step checks what a schema cannot (a non-empty
component list, a closed dependency graph). The `specVersion` is asserted
by the generator, not the schema: the 1.5 schema types that field as a free
string, so a file declaring 1.6 would otherwise pass as "a strict 1.5
document"; `--validate FILE` refuses it, and reports a missing or non-JSON
file as one error line. That strict check is how a
registry URL with spaces in it was caught before the first commit, by hand;
the `json-validation` extra is what brings the IRI format checker, which is
why the pin is exact and the extra is not optional. The schema ships inside
the wheel and `$ref`s resolve from a local registry, so the gate needs no
network. The Rust and Node documents are not held to 1.5: `cargo-cyclonedx`
and `cdxgen` choose their own `specVersion`.
