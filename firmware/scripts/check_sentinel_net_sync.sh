#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between canary-sense's network/witness stack and the
# copy canary-sentinel carries (F22 Phase 1a).
#
# canary-sentinel's Phase 1a network path is canary-sense's: supervised WiFi
# STA with the shared setup portal, MQTT with LWT + HA discovery, the witness
# key/chain plumbing, signed pull-OTA glue, the _securacv._tcp mDNS advert,
# NVS-backed runtime config and heap diagnostics. It is COPIED into
# firmware/projects/canary-sentinel (the way canary-vision carries its own
# src/net) rather than promoted to firmware/common, because a common/net
# promotion would move canary-sense, canary-vision and their sketch mirrors —
# its own milestone. A copy is only safe if it cannot drift silently: a fix
# to canary-sense's reconnect, TLS gate or key handling must reach the
# sentinel, or the two products quietly stop behaving alike. This script is
# what makes the copy a pin. Same shape as check_boot_sync.sh /
# check_fleet_beacon_sync.sh, with two finer grains where a whole-file pin
# would force product-specific code into both trees:
#
#   1. WHOLE FILES, byte-identical (cmp -s) — IDENTICAL below.
#   2. wifi_mgr.cpp, byte-identical but for ONE line: the setup network's
#      product name (`pc.product_name = "..."`), which must name the product
#      a phone is joining. The line must appear exactly once in each copy.
#   3. mqtt_mgr.cpp and witness.cpp, byte-identical FUNCTION BY FUNCTION for
#      the functions named below: the broker transport, the trust surface
#      (status / health / chain publishes), the OTA command latches, and the
#      identity-key + chain persistence. What is NOT pinned is the product's
#      own vocabulary — the state/heartbeat/event payloads, the connect-time
#      subscriptions (sense subscribes its radar dials and identify button,
#      which Phase 1a does not wire), and the event canonical (`sense` vs
#      `sentinel`, both in firmware/common/identity/device_signature.cpp).
#      A function is extracted from its column-0 signature to the first line
#      that is exactly `}` (or the same line, for a one-line body) — the
#      house style both trees are written in; a missing function fails.
#
# Comments inside the pinned code are canary-sense's, verbatim, so a few
# name its macros (CS_DEVICE_ID) or its radar; the sentinel's equivalents
# (SENT_DEVICE_ID → DEVICE_ID in canary/config.h) feed the same constant
# names, which is what lets the code be identical at all.
#
# To fix drift: make the change in canary-sense, then carry it here —
#   cp firmware/projects/canary-sense/<file> firmware/projects/canary-sentinel/<file>
# (for wifi_mgr.cpp, re-apply the sentinel's product-name line; for the
# function-pinned files, paste the function). If a change is genuinely
# product-specific, move it out of a pinned function instead of unpinning it.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_sentinel_net_sync.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SENSE="$ROOT/firmware/projects/canary-sense"
SENT="$ROOT/firmware/projects/canary-sentinel"

IDENTICAL=(
  include/canary/log.h
  include/canary/version.h
  include/canary/runtime_config.h
  include/canary/diagnostics.h
  include/canary/net/wifi_mgr.h
  include/canary/net/mdns_mgr.h
  include/canary/net/ota_mgr.h
  include/canary/ha/ha_discovery.h
  include/secrets.ci.h
  secrets/secrets.example.h
  src/runtime_config.cpp
  src/diagnostics.cpp
  src/net/mdns_mgr.cpp
  src/net/ota_mgr.cpp
)

WIFI=src/net/wifi_mgr.cpp
WIFI_PRODUCT_LINE='^  pc\.product_name = "[^"]*";$'
SENTINEL_PRODUCT='  pc.product_name = "Canary Sentinel";'

MQTT=src/net/mqtt_mgr.cpp
MQTT_FUNCS=(
  token_at
  take_pending_install
  take_pending_auto
  publish_checked
  mqtt_init
  mqtt_connected
  mqtt_loop
  publish_status_retained
  publish_event
  publish_health_retained
  publish_chain_retained
  ha_discovery_publish_once
  publish_update_state_retained
  publish_update_auto_retained
)

WITNESS=src/witness.cpp
WITNESS_FUNCS=(
  sha256_domain
  secure_zero
  load_or_generate_keypair
  load_or_start_chain
  persist_chain
  init
  ready
  chain_length
  chain_head
)

drift=0

fail() {
  echo "::error::$1"
  drift=1
}

# Print one function: from the column-0 line that declares NAME( to the first
# line that is exactly "}" — or just that line, when the body closes on it.
extract_fn() {
  awk -v name="$2" '
    !found && $0 ~ ("^[A-Za-z_][A-Za-z0-9_:<>*& ]*[ *&]" name "\\(") {
      found = 1
      print
      if ($0 ~ /\{.*\}[[:space:]]*$/) exit
      next
    }
    found {
      print
      if ($0 == "}") exit
    }
  ' "$1"
}

pin_functions() {  # <relpath> <func>...
  local rel="$1"; shift
  local src="$SENSE/$rel" dst="$SENT/$rel" fn a b
  if [ ! -f "$src" ] || [ ! -f "$dst" ]; then
    fail "Missing $rel in canary-sense or canary-sentinel"
    return
  fi
  for fn in "$@"; do
    a="$(extract_fn "$src" "$fn")"
    b="$(extract_fn "$dst" "$fn")"
    if [ -z "$a" ]; then
      fail "canary-sense $rel no longer defines $fn() — update the pin list in $0"
      continue
    fi
    if [ -z "$b" ]; then
      fail "canary-sentinel $rel does not define $fn() — carry it from canary-sense"
      continue
    fi
    if [ "$a" != "$b" ]; then
      fail "Drift detected: $fn() in canary-sentinel/$rel differs from canary-sense/$rel"
      diff -u <(printf '%s\n' "$a") <(printf '%s\n' "$b") || true
    fi
  done
}

# 1. Whole files.
for rel in "${IDENTICAL[@]}"; do
  src="$SENSE/$rel"
  dst="$SENT/$rel"
  if [ ! -f "$src" ]; then
    fail "canary-sense file missing: $src — update IDENTICAL in $0"
    continue
  fi
  if [ ! -f "$dst" ]; then
    fail "Missing sentinel copy: $dst"
    echo "         Run: cp $src $dst"
    continue
  fi
  if ! cmp -s "$src" "$dst"; then
    fail "Drift detected: $dst differs from $src"
    diff -u "$src" "$dst" || true
  fi
done

# 2. wifi_mgr.cpp — identical but for the setup network's product name.
for f in "$SENSE/$WIFI" "$SENT/$WIFI"; do
  if [ ! -f "$f" ]; then
    fail "Missing $f"
    continue
  fi
  n="$(grep -cE "$WIFI_PRODUCT_LINE" "$f" || true)"
  if [ "$n" != "1" ]; then
    fail "$f: expected exactly one \`pc.product_name = \"...\";\` line, found $n — the one line this pin excuses"
  fi
done
if [ -f "$SENT/$WIFI" ] && ! grep -qxF "$SENTINEL_PRODUCT" "$SENT/$WIFI"; then
  fail "canary-sentinel/$WIFI must name its own setup network: $SENTINEL_PRODUCT"
fi
if [ -f "$SENSE/$WIFI" ] && [ -f "$SENT/$WIFI" ]; then
  norm() { sed -E "s/$WIFI_PRODUCT_LINE/  pc.product_name = <PRODUCT>;/" "$1"; }
  if ! diff -q <(norm "$SENSE/$WIFI") <(norm "$SENT/$WIFI") > /dev/null; then
    fail "Drift detected: canary-sentinel/$WIFI differs from canary-sense/$WIFI beyond the product-name line"
    diff -u <(norm "$SENSE/$WIFI") <(norm "$SENT/$WIFI") || true
  fi
fi

# 3. Function-level pins.
pin_functions "$MQTT" "${MQTT_FUNCS[@]}"
pin_functions "$WITNESS" "${WITNESS_FUNCS[@]}"

if [ "$drift" -ne 0 ]; then
  echo ""
  echo "canary-sentinel's network/witness stack has DRIFTED from canary-sense's."
  echo "Make the change in firmware/projects/canary-sense and carry it into"
  echo "firmware/projects/canary-sentinel (see this script's header)."
  exit 1
fi
echo "canary-sentinel net stack in sync with canary-sense: ${#IDENTICAL[@]} files byte-identical, wifi_mgr.cpp identical but its product name, ${#MQTT_FUNCS[@]} mqtt_mgr + ${#WITNESS_FUNCS[@]} witness functions identical."
