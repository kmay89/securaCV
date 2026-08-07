#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between every committed copy of the birth-day decision
# (birth_day.h). This header is the ONE place that decides when a device may
# record the day its key was born, and when that day may be CALLED a birth day
# rather than the day it was first dated.
#
# Why drift here is worse than an ordinary copy skew: the rules are what make
# the birth certificate non-rottable. A tree that drifted to allow overwriting
# would let a device restate its own age — the certificate would show the last
# time it saw a clock, not when it was born — and a tree that drifted on the
# grace window would call a week on a shelf a birthday. Both produce a device
# that quietly misstates itself, with no compile error and nothing on screen
# that looks wrong.
#
# Canonical:  firmware/common/identity/birth_day.h
#             (host-tested by firmware/tests_host/test_birth_day.cpp)
# Copies:     - firmware/projects/canary-wap/arduino/canary_wap/birth_day.h
#               (committed next to the Arduino sketch so GitHub zip downloads
#                compile without setup.sh — same pattern as fleet_selfreport.h)
#
# PlatformIO trees include the canonical copy directly via -I firmware/common,
# so they keep no separate copy — nothing to check there. If a future tree adds
# a committed byte-copy, append it to COPIES.
#
# The canary-display Arduino sketch keeps no copy: a display pins OTHER
# devices' keys (src/trust.cpp) and has no witness key of its own, so it has
# no birth to record.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_birth_day_sync.sh
#
# Exits non-zero if any copy differs from — or is missing against — the
# canonical header.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL="${REPO_ROOT}/firmware/common/identity/birth_day.h"

COPIES=(
    "${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap/birth_day.h"
)

if [ ! -f "$CANONICAL" ]; then
    echo "::error::Canonical birth-day header not found: $CANONICAL"
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
    echo "Every committed birth_day.h copy must be byte-identical to"
    echo "$CANONICAL."
    echo "Re-stage a copy with: cp $CANONICAL <copy-path>"
    exit 1
fi

echo "Birth-day header copies are in sync."
