#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical boot banner library at
# firmware/common/boot/ and the committed copy that lives next to the
# canary-wap Arduino sketch (so fresh GitHub zip downloads compile without
# anyone having to run setup.sh first).
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_boot_sync.sh
#
# Exits non-zero if either file differs or is missing on either side.


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/boot"
STAGED="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

if [ ! -d "$CANONICAL" ]; then
    echo "::error::Canonical boot banner source dir not found: $CANONICAL"
    exit 1
fi

drift=0
for name in boot_banner.h boot_banner.cpp; do
    src="$CANONICAL/$name"
    dst="$STAGED/$name"
    if [ ! -f "$src" ]; then
        echo "::error::Canonical boot banner file missing: $src"
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
    echo "The committed boot banner copies under $STAGED/ must match $CANONICAL/."
    echo "Re-stage with: firmware/projects/canary-wap/setup.sh arduino"
    exit 1
fi

echo "Boot banner library copies are in sync."
