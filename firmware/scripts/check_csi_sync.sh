#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical CSI library at
# firmware/common/csi/src/ and the committed copy that lives next to the
# canary-wap Arduino sketch (so fresh GitHub zip downloads compile without
# anyone having to run setup.sh first).
#
# Run from the repo root:
#   firmware/scripts/check_csi_sync.sh
#
# Exits non-zero if any file differs or is missing on either side.


CANONICAL="firmware/common/csi/src"
STAGED="firmware/projects/canary-wap/arduino/canary_wap"

if [ ! -d "$CANONICAL" ]; then
    echo "::error::Canonical CSI source dir not found: $CANONICAL"
    exit 1
fi

drift=0
for src in "$CANONICAL"/*.h "$CANONICAL"/*.cpp; do
    name=$(basename "$src")
    dst="$STAGED/$name"
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

# Also catch stale staged files that no longer exist in the canonical source.
for dst in "$STAGED"/csi_*.h "$STAGED"/csi_*.cpp \
           "$STAGED"/core_*.h "$STAGED"/core_*.cpp \
           "$STAGED"/anomaly_baseline.* "$STAGED"/meta_daily_summary.*; do
    [ -f "$dst" ] || continue
    name=$(basename "$dst")
    # Sketch-local files that share a csi_*/core_* prefix but are not part
    # of the firmware/common/csi library. csi_integration is the host-side
    # wiring; csi_dashboard_html is the headline UI; csi_mqtt is the
    # optional Home Assistant bridge — none of these are portable across
    # consumers of the CSI library, so they live next to the sketch only.
    case "$name" in
        csi_integration.h|csi_integration.cpp|csi_dashboard_html.h|csi_mqtt.h|csi_mqtt.cpp|csi_event_log.h|csi_event_log.cpp) continue ;;
    esac
    if [ ! -f "$CANONICAL/$name" ]; then
        echo "::error::Stale staged file (no canonical source): $dst"
        drift=1
    fi
done

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "The committed CSI copies under $STAGED/ must match $CANONICAL/."
    echo "Re-stage with: firmware/projects/canary-wap/setup.sh arduino"
    exit 1
fi

echo "CSI library copies are in sync."
