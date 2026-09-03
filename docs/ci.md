# CI validation

This repository favors invariant-preserving checks that validate buildability
without expanding data access paths.

## What runs — the gate families

There are ~50 workflows under [`.github/workflows/`](../.github/workflows/).
This page indexes them by family and details one (container builds) below;
the per-gate detail for the rest lives next to each gate.

| Family | Workflows | What it gates |
|---|---|---|
| **Lint gates** | [`lint.yml`](../.github/workflows/lint.yml) | ~20 scripted checks: docs-map reachability (`scripts/lint_docs_index.py`), US spelling (`scripts/lint_spelling.py`), dictionary sync (`scripts/lint_dictionary_sync.py`), agent-entrypoint drift (`scripts/gen_agent_entrypoints.py --check`), Apple Home doc drift (`scripts/gen_apple_home_docs.py --check`), no-impersonation, feature-flag hygiene, build-matrix sync, app-version sync, BOM schema, and more. The most-tripped ones are tabled in [`AGENTS.md`](../AGENTS.md) ("CI gates you will trip"); the workflow file is the full list. |
| **Kernel (Rust)** | `rust.yml`, `fuzz.yml`, `codeql.yml`, `sbom.yml` | `cargo test`/`clippy`/`doc` across feature combinations, fuzz targets, static analysis, supply-chain SBOM. |
| **Firmware** | `firmware.yml`, `csi_module_disable_matrix.yml`, `ram_audit.yml` | Every flavor/env builds; the CSI module-disable matrix; RAM budget. |
| **Lab / emulator drift** | `canary-local.yml`, `emulator-dist-refresh.yml` | Firmware → WASM boots in a browser; the "Dist drift check" byte-compares the committed `canary-local/emulator/dist/` bundles, and the page-logic tests catch a stale `fw_version` stamp. The full stale-dist playbook is in [`CLAUDE.md`](../CLAUDE.md) ("Generated files"). |
| **Container images** | `container-images.yml`, `docker-sidecar.yml`, `addon-image.yml` | Detailed in the next section. |
| **Apps & release** | `desktop-*.yml`, `ios-*.yml`, `tvos*.yml`, `mac-apps-release.yml`, `flasher-release.yml`, `firmware-release*.yml`, `release-*.yml` | The build/publish pipelines. Operator's guide: [`RELEASE_BUTTONS.md`](RELEASE_BUTTONS.md); lessons: [`.github/RELEASE_LESSONS.md`](../.github/RELEASE_LESSONS.md); targets are declared in `.github/release-targets.yml`, not in workflow YAML. |
| **Freshness & guards** | `board-facts-freshness.yml`, `homeassistant-freshness.yml`, `features-dashboard-guard.yml`, `release-latest-guard.yml`, `workflows-lint.yml` | Scheduled and structural checks that fail when generated or pinned facts drift from their sources. |

## Container build validation

Container images are built and smoke-tested in CI — a green build is required
before changes to an image or the crate it packages can merge:

- **`witnessd` image** (root `Dockerfile`):
  `.github/workflows/container-images.yml` builds the image on every PR/push
  touching it and asserts every shipped binary loads with no missing shared
  libraries and that the healthcheck's `curl` is present.
- **Frigate sidecar** (`docker/sidecar/Dockerfile`):
  `.github/workflows/docker-sidecar.yml` lints the scripts, validates the
  compose files, runs a full end-to-end test against a live broker, and
  publishes to GHCR on main/tags.
- **Home Assistant add-on** (`privacy_witness_kernel/Dockerfile`):
  `.github/workflows/addon-image.yml` builds amd64 (+ aarch64 on main/tags,
  each natively on a runner of its own architecture — no QEMU), publishes to
  GHCR, and verifies every declared arch is anonymously pullable. The verify
  gate cross-probes with the workflow's own token so it can say *which* way
  an arch is uninstallable: a package nobody made public (exit 3), a tag the
  build never pushed (exit 4), or undetermined (exit 1).

To reproduce the witnessd build locally:

```bash
docker build --build-arg CARGO_FEATURES=rtsp-gstreamer -t witnessd:ci .
```

This confirms the deployable artifact can be built with RTSP support while
keeping the runtime surface limited to the Event API.
