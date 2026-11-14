#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between every committed copy of the fleet-link BLE
# wire formats and their canonical headers under firmware/common/fleet_link/.
# Both are byte-exact BLE contracts shared across firmwares on one company id
# (0xFFFF) — if the copies diverge, a canary-wap and the Canaries and displays
# that hear it silently stop understanding each other's adverts, with no
# compile error to catch it. One PAIRS line per header:
#
#   fleet_beacon.h — the presence beacon (type 0x10, 11- or 13-byte blob).
#                    Host-tested by firmware/common/fleet_link/
#                    test_fleet_beacon.cpp and, on this copy, by the
#                    canary-wap's tests_host/test_fleet_beacon.cpp.
#   fleet_chirp.h  — the Chirp advert (types 0x01..0x05, 17-byte blob): the
#                    canary-wap's ble_chirp.h builds it and its ble_config.h
#                    names its constants over it; ble_scout_advert_kind.h (the
#                    Scout pairing filter, staged by check_ble_scan_sync.sh)
#                    asks its fleet_chirp_matches(). Host-tested by
#                    firmware/common/fleet_link/test_fleet_chirp.cpp.
#
# Canonical:  firmware/common/fleet_link/<name>
# Copies:     firmware/projects/canary-wap/arduino/canary_wap/<same name>
#               (committed next to the Arduino sketch, which compiles only its
#                own folder, so GitHub zip downloads compile without setup.sh)
#
# The canary, canary-sense and canary-vision PlatformIO builds include the
# canonical headers directly via -I firmware/common/fleet_link, and the
# canary-display build via -I firmware/common, so they keep no separate copy —
# nothing to check there. The canary-display Arduino sketch's flat copies are
# written by that project's setup.sh regen and held by
# check_display_arduino_sync.sh.
#
# Every file in the sketch folder that has a same-named file in
# common/fleet_link must be held by a PAIRS line, so staging a new fleet-link
# header without adding it here fails instead of leaving a copy that drifts
# unseen.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_fleet_beacon_sync.sh
#
# Exits non-zero if any copy differs from — or is missing against — its
# canonical header, or a staged fleet-link header is held by no PAIRS line.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

COMMON="${REPO_ROOT}/firmware/common/fleet_link"
WAP="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

# canonical|copy pairs, one per line.
PAIRS=(
    "${COMMON}/fleet_beacon.h|${WAP}/fleet_beacon.h"
    "${COMMON}/fleet_chirp.h|${WAP}/fleet_chirp.h"
)

# The staged headers' names, from PAIRS, for the messages below.
names=$(for p in "${PAIRS[@]}"; do basename "${p##*|}"; done | paste -sd, - | sed 's/,/, /g')

drift=0
for pair in "${PAIRS[@]}"; do
    src="${pair%%|*}"
    dst="${pair##*|}"
    if [ ! -f "$src" ]; then
        echo "::error::Canonical fleet-link header not found: $src"
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

# Every same-named file in the sketch folder is held by a PAIRS line.
for staged in "${WAP}"/*; do
    name=$(basename "$staged")
    [ -f "${COMMON}/${name}" ] || continue
    held=0
    for p in "${PAIRS[@]}"; do
        [ "$(basename "${p##*|}")" = "$name" ] && held=1
    done
    if [ "$held" -eq 0 ]; then
        echo "::error::${WAP}/${name} has a same-named file in common/fleet_link and no PAIRS line holds it"
        echo "         Add a PAIRS line to this script."
        drift=1
    fi
done

if [ "$drift" -ne 0 ]; then
    echo ""
    echo "Every fleet-link header the canary-wap sketch stages (${names})"
    echo "must be byte-identical to its canonical file under"
    echo "${COMMON},"
    echo "and every staged file with a same-named one there must be held by PAIRS."
    echo "Re-stage a copy with: cp <canonical-path> <copy-path>"
    exit 1
fi

echo "Fleet-link header copies are in sync (${names})."
