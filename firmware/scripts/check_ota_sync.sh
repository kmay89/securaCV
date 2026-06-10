#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical pull-OTA engine at
# firmware/common/ota/src/ and the committed copies that live next to the
# canary-wap Arduino sketch (so fresh GitHub zip downloads compile without
# anyone having to run setup.sh first).
#
# ota_release_key.h is included in the sync set on purpose: the release
# public key MUST be identical across the canary (PIO) build, the WAP
# build's pull-OTA engine, and the WAP's BLE OTA path — one release
# signature serves every update channel.
#
# Can be run from any directory (the repo root is resolved from the
# script's own location):
#   firmware/scripts/check_ota_sync.sh
#
# Exits non-zero if any file differs or is missing on either side.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/ota/src"
STAGED="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

if [ ! -d "$CANONICAL" ]; then
    echo "::error::Canonical OTA engine source dir not found: $CANONICAL"
    exit 1
fi

drift=0
for name in securacv_ota.h securacv_ota.cpp ota_release_key.h; do
    src="$CANONICAL/$name"
    dst="$STAGED/$name"
    if [ ! -f "$src" ]; then
        echo "::error::Canonical OTA engine file missing: $src"
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
        echo "--- diff ($src vs $dst) ---"
        diff -u "$src" "$dst" || true
        drift=1
    fi
done

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "The committed OTA engine copies under $STAGED/ must match $CANONICAL/."
    echo "Re-stage with: cp $CANONICAL/{securacv_ota.h,securacv_ota.cpp,ota_release_key.h} $STAGED/"
    exit 1
fi

echo "OTA engine copies are in sync."
