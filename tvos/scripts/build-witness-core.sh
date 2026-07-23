#!/usr/bin/env bash
# Build the shared SecuraCV witness core for Apple TV (aarch64-apple-tvos).
#
# The chain-verification logic is the workspace's existing Rust verifier — this
# just compiles it for the TV so the SwiftUI app can call it over FFI. tvOS is a
# tier-3 Rust target, so we build the std source alongside it.
#
# Honest stub: the `tvos/witness-core/` crate and the `WitnessWall/` app do not
# exist yet (see tvos/README.md — "design + pipeline scaffold, not yet built").
# Fail loudly and specifically rather than pretending to succeed.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f witness-core/Cargo.toml ]; then
  echo "::error::tvos/witness-core/ does not exist yet."
  echo "The Witness Wall is still design-stage. Scaffold the crate that wraps the"
  echo "shared verifier for aarch64-apple-tvos, then this script builds it. See"
  echo "tvos/README.md (Intended layout) and docs/tvos/AUTOPIPELINE.md."
  exit 1
fi

echo "Building witness-core for aarch64-apple-tvos…"
cargo +nightly build \
  --release \
  --manifest-path witness-core/Cargo.toml \
  --target aarch64-apple-tvos \
  -Z build-std=std,panic_abort
echo "Done: witness-core/target/aarch64-apple-tvos/release/"
