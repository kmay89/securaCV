#!/usr/bin/env bash
set -euo pipefail
#
# The fleet's onboarding parity family lives ONCE in firmware/common and is
# host-tested there (firmware/tests_host):
#
#   network/onboarding_fsm.h   the one onboarding state machine — the setup
#                              AP's name (SecuraCV-<type>-<4>), the AP hold
#                              after a join, the recovery re-raise, the
#                              factory-reset hold, the serial QR line
#   network/provision_core.h   onboarding_fsm.h's sibling include (the WIFI:
#                              QR payload it prints) — staged so the include
#                              resolves beside it, byte for byte
#   network/wifi_join_policy.h the JoinFailure vocabulary and the words every
#                              setup surface shows for a failed join
#   network/wifi_store.h       the saved-networks ring (add / remove / pick /
#                              serialize; includes wifi_credential_policy.h,
#                              which check_wifi_credential_sync.sh pins)
#   network/node_status.h      the GET /api/status body every node serves
#   io/reset_hold.h            the BOOT long-hold classifier (Warn / Reset /
#                              Wipe / Released)
#
# The PIO trees include them path-prefixed; the canary-wap Arduino sketch
# carries each as a byte-identical staged copy next to canary_wap.ino (the
# same committed-copy pattern as ap_security_policy.h and
# wifi_credential_policy.h, so a fresh zip download compiles without a setup
# step). If a copy ever differed, two products would name their setup
# network, judge a failed join, hold their AP or reset by button differently
# — the drift the shared headers exist to prevent. Edit the canonical file,
# copy it here.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_onboarding_sync.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMMON="$ROOT/firmware/common"
SKETCH="$ROOT/firmware/projects/canary-wap/arduino/canary_wap"

# canonical path under firmware/common -> the staged copy's basename
FILES=(
  network/onboarding_fsm.h
  network/provision_core.h
  network/wifi_join_policy.h
  network/wifi_store.h
  network/node_status.h
  io/reset_hold.h
)

drift=0
for rel in "${FILES[@]}"; do
  A="$COMMON/$rel"
  B="$SKETCH/$(basename "$rel")"
  if [ ! -f "$A" ]; then
    echo "::error::canonical file missing: firmware/common/$rel — update FILES in $0" >&2
    drift=1
    continue
  fi
  if [ ! -f "$B" ]; then
    echo "::error::staged copy missing: ${B#"$ROOT/"}" >&2
    echo "         Run: cp firmware/common/$rel ${B#"$ROOT/"}" >&2
    drift=1
    continue
  fi
  if ! diff -u "$A" "$B"; then
    echo "" >&2
    echo "::error::$(basename "$rel") drifted between common/ and the canary-wap sketch." >&2
    echo "Edit firmware/common/$rel (the canonical copy) and mirror it: cp firmware/common/$rel ${B#"$ROOT/"}" >&2
    drift=1
  fi
done

if [ "$drift" -ne 0 ]; then
  exit 1
fi
echo "onboarding parity family (${#FILES[@]} headers): common and canary-wap copies are identical."
