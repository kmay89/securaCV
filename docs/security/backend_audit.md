# Detector backend audit

One row per backend module in the kernel's detector backends directory,
[`src/detect/backends/`](../../src/detect/backends/) (each is one file today;
a directory module would be named by its `mod.rs`): what each one does with a
frame, which crates it names and whether any of them builds native code, and
whether it is on in a default build.
It answers the "Backend audit trail" box of
[`firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`](../../firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md)
(section 5, sweep item F34, #1802).

Read at the tree of #1802 (2026-10-13; the native-code reading 2026-10-14). The rows are a source reading, not a
penetration test and not a bench run: nothing here was executed against a
camera, and the `tract-onnx` crate tree was read from `Cargo.lock`, not from
its source (the kernel's crates are not in any offline cache where this was
written).

## The gate

[`scripts/tests/test_backend_feature_gates.py`](../../scripts/tests/test_backend_feature_gates.py)
holds this table and the tree to each other. It reads `Cargo.toml` (every
build dependency, which of them are optional, and `[features]` expanded
transitively), `Cargo.lock` (each crate's locked tree, from the kernel's own
package down), and the backend modules `mod.rs` declares: each `mod name;`
resolves to `name.rs` or `name/mod.rs`, and the module is that file plus every
`.rs` under `name/`. It fails when:

1. a backend names an optional crate, or a crate whose locked tree builds
   native code (see below), and is not behind a `cfg(feature)`, on its `mod`
   item in `mod.rs` or as its root file's own `#![cfg(...)]`; or its gate's
   feature does not pull in an optional crate it names;
2. the default feature set enables a backend's gate feature, or any optional
   crate a backend needs (`Cargo.toml` declares no `default` today, so the
   default set is empty);
3. a backend has no row below, a row names a module that is not there, or a
   row disagrees with the module: its "Cargo feature" cell with the gate, its
   "Crates it names" cell with the build crates the module names (every one,
   in backticks, and no other), or its "Native code" cell says "none" for a
   module that names a native crate;
4. a `.rs` file under the directory belongs to no module `mod.rs` declares, or
   `mod.rs` declares a module with no file;
5. `mod.rs` itself names an optional or native crate.

It refuses what it cannot read rather than passing it: a gate of `any(...)`
over features or a bare `not(...)`, a feature name `Cargo.toml` does not
declare, a `#[path]` attribute or a `cfg_attr` on a module, an `include!`, an
inline `mod name { ... }` in `mod.rs`, a missing `Cargo.lock`, or a crate the
lock does not list. A crate counts as named when it appears before `::` in any
path, after `extern crate`, or anywhere in a `use` tree (`use x as y;` and a
bare `use x;` included), comments and string literals aside; a module that
shares a crate's name therefore reads as the crate, which fails closed.

**Decision (F34, #1802; maintainer to confirm):** the check is a static Python
test, not the `cargo test` the checklist box first asked for. The property
lives in the source text (an attribute on a `mod` line, a list in
`Cargo.toml`, the locked tree in `Cargo.lock`), `lint.yml` runs `scripts/tests`
on every pull request without a Rust toolchain, and the kernel's crates cannot
be fetched everywhere the check should run. Rejected: a `cargo test` (it would
read the same text through the compiler, but only where the whole crate graph
can be fetched, and the compile-side half is already `rust.yml`'s, which builds
the default feature set and `--features backend-tract`); and a grep in
`lint.yml` (it could not expand features transitively, so
`default = ["detect-eval"]` would pass it).

**Decision (F34, #1802; maintainer to confirm): what "non-pure-Rust" means.**
A backend is non-pure-Rust when a crate it names, optional or required, has in
its locked tree one of the build tools that compile or link native code:
`cc`, `cmake`, `bindgen`, `pkg-config` or `vcpkg`. Such a backend is gated like
one that needs an optional crate, even when the crate is in every build
already: a backend calling `rusqlite` (a required dependency, built with
bundled SQLCipher, whose tree holds `cc`) needs a feature gate that is off by
default, and its row must say what native code it carries. Rejected: reading
"non-pure-Rust" as "needs an optional crate" alone (the first version of this
gate did; a default backend on `rusqlite` passed it with a row saying it had no
native code); reading each crate's source language (no crate source is in an
offline cache here); and counting every `-sys` crate as native
(`linux-raw-sys` is pure Rust, and a `-sys` crate that does link native code
pulls in one of the tools above). What the lock reading cannot see: a crate
that ships prebuilt native code, or whose build script runs a compiler, with
none of those tools in its tree. The lock lists every platform's
dependencies, so a tool another platform pulls in counts here too, which fails
closed.

## The backends

| Backend file | What it is | Cargo feature | Crates it names | Native code | Reads, writes, network | Audit result |
|---|---|---|---|---|---|---|
| `src/detect/backends/cpu.rs` | `CpuBackend`, registry name `cpu`: the CPU-capability entry, motion only. Registered by `witnessd` and the demo in every build. | none (default build) | `anyhow` (required) | none in the file; it calls `motion.rs` | none: the frame is hashed in memory and dropped | Motion presence only, from `FrameHashMotion` (see `motion.rs`): reports no objects, `supports()` answers only `Motion`. `witnessd` logs at startup that this backend is motion-only. |
| `src/detect/backends/motion.rs` | `FrameHashMotion`, the shared engine behind `cpu` and `stub` (a private module, not a registry entry). | none (default build) | `sha2`, `anyhow` (both required) | none in the file | none: keeps the previous frame's 32-byte SHA-256 and nothing else | "Motion" is "this frame's bytes differ from the last frame's": any changed pixel, including sensor noise, reads as motion (confidence 0.85, size class Large). No pixels are retained, so nothing image-like outlives the call; the exported state is the 32-byte hash, and an import of any other length is refused. |
| `src/detect/backends/stub.rs` | `StubBackend`, registry name `stub`: the test placeholder, same engine as `cpu`. | none (default build) | `anyhow` (required) | none in the file; it calls `motion.rs` | none | Identical behavior to `cpu` by construction (one shared engine, so the two cannot drift); kept as a separate registry entry for tests and module descriptors that name it. |
| `src/detect/backends/tract.rs` | `TractBackend`, registry name `tract`: ONNX object detection on the host (`witnessd` on a Pi or x86 machine, never the ESP32). Selected by `detect.backend = "tract"`. | `backend-tract` (also enabled by `detect-eval`) | `tract-onnx` 0.23.8 (optional), `anyhow` (required) | through `tract-onnx`: `tract-linalg` 0.23.8 lists `cc` among its dependencies in `Cargo.lock`, the crate a build script uses to compile C or assembly, so treat it as carrying native kernels (its source was not read here) | reads one local model file when constructed (`model_for_path`); per frame, in-memory tensors only; no network and no disk writes in the file | Off unless built with the feature. Frames are size-checked before use (dimension product overflow refused, byte count must match), the confidence threshold must be in 0..=1, and a model whose output has an unexpected shape is an error, not a guess. The model is the one `scripts/fetch_detection_model.sh` downloads and checks against a pinned SHA-256, once, at fetch time: the backend does not re-hash the file when it loads it, so a model swapped on disk after the fetch is loaded as given. Built and tested by `rust.yml` (clippy and tests with `--features backend-tract`) and `detect-eval.yml`. |

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
