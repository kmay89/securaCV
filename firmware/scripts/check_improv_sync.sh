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
#                     iOS app's claim step is written against; loop() sweeps
#                     it with expire() every pass (sweep F347). Host-tested by
#                     firmware/tests_host/test_claim_ticket.cpp.
#   provisioning_gate.h — the BOOT tap's gate, which the sketch's
#                     g_provisioning_gate is (its State; take(), take_exact(),
#                     the one-asker tap_arbitrate(), sweep F342; expire(), the
#                     sweep loop() runs every pass, sweep F347), and the socket
#                     address mapping the sketch reads a request's peer
#                     through (peer_key_from_addr(),
#                     ipv4_host_order_from_addr()), the same functions the
#                     canary tree's securacv_network uses (that tree's State
#                     is its main.cpp's). Host-tested by
#                     firmware/tests_host/test_provisioning_gate.cpp.
#   auth_backoff.h  — the bearer lockout, one backoff per source address
#                     (sweep F343): 8 tracked addresses and a shared overflow,
#                     2 s doubling capped at 60 s. api_auth.h holds the one
#                     table. Host-tested by
#                     firmware/tests_host/test_auth_backoff.cpp.
#
# Canonical:  firmware/common/network/<name>
# Copies:     firmware/projects/canary-wap/arduino/canary_wap/<same name>
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_improv_sync.sh
#
# Every file in the sketch folder that has a same-named file in
# common/network is held by a PAIRS line here or by the sync script
# HELD_ELSEWHERE names for it, which must name the file. So dropping a PAIRS
# line, or staging a new header no script holds, fails here instead of
# leaving a copy that drifts unseen.
#
# Exit 0 when every copy matches and every staged header is held; exit 1
# (with a diff, or the header no script holds) otherwise.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

COMMON="${REPO_ROOT}/firmware/common/network"
WAP="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

# canonical|copy pairs, one per line.
PAIRS=(
    "${COMMON}/improv_core.h|${WAP}/improv_core.h"
    "${COMMON}/claim_ticket.h|${WAP}/claim_ticket.h"
    "${COMMON}/provisioning_gate.h|${WAP}/provisioning_gate.h"
    "${COMMON}/auth_backoff.h|${WAP}/auth_backoff.h"
)

# The other common/network headers the sketch stages, each with the
# firmware/scripts/ sync script that holds its copy (name|script).
HELD_ELSEWHERE=(
    "ap_security_policy.h|check_ap_security_sync.sh"
    "mqtt_transport_logic.h|check_mqtt_transport_sync.sh"
    "node_status.h|check_onboarding_sync.sh"
    "onboarding_fsm.h|check_onboarding_sync.sh"
    "provision_core.h|check_onboarding_sync.sh"
    "wifi_credential_policy.h|check_wifi_credential_sync.sh"
    "wifi_join_policy.h|check_onboarding_sync.sh"
    "wifi_store.h|check_onboarding_sync.sh"
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

# Every same-named file is held: by PAIRS, or by its HELD_ELSEWHERE script.
unheld=0
for staged in "${WAP}"/*; do
    name=$(basename "$staged")
    [ -f "${COMMON}/${name}" ] || continue
    held=""
    for p in "${PAIRS[@]}"; do
        [ "$(basename "${p##*|}")" = "$name" ] && held="PAIRS"
    done
    for h in "${HELD_ELSEWHERE[@]}"; do
        [ "${h%%|*}" = "$name" ] || continue
        script="${SCRIPT_DIR}/${h##*|}"
        if [ -f "$script" ] && grep -qF -- "$name" "$script"; then
            held="${h##*|}"
        else
            echo "::error::HELD_ELSEWHERE says ${h##*|} holds ${name}, and that script does not name it"
        fi
    done
    if [ -z "$held" ]; then
        echo "::error::${WAP}/${name} has a same-named file in common/network and no sync script holds it"
        echo "         Add a PAIRS line here (or name the script that holds it in HELD_ELSEWHERE)."
        unheld=1
    fi
done

if [ "$unheld" -ne 0 ]; then
    drift=1
fi

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "Every common/network header the canary-wap sketch stages (${names})"
    echo "must be byte-identical to its canonical file under"
    echo "${COMMON},"
    echo "and every staged file with a same-named one there must be held by PAIRS"
    echo "or by the script HELD_ELSEWHERE names."
    echo "Re-stage a copy with: cp <canonical-path> <copy-path>"
    exit 1
fi

echo "The canary-wap sketch's staged common/network headers are in sync (${names})."
