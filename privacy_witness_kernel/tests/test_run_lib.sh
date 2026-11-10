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

# ---- opt_list (sweep HA38) -------------------------------------------------
# run.sh reads every list option through opt_list: bashio::config prints an
# array one element per line, so the list is read whole from the options
# file instead. Each case holds the exact bytes, so the app's jq 1.6 (CI puts
# it first on PATH and names it in PWK_REQUIRE_JQ) and a runner's 1.7 are
# held to the same output, not merely to "some array".
JQ_VERSION=$(jq --version 2>&1 || true)
echo "info: opt_list cases run on ${JQ_VERSION}"
if [ -n "${PWK_REQUIRE_JQ:-}" ]; then
    assert_eq "$PWK_REQUIRE_JQ" "$JQ_VERSION" \
        "opt_list: the cases run on the jq CI requires (the app's)"
elif [ -n "${CI:-}" ]; then
    echo "FAIL: CI must name the jq these cases run on (PWK_REQUIRE_JQ, e.g. jq-1.6)" >&2
    FAILURES=$((FAILURES + 1))
fi

OPTS="$TMPDIR_TEST/options.json"
# opt_list KEY with OPTIONS_JSON=FILE, stdout only (the note goes to stderr).
opt_from() {
    OPTIONS_JSON="$1" opt_list "$2" 2>/dev/null
}

# A camera name holding a quote, a newline and an emoji (the quote and the
# newline escaped, the emoji as raw UTF-8), beside the frigate block the
# wizard writes. The same options with the emoji escaped too follow below.
cat > "$OPTS" <<'JSON'
{
  "mode": "standalone",
  "cameras": [
    {"name": "say \"cheese\"\nporch 🏠", "url": "rtsp://10.0.0.2/p", "zone_id": "zone:porch", "fps": 10},
    {"name": "🏠 back", "url": "rtsp://10.0.0.3/b"}
  ],
  "frigate": {"cameras": ["front_door", "driveway"], "labels": ["person", "car", "dog", "cat"], "min_confidence": 0.5},
  "go2rtc_url": "http://homeassistant.local:1984"
}
JSON
assert_eq '[{"name":"say \"cheese\"\nporch 🏠","url":"rtsp://10.0.0.2/p","zone_id":"zone:porch","fps":10},{"name":"🏠 back","url":"rtsp://10.0.0.3/b"}]' \
    "$(opt_from "$OPTS" cameras)" \
    "opt_list: two cameras come back as ONE array; a quote, a newline and an emoji survive"
assert_eq "2" "$(opt_from "$OPTS" cameras | jq length)" \
    "opt_list: jq length counts cameras, not each camera's keys"
assert_eq "say \"cheese\"
porch 🏠" "$(opt_from "$OPTS" cameras | jq -r '.[0].name')" \
    "opt_list: .[0] is the first camera, its name byte for byte"
assert_eq '["front_door","driveway"]' "$(opt_from "$OPTS" frigate.cameras)" \
    "opt_list: a dotted key reads the frigate block's list"
assert_eq "person,car,dog,cat" "$(opt_from "$OPTS" frigate.labels | jq -r 'join(",")')" \
    "opt_list: the labels join as run.sh joins them"
assert_eq "[]" "$(opt_from "$OPTS" go2rtc_url)" \
    "opt_list: a string option is not a list: []"
assert_eq "[]" "$(opt_from "$OPTS" frigate.min_confidence)" \
    "opt_list: a number is not a list: []"
assert_eq "[]" "$(opt_from "$OPTS" frigate)" \
    "opt_list: an object is not a list: []"
assert_eq "[]" "$(opt_from "$OPTS" mode.cameras)" \
    "opt_list: a path through a string reads as [] (jq's error is caught)"

# The same cameras as an ASCII-only JSON writer spells them (Python's
# json.dumps default): the emoji as its UTF-16 surrogate pair. jq decodes the
# pair, so the bytes out are the raw case's, on 1.6 as on 1.7.
cat > "$OPTS" <<'JSON'
{"mode": "standalone", "cameras": [{"name": "say \"cheese\"\nporch \ud83c\udfe0", "url": "rtsp://10.0.0.2/p", "zone_id": "zone:porch", "fps": 10}, {"name": "\ud83c\udfe0 back", "url": "rtsp://10.0.0.3/b"}]}
JSON
assert_eq '[{"name":"say \"cheese\"\nporch 🏠","url":"rtsp://10.0.0.2/p","zone_id":"zone:porch","fps":10},{"name":"🏠 back","url":"rtsp://10.0.0.3/b"}]' \
    "$(opt_from "$OPTS" cameras)" \
    "opt_list: the emoji written as a surrogate pair comes back as the raw bytes"
assert_eq "say \"cheese\"
porch 🏠" "$(opt_from "$OPTS" cameras | jq -r '.[0].name')" \
    "opt_list: the escaped name's .[0] is byte for byte the raw one"

# The frigate block missing entirely, cameras null, cameras empty.
printf '{"mode":"frigate","cameras":null}\n' > "$OPTS"
assert_eq "[]" "$(opt_from "$OPTS" cameras)" "opt_list: cameras null reads as []"
assert_eq "[]" "$(opt_from "$OPTS" frigate.cameras)" \
    "opt_list: the frigate block missing entirely: frigate.cameras is []"
assert_eq "" "$(opt_from "$OPTS" frigate.labels | jq -r 'join(",")')" \
    "opt_list: the frigate block missing entirely: the labels join to nothing"
printf '{"cameras":[],"frigate":{"cameras":[],"labels":[]}}\n' > "$OPTS"
assert_eq "[]" "$(opt_from "$OPTS" cameras)" "opt_list: an empty list reads as []"
assert_eq "0" "$(opt_from "$OPTS" cameras | jq length)" "opt_list: an empty list counts 0"
assert_eq "" "$(opt_from "$OPTS" frigate.labels | jq -r 'join(",")')" \
    "opt_list: an empty label list joins to nothing"

# opt_list KEY's return code with OPTIONS_JSON=FILE.
opt_rc() {
    local rc=0
    OPTIONS_JSON="$1" opt_list "$2" > /dev/null 2>&1 || rc=$?
    echo "$rc"
}

# A file it can read returns 0 whatever the list: the person has none.
printf '{"mode":"frigate","cameras":null}\n' > "$OPTS"
assert_eq "0" "$(opt_rc "$OPTS" cameras)" "opt_list: cameras null returns 0"
assert_eq "0" "$(opt_rc "$OPTS" frigate.cameras)" "opt_list: a missing block returns 0"
assert_eq "0" "$(opt_rc "$OPTS" mode)" "opt_list: a value that is not a list returns 0"

# The options file absent, empty, not JSON, or JSON that is not an object:
# [] and a note on stderr, never the string "null", and a return of 1, since
# the lists are then unknown, not empty (run.sh's frigate block stops on it;
# tests/test_serve_wizard.py runs run.sh's own lines on an absent file).
for bad in absent empty garbage array string; do
    file="$TMPDIR_TEST/$bad.json"
    case "$bad" in
        empty) : > "$file" ;;
        garbage) printf 'cameras: [a, b]\n' > "$file" ;;
        array) printf '[{"name":"a","url":"rtsp://a"}]\n' > "$file" ;;
        string) printf '"cameras"\n' > "$file" ;;
    esac
    label="opt_list: an options file that is $bad"
    assert_eq "[]" "$(opt_from "$file" cameras)" "$label reads as []"
    note=$(OPTIONS_JSON="$file" opt_list frigate.labels 2>&1 >/dev/null) || true
    assert_eq "opt_list: could not read frigate.labels from $file; reading it as []" "$note" \
        "$label: the note names the key and the file"
    assert_eq "1" "$(opt_rc "$file" cameras)" "$label returns 1"
    # The first-run gate's own test acts on the count, not the return, so
    # under set -euo pipefail it counts no camera and goes on.
    out=$(OPTIONS_JSON="$file" bash -c 'set -euo pipefail; source "$1"
        configured=false
        if [ "$(opt_list cameras | jq length)" -gt 0 ]; then configured=true; fi
        printf "%s|done\n" "$configured"' _ "$LIB" 2>/dev/null) || out="exited $?"
    assert_eq "false|done" "$out" "$label: the gate's count goes on under set -euo pipefail"
done

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

# ---- migrate_device_key_file -----------------------------------------------
# The one-time move of the seed file out of /config. The verifier is a stub
# log_verify (LOG_VERIFY_BIN) that records how it was called and accepts the
# seed named in $ACCEPT_SEED: the real one derives the database key and walks
# the lineage, which no host test can do without the kernel binaries.
MIG="$TMPDIR_TEST/migrate"
OLD_KEY="$MIG/config/.securacv/device_key"
NEW_KEY="$MIG/data/device_key"
MIG_DB="$MIG/config/witness.db"
STUB_LOG="$MIG/verify.log"
mkdir -p "$MIG/bin" "$MIG/config/.securacv" "$MIG/data"
cat > "$MIG/bin/log_verify" <<'STUB'
#!/usr/bin/env bash
# argv and the seed's presence in the environment, never its value
printf 'argv:%s env:%s\n' "$*" "${DEVICE_KEY_SEED:+set}" >> "$STUB_LOG"
[ "${DEVICE_KEY_SEED:-}" = "${ACCEPT_SEED:-}" ]
STUB
chmod +x "$MIG/bin/log_verify"
export LOG_VERIFY_BIN="$MIG/bin/log_verify" STUB_LOG
SEED_A="a1b2c3d4a1b2c3d4a1b2c3d4a1b2c3d4a1b2c3d4a1b2c3d4a1b2c3d4a1b2c3d4"
SEED_B="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"

# no legacy file: nothing happens, nothing printed
rm -f "$OLD_KEY" "$NEW_KEY"
assert_eq "" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: no legacy file prints nothing"
[ ! -e "$NEW_KEY" ] || { echo "FAIL: key move: created a key from nothing" >&2; FAILURES=$((FAILURES+1)); }

# legacy file, no log yet: moved on the byte check alone, 0600, legacy gone
printf '%s\n' "$SEED_A" > "$OLD_KEY"
chmod 644 "$OLD_KEY"
rm -f "$MIG_DB"
assert_eq "moved no-log" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: with no sealed log the byte-identical copy is the proof"
assert_eq "$SEED_A" "$(head -n1 "$NEW_KEY")" "key move: the new file holds the seed"
assert_eq "600" "$(stat -c '%a' "$NEW_KEY" 2>/dev/null || stat -f '%Lp' "$NEW_KEY")" \
    "key move: the new file is 0600 whatever the legacy mode was"
[ ! -e "$OLD_KEY" ] || { echo "FAIL: key move: legacy copy not removed" >&2; FAILURES=$((FAILURES+1)); }
[ ! -e "$MIG/verify.log" ] || { echo "FAIL: key move: verifier called with no log" >&2; FAILURES=$((FAILURES+1)); }
for leftover in "$MIG"/data/*.tmp.*; do
    [ -e "$leftover" ] && { echo "FAIL: key move: temp file left behind: $leftover" >&2; FAILURES=$((FAILURES+1)); }
done

# legacy file and a log that accepts the seed: verified, legacy gone; the
# verifier saw the seed in its environment only, never on argv
rm -f "$NEW_KEY"
printf '%s\n' "$SEED_A" > "$OLD_KEY"
printf 'not really sqlite\n' > "$MIG_DB"
ACCEPT_SEED="$SEED_A"; export ACCEPT_SEED
assert_eq "moved verified" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: the log accepts the seed, the legacy copy is removed"
[ ! -e "$OLD_KEY" ] || { echo "FAIL: key move: legacy copy not removed after verification" >&2; FAILURES=$((FAILURES+1)); }
assert_eq "argv:--db $MIG_DB --lineage --ui plain env:set" "$(cat "$STUB_LOG")" \
    "key move: the verifier gets --lineage on the log and the seed in its environment, not on argv"
case "$(cat "$STUB_LOG")" in
    *"$SEED_A"*) echo "FAIL: key move: the seed reached the verifier's argv" >&2; FAILURES=$((FAILURES+1)) ;;
    *) echo "ok: key move: the seed is not on the verifier's argv" ;;
esac

# legacy file the log refuses: copied, but kept in both places
rm -f "$NEW_KEY" "$STUB_LOG"
printf '%s\n' "$SEED_B" > "$OLD_KEY"
assert_eq "kept unverified" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: a seed the log refuses is kept under /config too"
[ -s "$OLD_KEY" ] || { echo "FAIL: key move: a refused legacy copy was removed" >&2; FAILURES=$((FAILURES+1)); }
assert_eq "$SEED_B" "$(head -n1 "$NEW_KEY")" "key move: the refused seed is still the one the app will read"
# ...and the next start, with both files identical, holds it to the log again
assert_eq "kept unverified" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: identical copies are re-verified, not trusted"
ACCEPT_SEED="$SEED_B"
assert_eq "moved verified" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: once the log accepts it, the identical legacy copy goes"
[ ! -e "$OLD_KEY" ] || { echo "FAIL: key move: identical legacy copy not removed" >&2; FAILURES=$((FAILURES+1)); }

# the new file is in use and a restored backup brings back a different key
printf '%s\n' "$SEED_A" > "$OLD_KEY"
ACCEPT_SEED="$SEED_A"
assert_eq "kept differs" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: a differing legacy copy changes nothing"
assert_eq "$SEED_B" "$(head -n1 "$NEW_KEY")" "key move: the key in use is untouched"
[ -s "$OLD_KEY" ] || { echo "FAIL: key move: differing legacy copy removed" >&2; FAILURES=$((FAILURES+1)); }

# no verifier installed: unverified, nothing removed
rm -f "$NEW_KEY"
LOG_VERIFY_BIN="$MIG/bin/missing"
assert_eq "kept unverified" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB")" \
    "key move: with no verifier the legacy copy is kept"
if [ ! -s "$OLD_KEY" ] || [ ! -s "$NEW_KEY" ]; then
    echo "FAIL: key move: both copies must exist without a verifier" >&2; FAILURES=$((FAILURES+1))
fi
LOG_VERIFY_BIN="$MIG/bin/log_verify"

# the copy cannot be written: failed, legacy untouched, no partial file
rm -f "$NEW_KEY"
chmod 500 "$MIG/data"
if [ "$(id -u)" = "0" ]; then
    echo "skip: key move: copy failure case needs an unprivileged user (root ignores modes)"
else
    assert_eq "failed copy" "$(migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB" || true)" \
        "key move: an unwritable destination fails the move"
    migrate_device_key_file "$OLD_KEY" "$NEW_KEY" "$MIG_DB" > /dev/null 2>&1 \
        && { echo "FAIL: key move: failed copy must return 1" >&2; FAILURES=$((FAILURES+1)); }
    [ -s "$OLD_KEY" ] || { echo "FAIL: key move: legacy copy touched by a failed move" >&2; FAILURES=$((FAILURES+1)); }
fi
chmod 700 "$MIG/data"
unset LOG_VERIFY_BIN ACCEPT_SEED STUB_LOG

# ---- remove_kernel_seed_copy -----------------------------------------------
# The copy witnessd used to keep beside the sealed log goes only when it
# holds the seed in use, byte for byte; anything else is left for a person.
KSEED="$MIG/config/witness.ed25519.seed"

rm -f "$KSEED"
assert_eq "" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A")" \
    "kernel seed copy: no file prints nothing"

# the kernel's copy holds the seed in use (witnessd wrote it trimmed, 0600)
printf '%s\n' "$SEED_A" > "$KSEED"
chmod 600 "$KSEED"
assert_eq "removed" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A")" \
    "kernel seed copy: the copy of the key in use is removed"
[ ! -e "$KSEED" ] || { echo "FAIL: kernel seed copy: identical copy not removed" >&2; FAILURES=$((FAILURES+1)); }

# surrounding whitespace is not a difference (the kernel trims on read too)
printf '  %s \n\n' "$SEED_A" > "$KSEED"
assert_eq "removed" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A")" \
    "kernel seed copy: whitespace around the seed is not a different key"

# a different key: kept, untouched
printf '%s\n' "$SEED_B" > "$KSEED"
assert_eq "kept differs" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A")" \
    "kernel seed copy: a copy holding another key is kept"
assert_eq "$SEED_B" "$(head -n1 "$KSEED")" "kernel seed copy: the kept file is unchanged"

# a prefix of the key in use is a different key, not a match
printf '%s\n' "${SEED_A:0:32}" > "$KSEED"
assert_eq "kept differs" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A")" \
    "kernel seed copy: a truncated seed is a different key"

# an empty file is never 'the key in use'
: > "$KSEED"
assert_eq "kept differs" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A")" \
    "kernel seed copy: an empty file is kept for a person to look at"
[ -e "$KSEED" ] || { echo "FAIL: kernel seed copy: empty file removed" >&2; FAILURES=$((FAILURES+1)); }

# the seed is never printed: every verdict is a fixed word
printf '%s\n' "$SEED_A" > "$KSEED"
case "$(remove_kernel_seed_copy "$KSEED" "$SEED_A" 2>&1; remove_kernel_seed_copy "$KSEED" "$SEED_B" 2>&1)" in
    *"$SEED_A"*|*"$SEED_B"*) echo "FAIL: kernel seed copy: a verdict carried the seed" >&2; FAILURES=$((FAILURES+1)) ;;
    *) echo "ok: kernel seed copy: no verdict names the seed" ;;
esac

# the file cannot be removed: failed, file untouched
printf '%s\n' "$SEED_A" > "$KSEED"
chmod 500 "$MIG/config"
if [ "$(id -u)" = "0" ]; then
    echo "skip: kernel seed copy: removal failure case needs an unprivileged user (root ignores modes)"
else
    assert_eq "failed remove" "$(remove_kernel_seed_copy "$KSEED" "$SEED_A" 2>/dev/null || true)" \
        "kernel seed copy: an unremovable file fails the cleanup"
    remove_kernel_seed_copy "$KSEED" "$SEED_A" > /dev/null 2>&1 \
        && { echo "FAIL: kernel seed copy: failed remove must return 1" >&2; FAILURES=$((FAILURES+1)); }
    [ -s "$KSEED" ] || { echo "FAIL: kernel seed copy: file touched by a failed remove" >&2; FAILURES=$((FAILURES+1)); }
fi
chmod 700 "$MIG/config"
rm -f "$KSEED"

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
