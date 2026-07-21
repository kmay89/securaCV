#!/usr/bin/env bash
# canary-local/tools/vendor_kiri.sh — vendor the Kiri:Moto slicing engine
# locally, offline-safe, for canary-local/assets/slicer.js.
#
# Run on a machine with network + node. Produces a self-contained engine bundle
# under canary-local/assets/vendor/kiri/ that serves from THIS site with no
# runtime call to grid.space — preserving the Lab's "nothing phones anywhere"
# promise. Refuses to finish if the bundle would phone home.
#
# See canary-local/assets/vendor/kiri/README.md for the why and the contract.
set -euo pipefail

# Pinned to the reviewed grid-apps commit. Bump deliberately, then re-verify a
# real in-browser slice and re-run tests/slicer.test.js.
PIN_COMMIT="c2eca07ed29614e7c477bd53b1369889182bae2e"   # 2026-04-08; MIT
REPO="https://github.com/GridSpace/grid-apps"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # canary-local/
dest="$here/assets/vendor/kiri"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "→ cloning $REPO @ ${PIN_COMMIT:0:12}"
git clone --no-checkout "$REPO" "$work/grid-apps"
git -C "$work/grid-apps" checkout -q "$PIN_COMMIT"

echo "→ building the production bundle (this pulls three.js/jszip/quickjs/wasm)"
( cd "$work/grid-apps" && npm ci && npm run bundle:prod )

# grid-apps emits its servable client under web/ + built code under code/ after
# bundling; the engine host is web/kiri/engine.html. Copy the engine bundle and
# the WASM the FDM path needs. NOTE: exact output paths can shift between
# releases — if this list misses a file, the in-browser slice check below
# catches it. Adjust and re-pin.
mkdir -p "$dest"
copy() { [ -e "$work/grid-apps/$1" ] && cp -v "$work/grid-apps/$1" "$dest/$(basename "$1")" || echo "  (skip $1 — not present in this build)"; }
copy "code/engine.js"
copy "src/wasm/kiri-geo.wasm"
copy "src/wasm/kiri-ani.wasm"
copy "LICENSE"

# ── enforce the offline promise ────────────────────────────────────────────
echo "→ checking the vendored bundle does not phone home"
if grep -rniE "grid\.space|https?://[a-z]" "$dest"/*.js 2>/dev/null \
     | grep -viE "^\s*//|license|@license|source-?map|w3\.org|example\.com"; then
  echo "✗ vendored bundle references an external origin — it would phone home." >&2
  echo "  Rebuild with grid-apps' offline/self-hosted asset base, or patch the" >&2
  echo "  bundle to resolve worker/wasm from ./ before committing." >&2
  exit 1
fi

# ── provenance ─────────────────────────────────────────────────────────────
{
  echo "Kiri:Moto slicing engine — vendored for canary-local/assets/slicer.js"
  echo "Source:  $REPO"
  echo "Commit:  $PIN_COMMIT"
  echo "License: MIT (see LICENSE)"
  echo "Vendored files (sha256):"
  ( cd "$dest" && for f in *.js *.wasm; do [ -e "$f" ] && echo "  $(sha256sum "$f")"; done )
} > "$dest/PROVENANCE.txt"

echo "✓ vendored into $dest"
echo "  Next: open a device's Enclosure → print guide and click 'slice for exact"
echo "  time' to verify a real slice, then run: node --test canary-local/tests/slicer.test.js"
