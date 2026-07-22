#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between every committed copy of the fleet-link presence
# beacon wire format (fleet_beacon.h). The beacon is a byte-exact BLE contract
# shared across firmwares (company 0xFFFF, type 0x10, 11-byte blob) — if the
# copies diverge, a canary and a canary-display silently stop understanding
# each other's presence beacon, with no compile error to catch it.
#
# Canonical:  firmware/common/fleet_link/fleet_beacon.h
# Copies:     - firmware/projects/canary-wap/arduino/canary_wap/fleet_beacon.h
#               (committed next to the Arduino sketch so GitHub zip downloads
#                compile without setup.sh; also host-tested there)
#
# The canary (ESP32-S3 PlatformIO) build includes the canonical copy directly
# via -I firmware/common/fleet_link, so it keeps no separate copy — nothing to
# check there. If a future tree adds a committed copy, append it to COPIES.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_fleet_beacon_sync.sh
#
# Exits non-zero if any copy differs from — or is missing against — the
# canonical header.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/fleet_link/fleet_beacon.h"

COPIES=(
    "${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap/fleet_beacon.h"
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
    echo "Every committed fleet_beacon.h copy must be byte-identical to"
    echo "$CANONICAL."
    echo "Re-stage a copy with: cp $CANONICAL <copy-path>"
    exit 1
fi

echo "Fleet beacon header copies are in sync."
