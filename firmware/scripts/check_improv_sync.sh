#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the firmware/common/network headers the
# canary-wap Arduino sketch stages and their canonical files. The sketch
# compiles only its own folder (GitHub zip downloads build without
# setup.sh), so each header it needs from common/network is committed
# byte-identical next to canary_wap.ino, and a copy that drifts compiles
# without a word. One PAIRS line per header; the messages below name them
# from PAIRS.
#
#   improv_core.h   — the Improv Wi-Fi core: the standard's RPC frames, the
#                     0x4677 service data and the provisioning session. A
#                     byte-exact BLE contract the phone's Swift twin
#                     (ios/Shared/ImprovWire.swift) is pinned to; if the copies
#                     diverge, a WAP and a Sense would answer the same phone
#                     differently, with no compile error to catch it.
#                     Host-tested by firmware/tests_host/test_improv_core.cpp;
#                     compiled by the canary-sense and canary-vision PlatformIO
#                     envs through common/network/improv_ble.cpp via
#                     -I firmware/common.
#   claim_ticket.h  — the claim a phone reads over that door once its join
#                     succeeded and spends on the home LAN for the pairing
#                     receipt (the bearer token never rides Bluetooth). Its
#                     TTL, hex shape and take-once rule are the contract the
#                     iOS app's claim step is written against. Host-tested by
#                     firmware/tests_host/test_claim_ticket.cpp.
#   auth_backoff.h  — the bearer lockout, one backoff per source address
#                     (sweep F343): 8 tracked addresses and a shared overflow,
#                     2 s doubling capped at 60 s. Host-tested by
#                     firmware/tests_host/test_auth_backoff.cpp.
#
# Canonical:  firmware/common/network/<name>
# Copies:     firmware/projects/canary-wap/arduino/canary_wap/<same name>
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_improv_sync.sh
#
# Exit 0 when every copy matches; exit 1 (with a diff) on drift.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

COMMON="${REPO_ROOT}/firmware/common/network"
WAP="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

# canonical|copy pairs, one per line.
PAIRS=(
    "${COMMON}/improv_core.h|${WAP}/improv_core.h"
    "${COMMON}/claim_ticket.h|${WAP}/claim_ticket.h"
    "${COMMON}/auth_backoff.h|${WAP}/auth_backoff.h"
)

# The staged headers' names, from PAIRS, for the messages below.
names=$(for p in "${PAIRS[@]}"; do basename "${p##*|}"; done | paste -sd, - | sed 's/,/, /g')

drift=0
for pair in "${PAIRS[@]}"; do
    src="${pair%%|*}"
    dst="${pair##*|}"
    if [ ! -f "$src" ]; then
        echo "::error::Canonical common/network header not found: $src"
        drift=1
        continue
    fi
    if [ ! -f "$dst" ]; then
        echo "::error::Missing committed copy: $dst"
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
    echo "Every common/network header the canary-wap sketch stages (${names})"
    echo "must be byte-identical to its canonical file under"
    echo "${COMMON}."
    echo "Re-stage a copy with: cp <canonical-path> <copy-path>"
    exit 1
fi

echo "The canary-wap sketch's staged common/network headers are in sync (${names})."
