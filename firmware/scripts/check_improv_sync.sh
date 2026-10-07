#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the committed copies of the Bluetooth setup
# door's pure headers:
#
#   improv_core.h   — the Improv Wi-Fi core: the standard's RPC frames, the
#                     0x4677 service data and the provisioning session. A
#                     byte-exact BLE contract the phone's Swift twin
#                     (ios/Shared/ImprovWire.swift) is pinned to; if the copies
#                     diverge, a WAP and a Sense would answer the same phone
#                     differently, with no compile error to catch it.
#   claim_ticket.h  — the claim a phone reads over that door once its join
#                     succeeded and spends on the home LAN for the pairing
#                     receipt (the bearer token never rides Bluetooth). Its
#                     TTL, hex shape and take-once rule are the contract the
#                     iOS app's claim step is written against.
#
# Canonical:  firmware/common/network/improv_core.h (host-tested by
#             firmware/tests_host/test_improv_core.cpp; compiled by the
#             canary-sense and canary-vision PlatformIO envs through
#             common/network/improv_ble.cpp via -I firmware/common)
#             firmware/common/network/claim_ticket.h (host-tested by
#             firmware/tests_host/test_claim_ticket.cpp)
# Copies:     firmware/projects/canary-wap/arduino/canary_wap/<same name>
#             (committed next to the Arduino sketch so GitHub zip downloads
#              compile without setup.sh)
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
)

drift=0
for pair in "${PAIRS[@]}"; do
    src="${pair%%|*}"
    dst="${pair##*|}"
    if [ ! -f "$src" ]; then
        echo "::error::Canonical Improv setup-door header not found: $src"
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
    echo "Every committed copy of the setup door's pure headers (improv_core.h,"
    echo "claim_ticket.h) must be byte-identical to its canonical file under"
    echo "${COMMON}."
    echo "Re-stage a copy with: cp <canonical-path> <copy-path>"
    exit 1
fi

echo "Improv setup-door header copies are in sync (improv_core.h, claim_ticket.h)."
