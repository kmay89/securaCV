#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical device_signature module at
# firmware/common/identity/ and the vendored copy that lives next to the
# canary-wap Arduino sketch (so a fresh GitHub zip download compiles without
# anyone having to run setup.sh first).
#
# Same shape as check_mesh_sync.sh. This pair was the one committed-copy family
# in the sketch with NO sync guard, and it had forked in BOTH directions: the
# sketch grew the whoami presence proof + the HTTP enrollment endpoints, the
# canonical grew the `sense` canonical (canary-sense / canary-vision witness
# events) and two null-pointer guards. This module is what lets Home Assistant
# tell a Canary from an impersonator with broker access — signature code
# drifting silently is the worst kind of drift, so the copies are byte-identical
# and this file is what keeps them that way. The HTTP handlers stay inside
# `#ifdef ARDUINO`, so the headless variants that compile the canonical copy
# never register them and the linker drops them as unreferenced.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_signature_sync.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CANONICAL="$ROOT/firmware/common/identity"
STAGED="$ROOT/firmware/projects/canary-wap/arduino/canary_wap"

FILES=(
  device_signature.h device_signature.cpp
)

drift=0
for name in "${FILES[@]}"; do
  src="$CANONICAL/$name"
  dst="$STAGED/$name"
  if [ ! -f "$src" ]; then
    echo "::error::Canonical device_signature file missing: $src"
    drift=1
    continue
  fi
  if [ ! -f "$dst" ]; then
    echo "::error::Missing staged copy: $dst"
    echo "         Run: cp $src $dst"
    drift=1
    continue
  fi
  if ! cmp -s "$src" "$dst"; then
    echo "::error::Drift detected: $dst differs from $src"
    diff -u "$src" "$dst" || true
    drift=1
  fi
done

if [ "$drift" -ne 0 ]; then
  echo "device_signature copies are OUT OF SYNC. Edit the canonical file under firmware/common/identity/ and copy it into the sketch."
  exit 1
fi
echo "device_signature copies (device_signature.h / device_signature.cpp) are in sync."
