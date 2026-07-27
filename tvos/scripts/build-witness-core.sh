#!/usr/bin/env bash
# Build the shared SecuraCV witness core for Apple TV, and stage it where Xcode
# links it from.
#
# tvOS is a tier-3 Rust target, so the standard library is built from source
# (`-Z build-std`), which needs nightly + rust-src. Two slices are produced:
#
#   aarch64-apple-tvos       → tvos/build/lib/appletvos/          (a real Apple TV)
#   aarch64-apple-tvos-sim   → tvos/build/lib/appletvsimulator/   (PR CI, no signing)
#
# The directory names are Xcode's $(PLATFORM_NAME) values, which is what lets
# ONE LIBRARY_SEARCH_PATHS setting in WitnessWall/project.yml serve both the
# device archive and the simulator test run.
#
# Release lessons this file is written against (.github/RELEASE_LESSONS.md):
#   P1 copy payloads with `cp -RL` — dereference, never stage a symlink
#   P2 pin or LOG every upstream ref — print the resolved toolchain + target
#   P4 verify a staged artifact exists IN THE COPY STEP, so the failure is a
#      clear line here and not an opaque linker error minutes later
#
# Usage:
#   bash scripts/build-witness-core.sh              # device + simulator
#   TVOS_SLICES=simulator bash scripts/…            # simulator only (PR CI)
#   TVOS_SLICES=device    bash scripts/…            # device only (release)
set -euo pipefail

cd "$(dirname "$0")/.."
CORE_DIR="witness-core"
STAGE_ROOT="build/lib"
LIB_NAME="libsecuracv_witness_core.a"

if [ ! -f "$CORE_DIR/Cargo.toml" ]; then
  echo "::error::tvos/witness-core/ is missing — the Witness Wall cannot be built without its verification core."
  exit 1
fi

# ── P2: log the toolchain instead of trusting it ─────────────────────────────
# A tier-3 target silently built by a different nightly is the kind of thing
# that breaks a release with no change on our side. Print what we actually got.
echo "── toolchain ──"
if ! command -v cargo >/dev/null 2>&1; then
  echo "::error::cargo not found. Install Rust (rustup) — see tvos/README.md."
  exit 1
fi
if ! cargo +nightly --version >/dev/null 2>&1; then
  echo "::error::the nightly toolchain is not installed."
  echo "tvOS is a tier-3 Rust target and needs -Z build-std:"
  echo "    rustup toolchain install nightly"
  echo "    rustup component add rust-src --toolchain nightly"
  exit 1
fi
cargo +nightly --version
rustc +nightly --version
# rust-src is what -Z build-std compiles the standard library from; without it
# the build fails deep inside cargo with a much less obvious message.
if ! rustc +nightly --print sysroot >/dev/null 2>&1 ||
   [ ! -d "$(rustc +nightly --print sysroot)/lib/rustlib/src/rust" ]; then
  echo "::error::the rust-src component is missing from the nightly toolchain."
  echo "    rustup component add rust-src --toolchain nightly"
  exit 1
fi

case "${TVOS_SLICES:-both}" in
  both)      slices=("aarch64-apple-tvos:appletvos" "aarch64-apple-tvos-sim:appletvsimulator") ;;
  device)    slices=("aarch64-apple-tvos:appletvos") ;;
  simulator) slices=("aarch64-apple-tvos-sim:appletvsimulator") ;;
  *)
    echo "::error::TVOS_SLICES must be both, device, or simulator (got '${TVOS_SLICES}')."
    exit 1
    ;;
esac

for slice in "${slices[@]}"; do
  target="${slice%%:*}"
  platform="${slice##*:}"

  echo "── building $target → $platform ──"
  cargo +nightly build \
    --release \
    --manifest-path "$CORE_DIR/Cargo.toml" \
    --target "$target" \
    -Z build-std=std,panic_abort

  built="$CORE_DIR/target/$target/release/$LIB_NAME"
  if [ ! -s "$built" ]; then
    echo "::error::cargo reported success but $built is missing or empty."
    echo "Nothing would link, and the failure would surface as an inscrutable"
    echo "linker error during the Xcode build instead of here."
    exit 1
  fi

  stage="$STAGE_ROOT/$platform"
  mkdir -p "$stage"
  # P1: -L dereferences. Cargo does not emit symlinks here today, but staging
  # is exactly the step where a dangling link becomes a confusing bundler abort
  # three minutes later, and the flag costs nothing.
  cp -RL "$built" "$stage/$LIB_NAME"

  # P4: prove the staged artifact, in the step that staged it.
  if [ ! -s "$stage/$LIB_NAME" ]; then
    echo "::error::staging failed — $stage/$LIB_NAME is missing or empty."
    exit 1
  fi
  size="$(wc -c < "$stage/$LIB_NAME" | tr -d ' ')"
  echo "staged $stage/$LIB_NAME (${size} bytes)"
done

echo
echo "── staged slices ──"
ls -la "$STAGE_ROOT"/*/ 2>/dev/null || true
echo "Done. Xcode links these via LIBRARY_SEARCH_PATHS=\$(SRCROOT)/../build/lib/\$(PLATFORM_NAME)."
