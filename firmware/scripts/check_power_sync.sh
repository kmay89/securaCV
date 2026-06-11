#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical battery decision logic at
# firmware/common/power/power_logic.h and the committed copy that lives
# next to the canary-wap Arduino sketch (so fresh GitHub zip downloads
# compile without anyone having to run setup.sh first). Same pattern as
# check_csi_sync.sh.
#
# Run from the repo root:
#   firmware/scripts/check_power_sync.sh

CANONICAL="firmware/common/power/power_logic.h"
STAGED="firmware/projects/canary-wap/arduino/canary_wap/power_logic.h"

if [ ! -f "$CANONICAL" ]; then
    echo "::error::Canonical power logic header not found: $CANONICAL"
    exit 1
fi

if [ ! -f "$STAGED" ]; then
    echo "::error::Missing staged copy: $STAGED"
    echo "         Run: cp $CANONICAL $STAGED"
    exit 1
fi

if ! cmp -s "$CANONICAL" "$STAGED"; then
    echo "::error::Drift detected: $STAGED differs from $CANONICAL"
    echo "--- diff ($CANONICAL vs $STAGED) ---"
    diff -u "$CANONICAL" "$STAGED" || true
    echo "Fix by editing the canonical file and re-copying:"
    echo "  cp $CANONICAL $STAGED"
    exit 1
fi

echo "OK: power_logic.h staged copy matches canonical source."
