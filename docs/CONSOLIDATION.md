# Consolidation & tree map

This monorepo has several **similarly-named trees**. Most are not duplicates —
they serve different purposes — but the names make a newcomer waste ten minutes
figuring out which one is "real." This file is the map, plus a short list of
genuine cleanup decisions still open. Keep it current; it's cheap insurance
against the sprawl becoming a maintenance nightmare.

## What each tree actually is

| Tree | Files | What it is | Duplicate? |
| --- | --- | --- | --- |
| `src/` | 111 | **The witness kernel** — the real Rust daemon (`witnessd`) and its binaries (`src/bin/`; the exact roster is the `[[bin]]` entries in `Cargo.toml`). This is the product. | No — canonical |
| `kernel/` | 2 | **Docs only** — `architecture.md` etc., the constitutional reference CONTRIBUTING points at. Not code. | No — rename candidate (see below) |
| `privacy_witness_kernel/` | 12 | **The Home Assistant add-on** — packaging wrapper that ships the kernel as a HACS/add-on. Not a second kernel. | No |
| `desktop/` | 92 | **Hub & flasher** — the Tauri Flasher app and the `hub-io`/`hub-core` Rust crates (provisioning, flashing, hub image writing). | No |
| `desktop-lab/` | 30 | **The Lab desktop app** — a Tauri shell around `canary-local` (the browser Lab), for Mac/Linux. Different app, different job. | No |
| `firmware/canary/` | 141 | **Active firmware** (PlatformIO, `seeed_xiao_esp32s3`) and the shared `lib/` the other trees pull from. | No — canonical |
| `firmware/projects/canary-wap/` | 267 | **The WAP variant** — Arduino compatibility lane **plus** a PlatformIO lane that FEATURES.md flags as a skeleton. Carries sketch copies of `common/csi`, mesh and signature code, kept byte-identical by the sync checks in `firmware/scripts/`. | Partial — see decision 1 |
| `firmware/projects/canary-display/` | 423 | **Canary Display** — the wall displays and dashes (the Nightstand Line, the Nightlight, the watch face); the most actively released product and the source the WASM emulator compiles. | No |
| `firmware/projects/canary-vision/` | 52 | **Canary Vision** — camera + on-device person detection, reports to Home Assistant. Specialized lane per the variant audit. | No |
| `firmware/projects/canary-sense/` | 34 | **Canary Sense** — presence and breathing radar (60 GHz MR60BHA2), no camera. | No |
| `firmware/projects/canary-ota/` | 16 | **The retired standalone OTA harness** (ESP-IDF). Signed updates ship from `firmware/common/ota/` now; this tree is kept for the tooling under `tools/`. | Superseded — see README OTA row |
| `firmware/projects/canary-tincan/` | 14 | **Canary Tincan** — the two-can acoustic-link experiment; host-tested core, Arduino sketch. | No |
| `firmware/projects/canary-companion/` | 9 | **Canary Companion** — host-tested headers for the companion-device contract; no board build. | No |
| `firmware/projects/canary-sentinel/` | 7 | **Canary Sentinel** — *Phase 0*: multi-sensor fusion core, host-tested, no released build. | No |
| `firmware/projects/canary-fence-guard/` | 3 | **Canary Fence Guard** — *concept*; nothing builds yet. | No |

File counts are `git ls-files <tree> | wc -l` on 2026-09-02; they will drift,
the rows will not.

**Rule of thumb for newcomers:** the product is `src/` (kernel) + `firmware/canary/`
(device). Everything else is a wrapper, a variant, or docs.

## Open decisions

1. **`canary-wap` PIO lane (~40% skeleton).** `firmware/FEATURES.md` already
   asks whether to retire it. Decide: promote it to parity, or drop the PIO
   lane and keep the Arduino compatibility lane. Leaving a half-built lane in
   the tree is the single biggest "which one do I touch?" trap here.
2. **`kernel/` → rename.** It's docs, not a kernel, but sits next to `src/`
   (the actual kernel) and `privacy_witness_kernel/` (the add-on). Renaming to
   `docs/kernel/` (or folding into `docs/`) would remove a real ambiguity —
   but CONTRIBUTING and CODEOWNERS reference `kernel/`, so this is a
   coordinated move, not a drive-by. Tracked, not yet done.
3. **`desktop/` vs `desktop-lab/` naming.** Both are legit apps, but the names
   suggest one is a variant of the other. Consider `desktop-hub/` +
   `desktop-lab/` so the pairing is obvious.

## Done (2026-07)

- **Committed build artifacts removed.** `desktop/hub-io/target/` (1,512
  tracked files) was checked in because `.gitignore` only ignored the
  *root* `/target/`. Fixed by adding `**/target/` and `git rm --cached`-ing the
  tree. Nested cargo crates no longer leak build output.
- **Naming drift fixed.** `CONTRIBUTING.md` referenced
  `custom_components/secura_cv/` (underscore) — a release-step bug, since the
  real directory is `custom_components/securacv/`. Corrected. The CONTRIBUTING
  title now names SecuraCV *and* the witness kernel, so the two aren't read as
  drift.

## Still drifting (not yet addressed)

- `AGENTS.md` opens "SecuraCV witness-kernel: …" — fine as a component name,
  but worth a consistency pass so "SecuraCV" (product) vs "witness kernel"
  (component) vs "Canary" (device) are used deliberately everywhere. See
  [`docs/BRAND.md`](BRAND.md) for the intended relationship.
