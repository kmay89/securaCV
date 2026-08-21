#!/usr/bin/env bash
# Unit tests for run_lib.sh (the bashio-free helpers used by run.sh).
# Run directly: bash privacy_witness_kernel/tests/test_run_lib.sh
set -euo pipefail

LIB="$(dirname "$0")/../run_lib.sh"
# shellcheck source=privacy_witness_kernel/run_lib.sh disable=SC1091
source "$LIB"

TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT

FAILURES=0
assert_eq() {
    local expected="$1" actual="$2" label="$3"
    if [ "$expected" = "$actual" ]; then
        echo "ok: $label"
    else
        echo "FAIL: $label — expected '$expected', got '$actual'" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

# ---- resolve_value ---------------------------------------------------------
assert_eq "explicit" "$(resolve_value "explicit" "discovered" "fallback")" \
    "resolve_value: explicit wins"
assert_eq "discovered" "$(resolve_value "" "discovered" "fallback")" \
    "resolve_value: discovered wins over fallback"
assert_eq "fallback" "$(resolve_value "" "" "fallback")" \
    "resolve_value: fallback when nothing else set"
assert_eq "" "$(resolve_value "" "" "")" \
    "resolve_value: all empty yields empty"

# ---- resolve_frigate_topic -------------------------------------------------
assert_eq "frigate/events" "$(resolve_frigate_topic "" "")" \
    "resolve_frigate_topic: defaults to frigate/events"
assert_eq "frigate_house/events" "$(resolve_frigate_topic "" "frigate_house")" \
    "resolve_frigate_topic: derives from prefix"
assert_eq "custom/topic" "$(resolve_frigate_topic "custom/topic" "frigate_house")" \
    "resolve_frigate_topic: legacy full topic wins"

# ---- compose_discovery_payload ---------------------------------------------
# The Supervisor /discovery announcement body: service name is pinned to
# "securacv" and the host/port land under config, exactly what the SecuraCV
# integration's discovery step consumes.
assert_eq '{"service":"securacv","config":{"host":"myaddon","port":8799}}' \
    "$(compose_discovery_payload "myaddon" 8799)" \
    "discovery: payload carries service securacv + host + port"
assert_eq '{"service":"securacv","config":{"host":"d0491a67-privacy-witness-kernel","port":8799}}' \
    "$(compose_discovery_payload "d0491a67-privacy-witness-kernel")" \
    "discovery: port defaults to 8799"
# jq does the quoting, so a hostname with JSON metacharacters cannot corrupt
# the payload.
assert_eq '{"service":"securacv","config":{"host":"we\"ird","port":1234}}' \
    "$(compose_discovery_payload 'we"ird' 1234)" \
    "discovery: host with a quote stays valid JSON"

# ---- resolve_device_key_seed -----------------------------------------------
KEY_FILE="$TMPDIR_TEST/keys/device_key"

assert_eq "abc123" "$(resolve_device_key_seed "abc123" "$KEY_FILE")" \
    "seed: explicit option wins"
[ ! -e "$KEY_FILE" ] || { echo "FAIL: explicit option must not write the key file" >&2; FAILURES=$((FAILURES+1)); }

mkdir -p "$(dirname "$KEY_FILE")"
printf 'feedface\n' > "$KEY_FILE"
assert_eq "feedface" "$(resolve_device_key_seed "" "$KEY_FILE")" \
    "seed: key file wins when option empty"
assert_eq "feedface" "$(resolve_device_key_seed "devkey:mvp" "$KEY_FILE")" \
    "seed: placeholder option treated as unset"

rm -f "$KEY_FILE"
GENERATED=$(resolve_device_key_seed "" "$KEY_FILE")
assert_eq "64" "${#GENERATED}" "seed: generated seed is 64 hex chars"
case "$GENERATED" in
    *[!0-9a-f]*) echo "FAIL: generated seed has non-hex chars: $GENERATED" >&2; FAILURES=$((FAILURES+1)) ;;
    *) echo "ok: seed: generated seed is lowercase hex" ;;
esac
[ -s "$KEY_FILE" ] || { echo "FAIL: generated seed not persisted" >&2; FAILURES=$((FAILURES+1)); }
PERMS=$(stat -c '%a' "$KEY_FILE" 2>/dev/null || stat -f '%Lp' "$KEY_FILE")
assert_eq "600" "$PERMS" "seed: persisted key file is 0600"
assert_eq "$GENERATED" "$(resolve_device_key_seed "" "$KEY_FILE")" \
    "seed: second call returns the same persisted seed"

# ----------------------------------------------------------------------------
if [ "$FAILURES" -gt 0 ]; then
    echo "$FAILURES test(s) failed" >&2
    exit 1
fi
echo "all run_lib tests passed"
