#!/usr/bin/env bash
# Unit tests for run_lib.sh (the bashio-free helpers used by run.sh and the
# s6 service scripts).
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

# ---- ha_device_id ----------------------------------------------------------
# The derivation every producer shares: pwk_ + first 8 hex of
# sha256("securacv:ha-device-id:v1:" + seed). The expected value is computed
# independently here (python), so a drift in the shell line is caught.
SEED_A="feedface"
EXPECTED_A="pwk_$(python3 -c 'import hashlib,sys; print(hashlib.sha256(("securacv:ha-device-id:v1:"+sys.argv[1]).encode()).hexdigest()[:8])' "$SEED_A")"
assert_eq "$EXPECTED_A" "$(ha_device_id "$SEED_A")" \
    "ha_device_id: pwk_ + sha256 prefix of the domain-separated seed"
case "$(ha_device_id "$SEED_A")" in
    pwk_[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) echo "ok: ha_device_id: shape is pwk_ + 8 hex" ;;
    *) echo "FAIL: ha_device_id: unexpected shape $(ha_device_id "$SEED_A")" >&2; FAILURES=$((FAILURES+1)) ;;
esac
case "$(ha_device_id "$SEED_A")" in
    *feedface*) echo "FAIL: ha_device_id: the seed leaks into the public id" >&2; FAILURES=$((FAILURES+1)) ;;
    *) echo "ok: ha_device_id: carries no bytes of the seed" ;;
esac

# ---- compose_discovery_payload (contract C6: five keys, in order) ----------
# The Supervisor /discovery announcement body: service pinned to "securacv";
# host, port (integer), token_file (a PATH, never a token), the bridge's
# state prefix and the kernel's public device id under config — exactly what
# the SecuraCV integration's hassio step consumes.
assert_eq '{"service":"securacv","config":{"host":"myaddon","port":8799,"token_file":"/config/api_token","mqtt_topic_prefix":"witness","device_id":"pwk_1a2b3c4d"}}' \
    "$(compose_discovery_payload "myaddon" 8799 /config/api_token witness pwk_1a2b3c4d)" \
    "discovery: payload carries service securacv + the five config keys in contract order"
assert_eq '{"service":"securacv","config":{"host":"d0491a67-privacy-witness-kernel","port":8799,"token_file":"/config/api_token","mqtt_topic_prefix":"witness"}}' \
    "$(compose_discovery_payload "d0491a67-privacy-witness-kernel")" \
    "discovery: port, token_file and prefix default; an unknown device id omits the key"
assert_eq '{"service":"securacv","config":{"host":"myaddon","port":8799,"token_file":"/config/api_token","mqtt_topic_prefix":"house","device_id":"pwk_00000000"}}' \
    "$(compose_discovery_payload "myaddon" 8799 /config/api_token house pwk_00000000)" \
    "discovery: a non-default topic prefix is carried (the integration must subscribe under it)"
# jq does the quoting, so a hostname with JSON metacharacters cannot corrupt
# the payload.
assert_eq '{"service":"securacv","config":{"host":"we\"ird","port":1234,"token_file":"/config/api_token","mqtt_topic_prefix":"witness"}}' \
    "$(compose_discovery_payload 'we"ird' 1234)" \
    "discovery: host with a quote stays valid JSON"
assert_eq "number" "$(compose_discovery_payload myaddon 8799 | jq -r '.config.port | type')" \
    "discovery: port is a JSON number"
# The shared seam vector (tests/fixtures/ha_contract/discovery.json, written by
# gen_ha_contract_fixtures.py): the SAME bytes the integration's hassio-step
# test feeds its config flow, so the producer and the consumer are held to
# one file.
DISCOVERY_FIXTURE="$(dirname "$0")/../../tests/fixtures/ha_contract/discovery.json"
if [ -f "$DISCOVERY_FIXTURE" ]; then
    assert_eq "$(jq -r '.payload' "$DISCOVERY_FIXTURE")" \
        "$(compose_discovery_payload \
            "$(jq -r '.args.host' "$DISCOVERY_FIXTURE")" \
            "$(jq -r '.args.port' "$DISCOVERY_FIXTURE")" \
            "$(jq -r '.args.token_file' "$DISCOVERY_FIXTURE")" \
            "$(jq -r '.args.mqtt_topic_prefix' "$DISCOVERY_FIXTURE")" \
            "$(jq -r '.args.device_id' "$DISCOVERY_FIXTURE")")" \
        "discovery: the payload is byte-identical to the shared C6 vector the integration reads"
else
    echo "skip: discovery seam vector not carried beside this checkout ($DISCOVERY_FIXTURE)"
fi
case "$(compose_discovery_payload myaddon 8799 /config/api_token witness pwk_1a2b3c4d)" in
    *token\":*) echo "FAIL: discovery: payload must never carry a token value" >&2; FAILURES=$((FAILURES+1)) ;;
    *) echo "ok: discovery: payload names the token file path, never a token" ;;
esac

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

# ---- install_blueprints ----------------------------------------------------
# The image carries the blueprints; the app places them under
# /config/blueprints/automation/securacv without ever overwriting a copy the
# user edited. The record file names what the app itself installed.
BP_SRC="$TMPDIR_TEST/shipped"
BP_DEST="$TMPDIR_TEST/config/blueprints/automation/securacv"
mkdir -p "$BP_SRC"
printf 'blueprint:\n  name: digest v1\n' > "$BP_SRC/securacv_daily_digest.yaml"
printf 'blueprint:\n  name: alerts v1\n' > "$BP_SRC/securacv_alerts.yaml"
printf 'not a blueprint\n' > "$BP_SRC/README.md"

OUT=$(install_blueprints "$BP_SRC" "$BP_DEST" | sort)
assert_eq "installed securacv_alerts.yaml
installed securacv_daily_digest.yaml" "$OUT" \
    "blueprints: a fresh destination gets every shipped .yaml, nothing else"
[ -f "$BP_DEST/securacv_daily_digest.yaml" ] || { echo "FAIL: blueprints: digest not installed" >&2; FAILURES=$((FAILURES+1)); }
[ ! -e "$BP_DEST/README.md" ] || { echo "FAIL: blueprints: non-yaml file copied" >&2; FAILURES=$((FAILURES+1)); }
assert_eq "2" "$(wc -l < "$BP_DEST/.securacv_app_sha256sums" | tr -d ' ')" \
    "blueprints: the record lists both installed files"

OUT=$(install_blueprints "$BP_SRC" "$BP_DEST" | sort)
assert_eq "unchanged securacv_alerts.yaml
unchanged securacv_daily_digest.yaml" "$OUT" \
    "blueprints: a second run with the same shipped copies changes nothing"

# The shipped digest moves to v2; the on-disk copy is still the app's own
# (matches the record) → it is updated.
printf 'blueprint:\n  name: digest v2\n' > "$BP_SRC/securacv_daily_digest.yaml"
OUT=$(install_blueprints "$BP_SRC" "$BP_DEST" | grep digest)
assert_eq "updated securacv_daily_digest.yaml" "$OUT" \
    "blueprints: the app's own unedited copy follows a shipped update"
assert_eq "blueprint:
  name: digest v2" "$(cat "$BP_DEST/securacv_daily_digest.yaml")" \
    "blueprints: the updated copy is the shipped v2"

# The user edits the alerts blueprint; a shipped v2 then arrives → kept.
printf 'blueprint:\n  name: alerts MINE\n' > "$BP_DEST/securacv_alerts.yaml"
printf 'blueprint:\n  name: alerts v2\n' > "$BP_SRC/securacv_alerts.yaml"
OUT=$(install_blueprints "$BP_SRC" "$BP_DEST" | grep alerts)
assert_eq "kept securacv_alerts.yaml" "$OUT" \
    "blueprints: a user-edited copy is never overwritten"
assert_eq "blueprint:
  name: alerts MINE" "$(cat "$BP_DEST/securacv_alerts.yaml")" \
    "blueprints: the user's edit survives"

# A file placed with no record (scripts/install.sh, or by hand) that differs
# from the shipped copy is treated as the user's: kept.
printf 'blueprint:\n  name: busybar v1\n' > "$BP_SRC/securacv_busybar_alert.yaml"
printf 'blueprint:\n  name: busybar OLD\n' > "$BP_DEST/securacv_busybar_alert.yaml"
OUT=$(install_blueprints "$BP_SRC" "$BP_DEST" | grep busybar)
assert_eq "kept securacv_busybar_alert.yaml" "$OUT" \
    "blueprints: an unrecorded, differing copy is kept (it may be edited)"
# ...unless it is byte-identical to the shipped copy, in which case the app
# adopts it (unchanged) and records it so future updates can follow.
printf 'blueprint:\n  name: hue v1\n' > "$BP_SRC/securacv_hue_alert_light.yaml"
cp "$BP_SRC/securacv_hue_alert_light.yaml" "$BP_DEST/securacv_hue_alert_light.yaml"
OUT=$(install_blueprints "$BP_SRC" "$BP_DEST" | grep hue)
assert_eq "unchanged securacv_hue_alert_light.yaml" "$OUT" \
    "blueprints: an identical unrecorded copy is adopted as unchanged"
assert_eq "1" "$(grep -c 'securacv_hue_alert_light.yaml' "$BP_DEST/.securacv_app_sha256sums")" \
    "blueprints: the adopted copy is recorded once"

# ----------------------------------------------------------------------------
if [ "$FAILURES" -gt 0 ]; then
    echo "$FAILURES test(s) failed" >&2
    exit 1
fi
echo "all run_lib tests passed"
