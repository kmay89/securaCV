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

# ── Identity helper: device_pseudonym follows the same single-source pattern ──
# The canonical, header-only device_pseudonym lives at firmware/common/identity/;
# the canary-wap sketch carries a byte-identical staged copy so a fresh zip
# download compiles and both firmware trees derive the same pseudonym. It is
# header-only, so the sketch must NOT carry a stale device_pseudonym.cpp.
IDENTITY_CANONICAL="firmware/common/identity/device_pseudonym.h"
IDENTITY_STAGED="$STAGED/device_pseudonym.h"
if [ -f "$IDENTITY_CANONICAL" ]; then
    if [ ! -f "$IDENTITY_STAGED" ]; then
        echo "::error::Missing staged copy: $IDENTITY_STAGED"
        echo "         Run: cp $IDENTITY_CANONICAL $IDENTITY_STAGED"
        drift=1
    elif ! cmp -s "$IDENTITY_CANONICAL" "$IDENTITY_STAGED"; then
        echo "::error::Drift detected: $IDENTITY_STAGED differs from $IDENTITY_CANONICAL"
        echo "--- diff ($IDENTITY_CANONICAL vs $IDENTITY_STAGED) ---"
        diff -u "$IDENTITY_CANONICAL" "$IDENTITY_STAGED" || true
        drift=1
    fi
    if [ -f "$STAGED/device_pseudonym.cpp" ]; then
        echo "::error::Stale staged file: $STAGED/device_pseudonym.cpp"
        echo "         device_pseudonym is header-only now — remove it."
        drift=1
    fi
fi

# ── Witness store: same single-source pattern ──
# The canonical, header-only witness_store (the /WITNESS/records.jsonl
# byte-exact line format + SD-wins boot reconciliation) lives at
# firmware/common/witness/; the canary-wap sketch carries a byte-identical
# staged copy so a fresh zip download compiles. The PIO canary tree
# (firmware/canary) includes the canonical directly via -I ../common.
WITSTORE_CANONICAL="firmware/common/witness/witness_store.h"
WITSTORE_STAGED="$STAGED/witness_store.h"
if [ -f "$WITSTORE_CANONICAL" ]; then
    if [ ! -f "$WITSTORE_STAGED" ]; then
        echo "::error::Missing staged copy: $WITSTORE_STAGED"
        echo "         Run: cp $WITSTORE_CANONICAL $WITSTORE_STAGED"
        drift=1
    elif ! cmp -s "$WITSTORE_CANONICAL" "$WITSTORE_STAGED"; then
        echo "::error::Drift detected: $WITSTORE_STAGED differs from $WITSTORE_CANONICAL"
        echo "--- diff ($WITSTORE_CANONICAL vs $WITSTORE_STAGED) ---"
        diff -u "$WITSTORE_CANONICAL" "$WITSTORE_STAGED" || true
        drift=1
    fi
fi

# Provisioning QR grammar (onboarding wave): the canonical shared parser
# lives in firmware/common/provision_qr/; the canary-wap sketch carries a
# staged copy (the display's setup.sh stages its own via regen).
PROVQR_CANONICAL="firmware/common/provision_qr/provision_qr.h"
PROVQR_STAGED="$STAGED/provision_qr.h"
if [ -f "$PROVQR_CANONICAL" ]; then
    if [ ! -f "$PROVQR_STAGED" ]; then
        echo "::error::Missing staged copy: $PROVQR_STAGED"
        echo "         Run: cp $PROVQR_CANONICAL $PROVQR_STAGED"
        drift=1
    elif ! cmp -s "$PROVQR_CANONICAL" "$PROVQR_STAGED"; then
        echo "::error::Drift detected: $PROVQR_STAGED differs from $PROVQR_CANONICAL"
        echo "--- diff ($PROVQR_CANONICAL vs $PROVQR_STAGED) ---"
        diff -u "$PROVQR_CANONICAL" "$PROVQR_STAGED" || true
        drift=1
    fi
fi

# GNSS time helper: same single-source pattern. The canonical, header-only
# gnss_time (NMEA UTC date/time -> validated Unix epoch, used by the
# GPS-derived system clock in both trees) lives at firmware/common/gnss/; the
# canary-wap sketch carries a byte-identical staged copy so a fresh zip
# download compiles and both firmware trees apply the same trust window.
GNSSTIME_CANONICAL="firmware/common/gnss/gnss_time.h"
GNSSTIME_STAGED="$STAGED/gnss_time.h"
if [ -f "$GNSSTIME_CANONICAL" ]; then
    if [ ! -f "$GNSSTIME_STAGED" ]; then
        echo "::error::Missing staged copy: $GNSSTIME_STAGED"
        echo "         Run: cp $GNSSTIME_CANONICAL $GNSSTIME_STAGED"
        drift=1
    elif ! cmp -s "$GNSSTIME_CANONICAL" "$GNSSTIME_STAGED"; then
        echo "::error::Drift detected: $GNSSTIME_STAGED differs from $GNSSTIME_CANONICAL"
        echo "--- diff ($GNSSTIME_CANONICAL vs $GNSSTIME_STAGED) ---"
        diff -u "$GNSSTIME_CANONICAL" "$GNSSTIME_STAGED" || true
        drift=1
    fi
fi

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "The committed copies under $STAGED/ must match their canonical sources."
    echo "Re-stage with: firmware/projects/canary-wap/setup.sh arduino"
    exit 1
fi

echo "CSI + identity + witness-store + provision-qr + gnss-time library copies are in sync."
