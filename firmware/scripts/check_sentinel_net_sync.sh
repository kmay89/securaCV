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
#   3. mqtt_mgr.cpp and witness.cpp, byte-identical as WHOLE FILES once a
#      short, named list of product regions is set aside. Everything else is
#      pinned — includes, the file-scope transport objects and constants
#      (wifiClient, PubSubClient, s_broker_tls, MQTT_SOCKET_TIMEOUT_SEC), the
#      connect path (TLS gate, LWT, the MAC-free client ID), the inbound OTA
#      command parser, the trust-surface publishes, the witness domain strings
#      and NVS key names, key generation and chain persistence — including any
#      function canary-sense adds later, until it is carried or named below.
#      The regions set aside are exactly:
#
#      Masked in BOTH copies (the product's own vocabulary):
#        mqtt_mgr.cpp  publish_heartbeat(), publish_state_retained() — the
#                      heartbeat/state payloads (radar vs fused-claim fields);
#        witness.cpp   the signatures of sign_event_envelope() and
#                      chain_advance() (sense's strings vs a SentinelClaim), and
#                      inside them only the call that builds or signs the event
#                      canonical (`sense` vs `sentinel`, both in
#                      firmware/common/identity/device_signature.cpp, pinned by
#                      their shared golden vector). The chain construction
#                      around that call stays pinned.
#
#      Excised from canary-sense's copy only (sense-only features Phase 1a
#      does not wire: the radar-dial number entities and the identify button):
#        - its `#include "canary/sense_config.h"`;
#        - the identify/radar-dial latch statics, their comments, and the
#          functions that feed or drain them (parse_cfg_number, take_pending,
#          take_pending_identify, take_pending_cfg_*, publish_sense_cfg_retained,
#          publish_identify_echo);
#        - inside on_mqtt_message(), the radar-dial dispatch and the identify
#          branch — the install/auto parser around them stays pinned;
#        - inside mqtt_connect_attempt(), the identify/radar-dial
#          re-subscriptions and the dial snapshot republish — the TLS gate,
#          LWT, client ID, connect and OTA re-subscribe around them stay pinned.
#
#      One comment line is swapped: sense's names CS_WATCHDOG_TIMEOUT_SEC where
#      the sentinel's names SENT_WATCHDOG_TIMEOUT_SEC.
#
#      Each excision is anchored to exact lines and may remove ONLY lines of the
#      shape it names (a comment, a blank, `mqtt.subscribe(g_topics.cfg_*_cmd,
#      1);` and so on). An anchor that moves, or a line in an excised region
#      that is not of that shape, fails this script — a fix canary-sense lands
#      inside one of those regions cannot be silently set aside.
#
# Comments inside the pinned code are canary-sense's, verbatim, so a few
# name its macros (CS_DEVICE_ID) or its radar; the sentinel's equivalents
# (SENT_DEVICE_ID → DEVICE_ID in canary/config.h) feed the same constant
# names, which is what lets the code be identical at all.
#
# To fix drift: make the change in canary-sense, then carry it here —
#   cp firmware/projects/canary-sense/<file> firmware/projects/canary-sentinel/<file>
# (for wifi_mgr.cpp, re-apply the sentinel's product-name line; for mqtt_mgr.cpp
# and witness.cpp, carry the hunk the diff below names). If a change is
# genuinely sense-only, keep it inside one of the regions above, or add a new
# region here with the narrowest line shape that describes it.
#
# Proven by scripts/tests/test_check_sentinel_net_sync.py (mutation cases in
# both directions; runs in lint.yml). Can be run from any directory (the repo
# root is resolved from the script's own location):
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
WITNESS=src/witness.cpp

drift=0

fail() {
  echo "::error::$1"
  drift=1
}

# ---------------------------------------------------------------------------
# Text transforms: stdin -> stdout. Patterns and lines travel through ENVIRON,
# never `awk -v` (which would eat the backslashes out of a regex). Each exits
# 3 when its anchor is not where the pin expects it, and drop_block exits 4
# when the region holds a line it does not excuse — so a moved anchor fails
# loudly instead of excusing nothing, or everything.

# drop_line RE: delete the one line matching RE.
drop_line() {
  RE="$1" awk '
    BEGIN { re = ENVIRON["RE"] }
    $0 ~ re { hits++; next }
    { print }
    END { if (hits != 1) exit 3 }
  '
}

# swap_line OLD NEW: the one line that is exactly OLD becomes exactly NEW.
swap_line() {
  OLD="$1" NEW="$2" awk '
    BEGIN { old = ENVIRON["OLD"]; new = ENVIRON["NEW"] }
    $0 == old { hits++; print new; next }
    { print }
    END { if (hits != 1) exit 3 }
  '
}

# drop_fns NAME_RE: delete every column-0 function DEFINITION whose name
# matches NAME_RE — its signature line through the first line that is
# exactly "}" (or the one line, when the body closes on it) — plus one blank
# line right after it. At least one must exist.
drop_fns() {
  NAME="$1" awk '
    BEGIN { re = "^[A-Za-z_][A-Za-z0-9_:<>*& ]*[ *&]" ENVIRON["NAME"] "\\(" }
    eat_blank { eat_blank = 0; if ($0 == "") next }
    infn { if ($0 == "}") { infn = 0; eat_blank = 1 }; next }
    $0 ~ re && $0 !~ /;[[:space:]]*$/ {
      hits++
      if ($0 ~ /\{.*\}[[:space:]]*$/) eat_blank = 1; else infn = 1
      next
    }
    { print }
    END { if (!hits || infn) exit 3 }
  '
}

# drop_run START ALLOW_RE: delete the one line that is exactly START and
# every line after it that matches ALLOW_RE; the first line that does not
# match ends the run and is kept.
drop_run() {
  START="$1" ALLOW="$2" awk '
    BEGIN { start = ENVIRON["START"]; allow = ENVIRON["ALLOW"] }
    run { if ($0 ~ allow) next; run = 0 }
    $0 == start { hits++; run = 1; next }
    { print }
    END { if (hits != 1) exit 3 }
  '
}

# drop_block START END ALLOW_RE: delete from the one line that is exactly
# START through the first line after it that is exactly END. Every line
# strictly between must match ALLOW_RE.
drop_block() {
  START="$1" END_="$2" ALLOW="$3" awk '
    BEGIN { start = ENVIRON["START"]; end_ = ENVIRON["END_"]; allow = ENVIRON["ALLOW"] }
    blk { if ($0 == end_) { blk = 0; closed++ } else if ($0 !~ allow) bad = 1; next }
    $0 == start { hits++; blk = 1; next }
    { print }
    END { if (hits != 1 || closed != 1) exit 3; if (bad) exit 4 }
  '
}

# mask START_RE END_RE LABEL: the one region from a line matching START_RE
# through the first line at or after it matching END_RE becomes the single
# line `<<masked: LABEL>>`. Applied to BOTH copies, so only the region's
# content is set aside, never its position.
mask() {
  START="$1" END_="$2" LABEL="$3" awk '
    BEGIN { start = ENVIRON["START"]; end_ = ENVIRON["END_"]; label = ENVIRON["LABEL"] }
    inm { if ($0 ~ end_) inm = 0; next }
    $0 ~ start {
      hits++
      print "<<masked: " label ">>"
      if ($0 !~ end_) inm = 1
      next
    }
    { print }
    END { if (hits != 1 || inm) exit 3 }
  '
}

# The text being normalized, and the label its failures carry.
T=""
T_OK=1
T_WHAT=""

# step <transform> <args...>: apply one transform to $T, or record why not.
step() {
  local out rc=0
  [ "$T_OK" -eq 1 ] || return 0
  out="$(printf '%s\n' "$T" | "$@")" || rc=$?
  if [ "$rc" -eq 0 ]; then
    T="$out"
    return 0
  fi
  T_OK=0
  if [ "$rc" -eq 4 ]; then
    fail "$T_WHAT: the region '$2' .. '$3' holds a line $1 does not excuse — carry it into canary-sentinel, or narrow the region in $0"
  else
    fail "$T_WHAT: $1 '$2' no longer matches the source exactly once — update the region list in $0"
  fi
}

# ---------------------------------------------------------------------------
# The regions (see the header for what each one is and why).

# Line shapes a sense-only region may contain.
BLANK_OR_COMMENT='^$|^[[:space:]]*//.*$'
LATCH_OK="$BLANK_OR_COMMENT"'|^static volatile (bool|long) s_pending_(identify|cfg_[a-z]+) = (false|-1);$'
CFG_DISPATCH_OK="$BLANK_OR_COMMENT"'|^  if \(strcmp\(topic, g_topics\.cfg_[a-z]+_cmd\) == 0\) \{$|^    s_pending_cfg_[a-z]+ = parse_cfg_number\(payload, len, [0-9]+\);$|^    return;$|^  \}$'
IDENTIFY_BRANCH_OK='^    if \(token_at\(p, n, "[A-Za-z]+", [0-9]+\)( \|\| token_at\(p, n, "[A-Za-z]+", [0-9]+\))*( \|\|)?$|^        token_at\(p, n, "[A-Za-z]+", [0-9]+\)( \|\| token_at\(p, n, "[A-Za-z]+", [0-9]+\))*\) \{$|^      s_pending_identify = true;$|^    \}$|^    return;$'
RESUBSCRIBE_OK="$BLANK_OR_COMMENT"'|^  mqtt\.subscribe\(g_topics\.(identify_cmd|cfg_[a-z]+_cmd), 1\);$'

# Masked in both mqtt_mgr.cpp copies: the product payloads.
mask_mqtt() {
  step mask '^void publish_heartbeat\(' '^}$' 'publish_heartbeat (product payload)'
  step mask '^void publish_state_retained\(' '^}$' 'publish_state_retained (product payload)'
}

# canary-sense's mqtt_mgr.cpp minus its sense-only features.
excise_sense_mqtt() {
  step drop_line '^#include "canary/sense_config\.h"'
  step swap_line \
    "// (CS_WATCHDOG_TIMEOUT_SEC) instead of resting on PubSubClient's library" \
    "// (SENT_WATCHDOG_TIMEOUT_SEC) instead of resting on PubSubClient's library"
  local fn
  for fn in parse_cfg_number take_pending take_pending_identify 'take_pending_cfg_[a-z]+' \
            publish_sense_cfg_retained publish_identify_echo; do
    step drop_fns "$fn"
  done
  step drop_run \
    '// Inbound identify command (HA identify button / companion app): the' \
    "$LATCH_OK"
  # on_mqtt_message: the radar-dial dispatch and the identify branch.
  step drop_run \
    '  // Runtime radar reflexes (HA number entities). Max values here are only the' \
    "$CFG_DISPATCH_OK"
  step drop_line '^  const bool is_identify = \(strcmp\(topic, g_topics\.identify_cmd\) == 0\);$'
  step swap_line \
    '  if (!is_install && !is_auto && !is_identify) return;' \
    '  if (!is_install && !is_auto) return;'
  step drop_block '  if (is_identify) {' '  }' "$IDENTIFY_BRANCH_OK"
  # mqtt_connect_attempt: the identify/radar-dial re-subscriptions.
  step drop_block \
    "  // Identify button: re-subscribe so the wizard's blink request always" \
    '  publish_sense_cfg_retained(g_topics);' \
    "$RESUBSCRIBE_OK"
}

# Masked in both witness.cpp copies: the event canonical's call sites.
mask_witness() {
  step mask '^bool sign_event_envelope\(' '\) \{$' 'sign_event_envelope signature (product claim)'
  step mask '^  if \(!device_signature::sign_[a-z]+\(' '\) \{$' 'event canonical signer (sense | sentinel)'
  step mask '^void chain_advance\(' '\) \{$' 'chain_advance signature (product claim)'
  step mask '^  const size_t n = device_signature::build_[a-z]+_canonical\($' '\);$' 'event canonical builder (sense | sentinel)'
}

# normalize <file> <label> <fn...>: read <file> into $T and run each named
# region list over it.
normalize() {
  local f="$1"; shift
  T_WHAT="$1"; shift
  T_OK=1
  T="$(cat "$f")"
  local fn
  for fn in "$@"; do "$fn"; done
}

# pin_file <relpath> <sense region fns> -- <sentinel region fns>
pin_file() {
  local rel="$1"; shift
  local src="$SENSE/$rel" dst="$SENT/$rel" a b ok_a
  local sense_fns=() sent_fns=()
  while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do sense_fns+=("$1"); shift; done
  [ "$#" -gt 0 ] && shift
  sent_fns=("$@")
  if [ ! -f "$src" ] || [ ! -f "$dst" ]; then
    fail "Missing $rel in canary-sense or canary-sentinel"
    return
  fi
  normalize "$src" "canary-sense/$rel" "${sense_fns[@]}"
  a="$T"; ok_a="$T_OK"
  normalize "$dst" "canary-sentinel/$rel" "${sent_fns[@]}"
  b="$T"
  [ "$ok_a" -eq 1 ] && [ "$T_OK" -eq 1 ] || return 0
  if [ "$a" != "$b" ]; then
    fail "Drift detected: canary-sentinel/$rel differs from canary-sense/$rel outside the product regions this script names"
    echo "--- diff (canary-sense minus its sense-only regions vs canary-sentinel; <<masked>> = product region) ---"
    diff -u --label "canary-sense/$rel" --label "canary-sentinel/$rel" \
      <(printf '%s\n' "$a") <(printf '%s\n' "$b") || true
  fi
}

# ---------------------------------------------------------------------------
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

# 3. Whole files but for the named product regions.
pin_file "$MQTT" excise_sense_mqtt mask_mqtt -- mask_mqtt
pin_file "$WITNESS" mask_witness -- mask_witness

if [ "$drift" -ne 0 ]; then
  echo ""
  echo "canary-sentinel's network/witness stack has DRIFTED from canary-sense's."
  echo "Make the change in firmware/projects/canary-sense and carry it into"
  echo "firmware/projects/canary-sentinel (see this script's header)."
  exit 1
fi
echo "canary-sentinel net stack in sync with canary-sense: ${#IDENTICAL[@]} files byte-identical, wifi_mgr.cpp identical but its product name, mqtt_mgr.cpp + witness.cpp identical but their named product regions."
