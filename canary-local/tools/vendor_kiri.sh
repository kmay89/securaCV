#!/usr/bin/env bash
# canary-local/tools/vendor_kiri.sh — vendor the Kiri:Moto slicing engine
# locally, offline-safe, for canary-local/assets/slicer.js.
#
# Run on a machine with network + node. Produces a self-contained engine under
# canary-local/assets/vendor/kiri/ that serves from THIS site with no runtime
# call to grid.space — preserving the Lab's "nothing phones anywhere" promise.
#
# IMPORTANT — read canary-local/assets/vendor/kiri/README.md first. Kiri:Moto
# is NOT a single-file library: its client (engine + worker + minion + wasm) is
# packed into a custom bundle served by grid-apps' own server, and the slicing
# pool needs SharedArrayBuffer, i.e. a CROSS-ORIGIN-ISOLATED page (COOP/COEP).
# This script does the buildable, verifiable part; it deliberately stops before
# the two steps that need a human decision (site isolation) and a real browser
# (the slice check). It will not commit anything it cannot verify is offline.
set -euo pipefail

# Pinned to the reviewed grid-apps commit. Bump deliberately, then re-verify a
# real in-browser slice (kiri_slice_probe.mjs) and re-run tests/slicer.test.js.
PIN_COMMIT="c2eca07ed29614e7c477bd53b1369889182bae2e"   # 2026-04-08; MIT
REPO="https://github.com/GridSpace/grid-apps"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # canary-local/
dest="$here/assets/vendor/kiri"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "→ cloning $REPO @ ${PIN_COMMIT:0:12}"
git clone --quiet --no-checkout "$REPO" "$work/grid-apps"
git -C "$work/grid-apps" checkout -q "$PIN_COMMIT"
cd "$work/grid-apps"

echo "→ installing build deps (no electron binary — we only need the web build)"
npm install --ignore-scripts --no-audit --no-fund

echo "→ building the client sources (esbuild: produces src/pack/*)"
node bin/esbuild.config.mjs

# The three browser entry points, esbuilt into standalone ESM. The engine loads
# the worker/minion via client.setWorkPath('./worker.js') / setPoolPath(
# './minion.js') (src/kiri/app/workers.js) so they resolve next to each other.
echo "→ bundling standalone engine + worker + minion"
mkdir -p "$dest"
npx --yes esbuild src/kiri/run/engine.js  --bundle --format=esm --platform=browser --outfile="$dest/engine.js"
npx --yes esbuild src/kiri/run/worker.js  --bundle --format=esm --platform=browser --outfile="$dest/worker.js"
npx --yes esbuild src/kiri/run/minion.js  --bundle --format=esm --platform=browser --outfile="$dest/minion.js"

echo "→ copying FDM WASM + license"
# kiri-geo.wasm is the FDM geometry kernel — without it slicing dies in the
# browser, so a missing file must fail the vendor, not ship a broken bundle.
cp -v src/wasm/kiri-geo.wasm "$dest/" || { echo "✗ kiri-geo.wasm not found — path moved? check src/wasm" >&2; exit 1; }
cp -v src/wasm/kiri-ani.wasm "$dest/" 2>/dev/null || true   # animation only — optional for FDM time
cp -v LICENSE "$dest/LICENSE"

# ── enforce the offline promise ────────────────────────────────────────────
echo "→ checking the vendored bundle does not phone home"
if grep -rniE "grid\.space|cdnjs|cloudflare|https?://[a-z]" "$dest"/*.js 2>/dev/null \
     | grep -viE "^\s*//|@license|source-?map|w3\.org|example\.com"; then
  echo "✗ vendored output references an external origin — it would phone home." >&2
  echo "  Rebuild resolving worker/wasm from ./ (setWorkPath/setPoolPath), or" >&2
  echo "  patch the offending reference, before committing." >&2
  exit 1
fi

# ── provenance ─────────────────────────────────────────────────────────────
{
  echo "Kiri:Moto slicing engine — vendored for canary-local/assets/slicer.js"
  echo "Source:  $REPO"
  echo "Commit:  $PIN_COMMIT"
  echo "License: MIT (see LICENSE)"
  echo "Built:   esbuild standalone engine/worker/minion + FDM wasm"
  echo "Files (sha256):"
  ( cd "$dest" && for f in *.js *.wasm; do [ -e "$f" ] && echo "  $(sha256sum "$f")"; done )
} > "$dest/PROVENANCE.txt"

cat <<'NEXT'
✓ engine staged and verified offline-clean.

TWO STEPS REMAIN (human + browser — see assets/vendor/kiri/README.md):

  1. Cross-origin isolation. The slicing pool needs SharedArrayBuffer, so the
     page must send COOP: same-origin + COEP: require-corp. On GitHub Pages add
     a coi-serviceworker shim (or set the headers at your host). Without this,
     the engine loads but the pool throws and the bridge falls back to the
     estimate.

  2. Verify a real slice:
       node canary-local/tests/kiri_slice_probe.mjs   # expects KIRI_PROBE_OK
     then: node --test canary-local/tests/slicer.test.js
NEXT
