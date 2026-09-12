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

# ── One CSI HAL: the canary product's lib/securacv_csi is an adapter ──
# firmware/canary/lib/securacv_csi/src/securacv_csi.cpp used to be a second
# copy of csi_hal.cpp + csi_features.cpp (~1150 lines), kept equal to the
# canonical by hand — the September 2026 pass did exactly that (roadmap 22).
# It is now a thin csi:: adapter over csi_hal::, and the canonical two files
# are compiled into the canary build by platformio.ini's build_src_filter.
# Five guards keep it that way:
#
#   1. Body divergence, name-based. No function the canonical HAL or
#      extractor DEFINES may also be defined in the adapter, unless
#      securacv_csi.h declares that name (init/start/stop/... exist in both
#      the csi:: and csi_hal:: namespaces by design). This reads
#      definition-shaped lines with grep — it does not parse C++ — so a
#      body hidden behind a macro or a lambda, or a copy of a body under a
#      NEW name, gets past it. The line budget (3) is the backstop for the
#      renamed copy; a copy under the canonical name also fails to link in
#      firmware/tests_host/test_csi_hal_adapter.cpp, which links the adapter
#      beside the canonical objects on the host; and for a second callback
#      registration the backstop is the canary CI build itself (two
#      esp_wifi_set_csi_rx_cb registrations crash at boot).
#   2. The adapter may not call the esp_wifi CSI driver itself — the one
#      registration in the image is csi_hal.cpp's.
#   3. A line budget. The adapter was $ADAPTER_LINES_AT_WRITING lines when
#      this guard was written; past $ADAPTER_LINE_BUDGET it fails, so a body
#      cannot creep back one helper at a time.
#   4. securacv_csi.h must include csi_types.h and must not re-declare its
#      contract (csi_features_t / csi_config_t / csi_stats_t / csi_cap_t or
#      the CSI_* constants). One typedef in the build is what lets main.cpp
#      and the module bridge share the struct without a cast dance.
#   5. platformio.ini must name csi_hal.cpp and csi_features.cpp in its
#      build_src_filter — the adapter links against nothing otherwise, and
#      the failure would be an undefined reference long after compile.

ADAPTER_DIR="firmware/canary/lib/securacv_csi/src"
ADAPTER_CPP="$ADAPTER_DIR/securacv_csi.cpp"
ADAPTER_H="$ADAPTER_DIR/securacv_csi.h"
ADAPTER_LINES_AT_WRITING=76
ADAPTER_LINE_BUDGET=120
CANARY_INI="firmware/canary/platformio.ini"

# Names of the functions a C++ file DEFINES (not merely declares): lines of
# the form `[qualifiers] type name(args...` that are not prototypes (`);`),
# control statements, assignments, member calls or preprocessor lines.
# Matches the flat one-definition-per-line style both files use — see the
# limits above.
defined_function_names() {
    sed -E 's/^[[:space:]]+//' "$@" \
    | grep -vE '^(//|/\*|\*|#|\})' \
    | grep -E '^[A-Za-z_][A-Za-z0-9_:<>,*& ]*[ *&][A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
    | grep -vE '\)[[:space:]]*(const)?[[:space:]]*;[[:space:]]*$' \
    | grep -vE '^(return|else|if|for|while|switch|case|do|goto|using|typedef|extern|namespace)\b' \
    | sed -E 's/^([^(=]*[ *&])([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\(.*$/\2/' \
    | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' \
    | sort -u
}

# Names a header DECLARES (prototype lines ending in `);`).
declared_function_names() {
    sed -E 's/^[[:space:]]+//' "$@" \
    | grep -E '^[A-Za-z_][A-Za-z0-9_:<>,*& ]*[ *&][A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(.*\)[[:space:]]*;[[:space:]]*$' \
    | sed -E 's/^([^(=]*[ *&])([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\(.*$/\2/' \
    | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' \
    | sort -u
}

if [ -f "$ADAPTER_CPP" ]; then
    # 1. Body divergence.
    canonical_defs=$(defined_function_names "$CANONICAL/csi_hal.cpp" "$CANONICAL/csi_features.cpp")
    adapter_defs=$(defined_function_names "$ADAPTER_CPP")
    header_decls=$(declared_function_names "$ADAPTER_H")
    shared=$(comm -12 <(echo "$canonical_defs") <(echo "$adapter_defs"))
    offending=$(comm -23 <(echo "$shared") <(echo "$header_decls") | sed '/^$/d')
    if [ -n "$offending" ]; then
        echo "::error::$ADAPTER_CPP defines functions the canonical CSI HAL/extractor also defines:"
        echo "$offending" | sed 's/^/           /'
        echo "         The adapter delegates to csi_hal:: — it must not carry a copy of a body."
        drift=1
    fi

    # 2. No direct driver calls in the adapter.
    if sed -E 's/^[[:space:]]+//' "$ADAPTER_CPP" | grep -vE '^(//|/\*|\*)' \
         | grep -nE 'esp_wifi_set_csi[a-z_]*[[:space:]]*\(' ; then
        echo "::error::$ADAPTER_CPP calls the esp_wifi CSI driver directly."
        echo "         The only registration in the image is csi_hal.cpp's; a second one crashes at boot."
        drift=1
    fi

    # 3. Line budget.
    adapter_lines=$(wc -l < "$ADAPTER_CPP")
    if [ "$adapter_lines" -gt "$ADAPTER_LINE_BUDGET" ]; then
        echo "::error::$ADAPTER_CPP is $adapter_lines lines; the adapter budget is $ADAPTER_LINE_BUDGET."
        echo "         New CSI behavior belongs in $CANONICAL (both products get it), not in the adapter."
        drift=1
    fi

    # 4. The header consumes the canonical contract instead of restating it.
    if ! grep -qE '^[[:space:]]*#[[:space:]]*include[[:space:]]+["<]csi_types\.h[">]' "$ADAPTER_H"; then
        echo "::error::$ADAPTER_H must #include csi_types.h — it is the one csi_features_t in the build."
        drift=1
    fi
    if grep -nE '^[[:space:]]*\}[[:space:]]*(csi_features_t|csi_config_t|csi_stats_t|csi_cap_t)[[:space:]]*;|^[[:space:]]*#[[:space:]]*define[[:space:]]+(CSI_FEATURE_DIM|CSI_MAX_SUBCARRIERS|CSI_WINDOW_MS|CSI_RSSI_NOISE_FLOOR_DBM|CSI_CONFIG_DEFAULT)\b' "$ADAPTER_H"; then
        echo "::error::$ADAPTER_H re-declares part of the csi_types.h contract (above)."
        drift=1
    fi

    # 5. The canary build compiles the canonical HAL.
    for src in csi_hal.cpp csi_features.cpp; do
        if ! grep -qE "^[[:space:]]*\+<\.\./\.\./common/csi/src/$src>" "$CANARY_INI"; then
            echo "::error::$CANARY_INI build_src_filter does not name ../../common/csi/src/$src."
            echo "         The adapter in $ADAPTER_DIR delegates to it; without it the link fails."
            drift=1
        fi
    done
fi

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "The committed copies under $STAGED/ must match their canonical sources,"
    echo "and the canary CSI library must stay a thin adapter over them."
    echo "Re-stage with: firmware/projects/canary-wap/setup.sh arduino"
    exit 1
fi

echo "CSI + identity + witness-store + provision-qr + gnss-time library copies are in sync; the canary CSI adapter is thin."
