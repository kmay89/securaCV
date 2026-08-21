#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical BLE Scout library at
# firmware/canary/lib/securacv_ble_scan/src/ and the vendored copy that lives
# next to the canary-wap Arduino sketch (so a fresh GitHub zip download
# compiles without anyone having to run setup.sh first).
#
# The Arduino IDE only sees files inside the sketch folder and globally
# installed libraries, so the Scout module is committed twice: once in the
# PlatformIO library (canonical, compiled by firmware/canary's `full` env) and
# once next to the sketch. This guard catches drift in either direction — the
# ble/mesh sets were historically the ONLY canary-wap staged families with no
# sync guard, and they had silently forked in both directions.
#
# Most of the family is byte-identical. Two .cpp files carry DOCUMENTED,
# intentional divergences (see the header comment in each vendored file), so
# like check_audio_sync.sh the comparison normalizes both sides first:
#
#   1. fleet_roster_feed — the canonical Scout feeds a second consumer that
#      tracks OTHER Canaries (fleet_roster_feed). The WAP tracks its fleet
#      through the mesh layer instead, so fleet_roster_feed and its
#      fleet_roster.h dependency are not staged into the sketch. The
#      normalizer strips the fleet_roster_feed include/consumer/tick lines.
#   2. NimBLE init ownership — the canary PIO build's init owner is the
#      FEATURE_BLE_STATUS securacv_ble_status service (the Scout ATTACHES);
#      the WAP's owner is bluetooth_channel.cpp (the Scout brings the stack up
#      itself, idempotently). The Scout's own init call site differs between
#      the two, so the normalizer collapses that ownership block to a token.
#
# What normalization CANNOT hide, and is asserted separately below: BOTH copies
# must consult ble_heap_guard::can_init() before NimBLEDevice::init(). That is
# the boot-loop crash fix (ble_heap_guard.h) — the guard header's rule is that
# every init call site checks free heap first, or a no-PSRAM build asserts and
# boot-loops. ble_heap_guard.h + bt_defaults.h are themselves in the
# byte-identical set so the threshold can never fork.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_ble_scan_sync.sh
#
# Exits non-zero on drift or missing files.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL_DIR="${REPO_ROOT}/firmware/canary/lib/securacv_ble_scan/src"
STAGED_DIR="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

# Files that must be byte-for-byte identical across both copies.
#   ble_heap_guard.h + bt_defaults.h are in this set on purpose: the canonical
#   lib gained a copy when the boot-loop crash fix was ported into its Scout
#   init site, and the OOM threshold must never fork between the two trees.
BYTE_FILES=(
    ble_scan.h ble_scan.cpp
    ble_scout.h
    ble_scout_key.h ble_scout_key.cpp
    ble_scout_state.h ble_scout_state.cpp
    ble_heap_guard.h bt_defaults.h
)

# Files with documented divergences — compared after normalization.
NORMALIZED_FILES=(ble_scout.cpp ble_scout_nimble.cpp)

if [ ! -d "$CANONICAL_DIR" ]; then
    echo "::error::Canonical BLE Scout source dir not found: $CANONICAL_DIR"
    exit 1
fi

drift=0

# ── Presence check ───────────────────────────────────────────────────────
for name in "${BYTE_FILES[@]}" "${NORMALIZED_FILES[@]}"; do
    for f in "$CANONICAL_DIR/$name" "$STAGED_DIR/$name"; do
        if [ ! -f "$f" ]; then
            echo "::error::BLE Scout module file missing: $f"
            drift=1
        fi
    done
done
[ "$drift" -ne 0 ] && exit 1

# ── Byte-identical set ───────────────────────────────────────────────────
for name in "${BYTE_FILES[@]}"; do
    if ! cmp -s "$CANONICAL_DIR/$name" "$STAGED_DIR/$name"; then
        echo "::error::Drift detected: $STAGED_DIR/$name differs from canonical"
        echo "--- diff (canonical vs staged) ---"
        diff -u "$CANONICAL_DIR/$name" "$STAGED_DIR/$name" || true
        drift=1
    fi
done

# ── Normalizer ───────────────────────────────────────────────────────────
# POSIX awk/sed only (no GNU/BSD-specific extensions) so CI (GNU) and a macOS
# dev box (BSD) agree. strip_comments removes every /* */ and // comment and
# collapses whitespace, so all comment-only divergence (the header notes, the
# rationale blocks) vanishes before the code is compared.
strip_comments() {
    awk '
    BEGIN { inc = 0 }
    {
        line = $0; out = ""; i = 1; n = length(line)
        while (i <= n) {
            if (inc) {
                if (substr(line, i, 2) == "*/") { inc = 0; i += 2 }
                else { i += 1 }
            } else {
                c2 = substr(line, i, 2)
                if (c2 == "/*") { inc = 1; i += 2 }
                else if (c2 == "//") { break }
                else { out = out substr(line, i, 1); i += 1 }
            }
        }
        gsub(/[ \t]+/, " ", out); sub(/^ /, "", out); sub(/ $/, "", out)
        if (out != "") print out
    }' "$1"
}

# Collapse the Scout NimBLE-init ownership block (the one documented structural
# divergence) to a single token in both trees. The block runs from the
# `if (!NimBLEDevice::isInitialized...` line to just before the shared
# `s_scanner = NimBLEDevice::getScan();` reconvergence line.
collapse_init() {
    awk '
    BEGIN { skip = 0 }
    {
        if (skip) {
            if ($0 == "s_scanner = NimBLEDevice::getScan();") { skip = 0; print }
            next
        }
        if (index($0, "if (!NimBLEDevice::isInitialized") == 1) {
            print "__SCOUT_INIT_OWNERSHIP__"; skip = 1; next
        }
        print
    }'
}

# ble_scout.cpp: only the fleet_roster_feed consumer diverges.
normalize_scout() {
    strip_comments "$1" \
    | sed \
        -e '/^#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN$/,/^#endif$/d' \
        -e '/^const uint32_t now = now_ms_impl();$/d' \
        -e 's/^ble_scout_tick(now);$/ble_scout_tick(now_ms_impl());/'
}

# ble_scout_nimble.cpp: fleet_roster_feed consumer + init ownership block.
normalize_nimble() {
    strip_comments "$1" \
    | sed \
        -e '/^#include "fleet_roster_feed.h"$/d' \
        -e '/^#include <string>$/d' \
        -e '/^const uint32_t now = millis();$/d' \
        -e 's/^ble_scout_on_advert(mac, rssi, now);$/ble_scout_on_advert(mac, rssi, millis());/' \
        -e '/^if (device->haveManufacturerData()) {$/,/^}$/d' \
    | collapse_init
}

compare_normalized() {
    local name="$1" fn="$2"
    local a b
    a="$(mktemp)"; b="$(mktemp)"
    "$fn" "$CANONICAL_DIR/$name" > "$a"
    "$fn" "$STAGED_DIR/$name"    > "$b"
    if ! cmp -s "$a" "$b"; then
        echo "::error::Drift detected: $STAGED_DIR/$name diverges from canonical beyond the documented adaptations"
        echo "         Apply the change to BOTH copies (or re-apply the documented divergences)."
        echo "--- diff (canonical vs staged, both normalized) ---"
        diff -u "$a" "$b" || true
        drift=1
    fi
    rm -f "$a" "$b"
}

compare_normalized ble_scout.cpp        normalize_scout
compare_normalized ble_scout_nimble.cpp normalize_nimble

# ── Crash-fix invariant ──────────────────────────────────────────────────
# The boot-loop guard must be present at BOTH Scout init sites — normalization
# collapses the init block, so assert it here directly.
for f in "$CANONICAL_DIR/ble_scout_nimble.cpp" "$STAGED_DIR/ble_scout_nimble.cpp"; do
    if ! grep -q "ble_heap_guard::can_init" "$f"; then
        echo "::error::Missing boot-loop crash guard (ble_heap_guard::can_init) in $f"
        echo "         Every NimBLEDevice::init() call site must consult ble_heap_guard first."
        drift=1
    fi
done

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "The committed BLE Scout copies under $STAGED_DIR/ must match the canonical"
    echo "library at $CANONICAL_DIR/ (modulo the documented fleet_roster_feed and"
    echo "init-ownership divergences). Apply the change to both trees."
    exit 1
fi

echo "OK: securacv_ble_scan vendored copies match canonical (modulo documented divergences)."
