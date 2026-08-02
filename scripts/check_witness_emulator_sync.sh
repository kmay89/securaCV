#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the app copies of the vendored Witness Wall
# emulator. The canonical emulator lives in the securacv_website repo;
# scripts/vendor_witness_emulator.sh stamps it into every app surface here in
# one pass. If the copies diverge, the Flasher's Fleet tab and the Lab's
# Witness Wall silently become two different products — with no compile error
# to catch it (the exact failure mode AGENTS.md rule 7, "two flashers, two
# frontends", exists to prevent).
#
#   Copy A: desktop/src/witness/    (the Flasher's "Your Fleet" tab)
#   Copy B: canary-local/witness/   (the Lab desktop app + browser Lab)
#
# Run from anywhere:
#   scripts/check_witness_emulator_sync.sh
#
# Exits non-zero on any drift or missing file. Fix with:
#   scripts/vendor_witness_emulator.sh <path-to-securacv_website>

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

A="${REPO_ROOT}/desktop/src/witness"
B="${REPO_ROOT}/canary-local/witness"
FILES=(witness.html tv-emulator.js highlight.js demo-fleet.json PROVENANCE.txt)

drift=0
for f in "${FILES[@]}"; do
  for d in "$A" "$B"; do
    if [ ! -f "${d}/${f}" ]; then
      echo "::error::missing ${d}/${f} — run scripts/vendor_witness_emulator.sh"
      drift=1
    fi
  done
  if [ -f "${A}/${f}" ] && [ -f "${B}/${f}" ] && ! cmp -s "${A}/${f}" "${B}/${f}"; then
    echo "::error::drift: ${A}/${f} differs from ${B}/${f}"
    echo "--- diff ---"
    diff -u "${A}/${f}" "${B}/${f}" | head -40 || true
    drift=1
  fi
done

if [ "$drift" -ne 0 ]; then
  echo ""
  echo "Every app copy of the Witness Wall emulator must be byte-identical."
  echo "Re-vendor all copies in one pass:"
  echo "  scripts/vendor_witness_emulator.sh <path-to-securacv_website-checkout>"
  exit 1
fi

echo "Witness Wall emulator app copies are in sync."
