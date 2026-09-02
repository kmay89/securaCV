#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical Opera mesh modules at
# firmware/canary/lib/securacv_mesh/src/ and the vendored copies that live next
# to the canary-wap Arduino sketch (so a fresh GitHub zip download compiles
# without anyone having to run setup.sh first).
#
# Same shape as check_ble_scan_sync.sh, for the three mesh pairs that were the
# LAST staged family in the sketch with no sync guard at all — byte-identical
# today, and nothing but this file keeps them that way. These modules carry
# channel-hop and hub-election logic that both builds must agree on, or two
# Canaries running the two builds stop hearing each other.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_mesh_sync.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CANONICAL="$ROOT/firmware/canary/lib/securacv_mesh/src"
STAGED="$ROOT/firmware/projects/canary-wap/arduino/canary_wap"

FILES=(
  mesh_beacon.h mesh_beacon.cpp
  mesh_channel_hop.h mesh_channel_hop.cpp
  mesh_hub_election.h mesh_hub_election.cpp
)

drift=0
for name in "${FILES[@]}"; do
  src="$CANONICAL/$name"
  dst="$STAGED/$name"
  if [ ! -f "$src" ]; then
    echo "::error::Canonical mesh file missing: $src"
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
  echo "Mesh module copies are OUT OF SYNC. Edit the canonical file under firmware/canary/lib/securacv_mesh/src/ and copy it into the sketch."
  exit 1
fi
echo "Mesh module copies (mesh_beacon / mesh_channel_hop / mesh_hub_election) are in sync."
