#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between every committed copy of the shared /api/fleet
# self-report builder (fleet_selfreport.h). This header is the ONE place the
# fleet-presence wire shape lives — the coarse JSON every networked Canary
# answers at GET /api/fleet (the contract in tvos/discovery/DISCOVERY.md, read
# by the Witness Wall emulator and the Flasher's post-flash LAN discovery). If
# the copies diverge, boards silently answer /api/fleet with different shapes
# and the wall/Flasher misparse them, with no compile error to catch it.
#
# Canonical:  firmware/common/fleet_selfreport/fleet_selfreport.h
# Copies:     - firmware/projects/canary-wap/arduino/canary_wap/fleet_selfreport.h
#               (committed next to the Arduino sketch so GitHub zip downloads
#                compile without setup.sh; also host-tested via the canonical).
#
# PlatformIO trees include the canonical copy directly via -I firmware/common,
# so they keep no separate copy — nothing to check there. If a future tree adds
# a committed byte-copy, append it to COPIES.
#
# The canary-display Arduino sketch ALSO carries a staged copy
# (arduino/canary_display/fleet_selfreport.h), but that one is owned by the
# display's regen mechanism (setup.sh copies it fresh from the canonical on
# every `./setup.sh regen`, and check_display_arduino_sync.sh proves the
# committed sketch matches a fresh regen) — so it is intentionally NOT listed
# here to keep single ownership of that copy with the regen guard.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_fleet_selfreport_sync.sh
#
# Exits non-zero if any copy differs from — or is missing against — the
# canonical header.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/fleet_selfreport/fleet_selfreport.h"

COPIES=(
    "${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap/fleet_selfreport.h"
)

if [ ! -f "$CANONICAL" ]; then
    echo "::error::Canonical fleet self-report header not found: $CANONICAL"
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
    echo "Every committed fleet_selfreport.h copy must be byte-identical to"
    echo "$CANONICAL."
    echo "Re-stage a copy with: cp $CANONICAL <copy-path>"
    exit 1
fi

echo "Fleet self-report header copies are in sync."
