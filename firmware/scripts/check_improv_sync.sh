#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the committed copies of the Improv Wi-Fi core
# (improv_core.h): the pure half of the Bluetooth setup door — the standard's
# RPC frames, the 0x4677 service data and the provisioning session. It is a
# byte-exact BLE contract the phone's Swift twin (ios/Shared/ImprovWire.swift)
# is pinned to; if the copies diverge, a WAP and a Sense would answer the same
# phone differently, with no compile error to catch it.
#
# Canonical:  firmware/common/network/improv_core.h (host-tested by
#             firmware/tests_host/test_improv_core.cpp; compiled by the
#             canary-sense and canary-vision PlatformIO envs through
#             common/network/improv_ble.cpp via -I firmware/common)
# Copies:     - firmware/projects/canary-wap/arduino/canary_wap/improv_core.h
#               (committed next to the Arduino sketch so GitHub zip downloads
#                compile without setup.sh)
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_improv_sync.sh
#
# Exit 0 when every copy matches; exit 1 (with a diff) on drift.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/network/improv_core.h"

COPIES=(
    "${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap/improv_core.h"
)

if [ ! -f "$CANONICAL" ]; then
    echo "::error::Canonical fleet beacon header not found: $CANONICAL"
    exit 1
fi

drift=0
for dst in "${COPIES[@]}"; do
    if [ ! -f "$dst" ]; then
        echo "::error::Missing committed copy: $dst"
        echo "         Run: cp $CANONICAL $dst"
        drift=1
        continue
    fi
    if ! cmp -s "$CANONICAL" "$dst"; then
        echo "::error::Drift detected: $dst differs from $CANONICAL"
        echo "--- diff ($CANONICAL vs $dst) ---"
        diff -u "$CANONICAL" "$dst" || true
        drift=1
    fi
done

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "Every committed improv_core.h copy must be byte-identical to"
    echo "$CANONICAL."
    echo "Re-stage a copy with: cp $CANONICAL <copy-path>"
    exit 1
fi

echo "Improv core header copies are in sync."
