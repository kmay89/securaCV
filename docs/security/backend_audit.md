# Detector backend audit

One row per file in the kernel's detector backends directory,
[`src/detect/backends/`](../../src/detect/backends/): what each one does with a
frame, what it pulls into the build, and whether it is on in a default build.
It answers the "Backend audit trail" box of
[`firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`](../../firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md)
(section 5, sweep item F34, #1802).

Read at the tree of #1802 (2026-10-13). The rows are a source reading, not a
penetration test and not a bench run: nothing here was executed against a
camera, and the `tract-onnx` crate tree was read from `Cargo.lock`, not from
its source (the kernel's crates are not in any offline cache where this was
written).

## The gate

[`scripts/tests/test_backend_feature_gates.py`](../../scripts/tests/test_backend_feature_gates.py)
holds this table and the tree to each other. It reads `Cargo.toml` (every
optional dependency, and `[features]` expanded transitively) and every
`src/detect/backends/*.rs`, and it fails when:

1. a backend file names an optional crate and is not behind a `cfg(feature)`,
   on its `mod` line in `mod.rs` or as the file's own `#![cfg(...)]`, whose
   feature pulls that crate in;
2. the default feature set enables a backend's gate feature, or any optional
   crate a backend needs (`Cargo.toml` declares no `default` today, so the
   default set is empty);
3. a backend file has no row below, a row names a file that is not there, or a
   row's "Cargo feature" cell disagrees with the file's gate;
4. a file in the directory is not declared in `mod.rs`.

It refuses a gate it cannot read (`any(...)` over features, a bare `not(...)`,
a feature name `Cargo.toml` does not declare) rather than passing it.

**Decision (F34, #1802; maintainer to confirm):** the check is a static Python
test, not the `cargo test` the checklist box first asked for. The property
lives in the source text (an attribute on a `mod` line, a list in
`Cargo.toml`), `lint.yml` runs `scripts/tests` on every pull request without a
Rust toolchain, and the kernel's crates cannot be fetched everywhere the check
should run. Rejected: a `cargo test` (it would read the same text through the
compiler, but only where the whole crate graph can be fetched, and the
compile-side half is already `rust.yml`'s, which builds the default feature set
and `--features backend-tract`); and a grep in `lint.yml` (it could not expand
features transitively, so `default = ["detect-eval"]` would pass it).

"Non-pure-Rust" in the checklist is read here as "needs an optional crate":
every backend that adds a dependency to the build is behind a feature, whatever
language the dependency is written in. The default backends add none.

## The backends

| Backend file | What it is | Cargo feature | Optional crates | Native code | Reads, writes, network | Audit result |
|---|---|---|---|---|---|---|
| `src/detect/backends/cpu.rs` | `CpuBackend`, registry name `cpu`: the CPU-capability entry, motion only. Registered by `witnessd` and the demo in every build. | none (default build) | none | none in the file; it calls `motion.rs` | none: the frame is hashed in memory and dropped | Motion presence only, from `FrameHashMotion` (see `motion.rs`): reports no objects, `supports()` answers only `Motion`. `witnessd` logs at startup that this backend is motion-only. |
| `src/detect/backends/motion.rs` | `FrameHashMotion`, the shared engine behind `cpu` and `stub` (a private module, not a registry entry). | none (default build) | none (`sha2` and `anyhow` are required dependencies) | none in the file | none: keeps the previous frame's 32-byte SHA-256 and nothing else | "Motion" is "this frame's bytes differ from the last frame's": any changed pixel, including sensor noise, reads as motion (confidence 0.85, size class Large). No pixels are retained, so nothing image-like outlives the call; the exported state is the 32-byte hash, and an import of any other length is refused. |
| `src/detect/backends/stub.rs` | `StubBackend`, registry name `stub`: the test placeholder, same engine as `cpu`. | none (default build) | none | none in the file; it calls `motion.rs` | none | Identical behavior to `cpu` by construction (one shared engine, so the two cannot drift); kept as a separate registry entry for tests and module descriptors that name it. |
| `src/detect/backends/tract.rs` | `TractBackend`, registry name `tract`: ONNX object detection on the host (`witnessd` on a Pi or x86 machine, never the ESP32). Selected by `detect.backend = "tract"`. | `backend-tract` (also enabled by `detect-eval`) | `tract-onnx` 0.23.8 | through `tract-onnx`: `tract-linalg` 0.23.8 lists `cc` among its dependencies in `Cargo.lock`, the crate a build script uses to compile C or assembly, so treat it as carrying native kernels (its source was not read here) | reads one local model file when constructed (`model_for_path`); per frame, in-memory tensors only; no network and no disk writes in the file | Off unless built with the feature. Frames are size-checked before use (dimension product overflow refused, byte count must match), the confidence threshold must be in 0..=1, and a model whose output has an unexpected shape is an error, not a guess. The model is the one `scripts/fetch_detection_model.sh` downloads and checks against a pinned SHA-256, once, at fetch time: the backend does not re-hash the file when it loads it, so a model swapped on disk after the fetch is loaded as given. Built and tested by `rust.yml` (clippy and tests with `--features backend-tract`) and `detect-eval.yml`. |

## What this does not cover

- The crates under `tract-onnx` (`tract-core`, `tract-linalg`, `tract-hir`,
  `prost`, `memmap2` and the rest): their versions are pinned by `Cargo.lock`
  and they go through `cargo audit` in CI, but nobody has read their source
  for this table.
- Model provenance past the fetch script's hash: which training data, what
  the classes mean for a privacy witness, and whether a load-time hash check
  belongs in `witnessd` are open questions, not answered here.
- The detection modules outside this directory (`src/detect/yolo.rs`'s decode
  and NMS, the backend registry, the module runtime's capability checks): they
  are shared by every backend and belong to the kernel's own review, not to
  a per-backend row.
