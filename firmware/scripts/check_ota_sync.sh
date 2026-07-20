#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical pull-OTA engine at
# firmware/common/ota/src/ and the committed copies that live next to each
# Arduino sketch (so fresh GitHub zip downloads compile without anyone having
# to run setup.sh first).
#
# securacv_ota.cpp is the single most security-critical firmware module, and
# it is committed as byte-identical copies next to BOTH Arduino sketches:
#   - firmware/projects/canary-wap/arduino/canary_wap
#   - firmware/projects/canary-display/arduino/canary_display
# A fix applied to the canonical source must reach every copy; this guard
# fails if any copy drifts, so a change can never silently miss a tree.
#
# ota_release_key.h is included in the sync set on purpose: the release
# public key MUST be identical across the canary (PIO) build, the WAP and
# display pull-OTA engines, and the WAP's BLE OTA path — one release
# signature serves every update channel.
#
# The PlatformIO canary tree (firmware/canary) compiles the canonical source
# directly via -I ../common, so it needs no copy. The canary-display copy is
# also re-staged by setup.sh regen (verified end-to-end by
# check_display_arduino_sync.sh); this guard adds a direct, purpose-built
# byte-identical check so the OTA engine's single-source guarantee is
# explicit and does not rest on the display regen mechanism alone.
#
# Can be run from any directory (the repo root is resolved from the
# script's own location):
#   firmware/scripts/check_ota_sync.sh
#
# Exits non-zero if any file differs or is missing on either side.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/ota/src"

# Every Arduino sketch that carries a committed copy of the OTA engine.
STAGED_DIRS=(
    "${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"
    "${REPO_ROOT}/firmware/projects/canary-display/arduino/canary_display"
)

# The files that must be byte-identical across every copy.
OTA_FILES=(securacv_ota.h securacv_ota.cpp ota_release_key.h)

if [ ! -d "$CANONICAL" ]; then
    echo "::error::Canonical OTA engine source dir not found: $CANONICAL"
    exit 1
fi

drift=0
for staged in "${STAGED_DIRS[@]}"; do
    rel_staged="${staged#"${REPO_ROOT}/"}"
    for name in "${OTA_FILES[@]}"; do
        src="$CANONICAL/$name"
        dst="$staged/$name"
        if [ ! -f "$src" ]; then
            echo "::error::Canonical OTA engine file missing: $src"
            drift=1
            continue
        fi
        if [ ! -f "$dst" ]; then
            echo "::error::Missing staged copy: $rel_staged/$name"
            echo "         Run: cp $src $dst"
            drift=1
            continue
        fi
        if ! cmp -s "$src" "$dst"; then
            echo "::error::Drift detected: $rel_staged/$name differs from the canonical source"
            echo "--- diff ($src vs $dst) ---"
            diff -u "$src" "$dst" || true
            drift=1
        fi
    done
done

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "The committed OTA engine copies must match ${CANONICAL#"${REPO_ROOT}/"}/."
    echo "Re-stage every sketch, e.g.:"
    for staged in "${STAGED_DIRS[@]}"; do
        echo "  cp $CANONICAL/{securacv_ota.h,securacv_ota.cpp,ota_release_key.h} ${staged#"${REPO_ROOT}/"}/"
    done
    exit 1
fi

echo "OTA engine copies are in sync (canary-wap + canary-display)."
