#!/usr/bin/env bash
# Render-and-parse check for the kernel config JSON that the add-on's run.sh
# and the Docker sidecar's entrypoint.sh write, plus the fleet-peers wiring
# that ties the kernel's `GET /api/fleet` to event_mqtt_bridge.
#
# Why this exists: both scripts write witness_config.json from a bash
# heredoc, and the kernel parses it with serde `deny_unknown_fields`
# (src/config.rs: ApiConfigFile, WitnessApiConfigFile, WitnessdConfigFile).
# A mistyped key there is not a warning — witness_api or witnessd exits at
# startup, and in frigate mode run.sh keeps going to `exec frigate_bridge`,
# so the add-on looks "running" with a dead API. Nothing else renders these
# heredocs before a real container boots, so this test does: it lifts each
# heredoc out of the script, expands it with bash exactly as the script
# would (the same `$VAR` references, the script's own fixed values, `set -u`
# so a variable this test does not know about fails loudly), and parses the
# result with jq against the key sets the Rust parser accepts.
#
# The fleet-peers half pins the one invariant that makes the roll-call work:
# the kernel's `api.fleet_peers_path` and the bridge's `--fleet-peers-path`
# must name the SAME file, so both seams are required to read the one shell
# variable, and that file's directory must exist before the bridge's first
# write (PeerSummaryFile::write_atomic never creates a parent directory).
#
# Run directly: bash privacy_witness_kernel/tests/test_config_render.sh
# Mutation runs may point TEST_RUN_SH / TEST_ENTRYPOINT_SH at edited copies.
#
# The needles below are the scripts' literal source text, so their $VAR must
# stay unexpanded: single quotes are the point, not a mistake.
# shellcheck disable=SC2016
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
RUN_SH="${TEST_RUN_SH:-$HERE/../run.sh}"
ENTRYPOINT_SH="${TEST_ENTRYPOINT_SH:-$HERE/../../docker/sidecar/entrypoint.sh}"

for f in "$RUN_SH" "$ENTRYPOINT_SH"; do
    [ -f "$f" ] || { echo "missing script under test: $f" >&2; exit 2; }
done
command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

FAILURES=0
ok()   { echo "ok: $*"; }
fail() { echo "FAIL: $*" >&2; FAILURES=$((FAILURES + 1)); }
assert_eq() {
    local expected="$1" actual="$2" label="$3"
    if [ "$expected" = "$actual" ]; then
        ok "$label"
    else
        fail "$label — expected '$expected', got '$actual'"
    fi
}

# The key sets src/config.rs accepts. deny_unknown_fields means a key
# outside these is a startup error, so a new key added to a heredoc must be
# added to the Rust struct first — and then here.
API_KEYS=(addr token_path rate_limit_per_minute fleet_peers_path viewer_token_path)
WITNESS_API_TOP_KEYS=(db_path ruleset_id api zones retention)
WITNESSD_TOP_KEYS=(db_path ruleset_id api ingest rtsp file v4l2 esp32 detect
                   zones retention health storage clock storage_health)

# code_lines FILE — the script without comment lines, so a needle count
# is satisfied by code only, never by prose about the code.
code_lines() { grep -v -E '^[[:space:]]*#' "$1"; }

# count_needle FILE NEEDLE — fixed-string occurrences on code lines.
count_needle() { code_lines "$1" | { grep -c -F -- "$2" || true; }; }

# fixed_value FILE NAME — the value of a top-level NAME="literal" assignment.
fixed_value() { sed -n -E "s/^${2}=\"([^\"\$]+)\"[[:space:]]*$/\1/p" "$1" | head -n1; }

# heredoc_body FILE N — the body of the N-th `cat > "$CONFIG_FILE" << EOF`
# heredoc in FILE, without its terminator. run.sh has two (frigate mode's
# write_frigate_api_config, then the standalone block); entrypoint.sh one.
heredoc_body() {
    awk -v want="$2" '
        /^[[:space:]]*cat > "\$CONFIG_FILE" <<[[:space:]]?EOF[[:space:]]*$/ {
            n++
            if (n == want) { grab = 1; next }
        }
        grab && /^EOF$/ { grab = 0; done = 1 }
        grab { print }
        END { if (!done) exit 1 }
    ' "$1"
}

# shell_function FILE NAME — the source of the top-level `NAME() {` ... `}`
# function in FILE, so a test can run the script's own mapping instead of
# re-deriving it (the heredoc test above proves the render; this proves
# what feeds it).
shell_function() {
    awk -v name="$2" '
        $0 == name "() {" { grab = 1 }
        grab { print }
        grab && /^}$/ { grab = 0; done = 1 }
        END { if (!done) exit 1 }
    ' "$1"
}

# run_function FUNC_FILE NAME VAR=VALUE... — call a lifted function in an
# emptied environment (as `render` does) with a `die` stub that exits 1,
# and print the bind variables it set: "<API_ADDR> <WITNESS_API_ALLOW_INSECURE|unset>".
run_function() {
    local func="$1" name="$2"
    shift 2
    {
        printf 'die() { echo "ERROR: $*" >&2; exit 1; }\n'
        cat "$func"
        printf '%s\n' "$name"
        printf 'printf "%%s %%s\\n" "$API_ADDR" "${WITNESS_API_ALLOW_INSECURE:-unset}"\n'
    } > "$TMP/run_function.sh"
    env -i PATH="$PATH" "$@" bash -u "$TMP/run_function.sh"
}

# render BODY_FILE OUT_FILE VAR=VALUE... — expand a lifted heredoc the way
# bash does inside the script. The environment is emptied first so only the
# variables named here exist, and `set -u` turns any other reference in the
# heredoc into a hard failure instead of an empty string.
render() {
    local body="$1" out="$2"
    shift 2
    {
        printf 'cat <<EOF\n'
        cat "$body"
        printf 'EOF\n'
    } > "$TMP/render.sh"
    env -i PATH="$PATH" "$@" bash -u "$TMP/render.sh" > "$out"
}

# assert_keys_accepted JSON JQ_PATH LABEL ALLOWED... — every key at JQ_PATH
# is one the Rust parser accepts.
assert_keys_accepted() {
    local json="$1" path="$2" label="$3"
    shift 3
    local allowed extra
    allowed=$(printf '%s\n' "$@" | jq -R . | jq -sc .)
    extra=$(jq -c --argjson allowed "$allowed" "[$path | keys[]] - \$allowed" "$json")
    if [ "$extra" = "[]" ]; then
        ok "$label: every key under $path is one the kernel accepts"
    else
        fail "$label: keys the kernel would refuse under $path: $extra"
    fi
}

# ============================================================================
# Add-on (privacy_witness_kernel/run.sh)
# ============================================================================
ADDON_PEERS=$(fixed_value "$RUN_SH" FLEET_PEERS_FILE)
if [ -n "$ADDON_PEERS" ]; then
    ok "run.sh: FLEET_PEERS_FILE is a fixed path ($ADDON_PEERS)"
else
    fail "run.sh: FLEET_PEERS_FILE is not assigned a fixed path"
fi
assert_eq "/config" "$(dirname "${ADDON_PEERS:-/missing/x}")" \
    "run.sh: the peers file lives in /config (exists at boot, rw-mapped, in HA backups)"

# The needles: one api fragment behind the publisher switch, carried by both
# kernel config blocks; one bridge argv element; one variable. The fragment is
# empty when publishing is off, so the kernel is never pointed at a file no
# bridge keeps (review round on #1686: it would have served rows a past
# bridge pinned, while the log said the roll-call lists the kernel alone).
assert_eq 1 "$(count_needle "$RUN_SH" 'FLEET_PEERS_API=", \"fleet_peers_path\": \"$FLEET_PEERS_FILE\""')" \
    "run.sh: the api fragment names fleet_peers_path from FLEET_PEERS_FILE, once, behind the publisher switch"
assert_eq 2 "$(count_needle "$RUN_SH" '"token_path": "$TOKEN_FILE"$FLEET_PEERS_API')" \
    "run.sh: both kernel config blocks carry the fragment"
assert_eq 0 "$(count_needle "$RUN_SH" '"fleet_peers_path"')" \
    "run.sh: no block names fleet_peers_path outside the fragment (which spells it escaped)"
assert_eq 1 "$(count_needle "$RUN_SH" '--fleet-peers-path')" \
    "run.sh: the bridge argv names the flag exactly once (MQTT_CMD_ARRAY serves both modes)"
assert_eq 1 "$(count_needle "$RUN_SH" '--fleet-peers-path "$FLEET_PEERS_FILE"')" \
    "run.sh: the argv element reads the same variable"

ADDON_DB=$(fixed_value "$RUN_SH" DB_PATH)
ADDON_TOKEN=$(fixed_value "$RUN_SH" TOKEN_FILE)
ADDON_ADDR=$(fixed_value "$RUN_SH" API_BIND_ADDR)
for v in ADDON_DB ADDON_TOKEN ADDON_ADDR; do
    [ -n "${!v}" ] || fail "run.sh: could not read the fixed value behind $v"
done

# The fragment as run.sh builds it with publishing on, and as it is with it off.
ADDON_API_ON=", \"fleet_peers_path\": \"$ADDON_PEERS\""

# assert_off_block JSON LABEL TOPKEYS... — a block rendered with the fragment
# empty is still valid, still made of accepted keys, and names no peers file.
assert_off_block() {
    local json="$1" label="$2"
    shift 2
    if jq -e . "$json" > /dev/null 2>&1; then
        ok "$label (publishing off): renders as JSON"
        assert_keys_accepted "$json" '.' "$label (publishing off)" "$@"
        assert_keys_accepted "$json" '.api' "$label (publishing off, ApiConfigFile)" "${API_KEYS[@]}"
        assert_eq "false" "$(jq -r '.api | has("fleet_peers_path")' "$json")" \
            "$label (publishing off): the kernel is not pointed at a peers file"
    else
        fail "$label (publishing off): rendered text is not JSON: $(cat "$json")"
    fi
}

# ---- block 1: frigate mode → witness_api ----------------------------------
if heredoc_body "$RUN_SH" 1 > "$TMP/addon_frigate.body"; then
    render "$TMP/addon_frigate.body" "$TMP/addon_frigate_off.json" \
        DB_PATH="$ADDON_DB" API_BIND_ADDR="$ADDON_ADDR" TOKEN_FILE="$ADDON_TOKEN" \
        FLEET_PEERS_API= RETENTION_SECS=604800
    assert_off_block "$TMP/addon_frigate_off.json" "run.sh frigate block" "${WITNESS_API_TOP_KEYS[@]}"
    render "$TMP/addon_frigate.body" "$TMP/addon_frigate.json" \
        DB_PATH="$ADDON_DB" API_BIND_ADDR="$ADDON_ADDR" TOKEN_FILE="$ADDON_TOKEN" \
        FLEET_PEERS_API="$ADDON_API_ON" RETENTION_SECS=604800
    if jq -e . "$TMP/addon_frigate.json" > /dev/null 2>&1; then
        ok "run.sh frigate block: renders as JSON"
        assert_keys_accepted "$TMP/addon_frigate.json" '.' \
            'run.sh frigate block (WitnessApiConfigFile)' "${WITNESS_API_TOP_KEYS[@]}"
        assert_keys_accepted "$TMP/addon_frigate.json" '.api' \
            'run.sh frigate block (ApiConfigFile)' "${API_KEYS[@]}"
        assert_eq "$ADDON_PEERS" "$(jq -r '.api.fleet_peers_path' "$TMP/addon_frigate.json")" \
            "run.sh frigate block: api.fleet_peers_path renders to the fixed path"
        assert_eq "$ADDON_TOKEN" "$(jq -r '.api.token_path' "$TMP/addon_frigate.json")" \
            "run.sh frigate block: api.token_path still renders"
        assert_eq "ruleset:frigate_v1" "$(jq -r '.ruleset_id' "$TMP/addon_frigate.json")" \
            "run.sh frigate block: ruleset_id is the frigate ruleset"
        assert_eq "604800" "$(jq -r '.retention.seconds' "$TMP/addon_frigate.json")" \
            "run.sh frigate block: retention.seconds renders as a number"
    else
        fail "run.sh frigate block: rendered text is not JSON: $(cat "$TMP/addon_frigate.json")"
    fi
else
    fail "run.sh: could not find the frigate-mode config heredoc"
fi

# ---- block 2: standalone mode → witnessd ---------------------------------
if heredoc_body "$RUN_SH" 2 > "$TMP/addon_standalone.body"; then
    render "$TMP/addon_standalone.body" "$TMP/addon_standalone_off.json" \
        DB_PATH="$ADDON_DB" API_BIND_ADDR="$ADDON_ADDR" TOKEN_FILE="$ADDON_TOKEN" \
        FLEET_PEERS_API= RETENTION_SECS=604800 \
        CAMERA_URL="rtsp://user:secret@cam.local:554/stream" CAMERA_FPS=10 \
        CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_ZONE="zone:front_boundary"
    assert_off_block "$TMP/addon_standalone_off.json" "run.sh standalone block" "${WITNESSD_TOP_KEYS[@]}"
    render "$TMP/addon_standalone.body" "$TMP/addon_standalone.json" \
        DB_PATH="$ADDON_DB" API_BIND_ADDR="$ADDON_ADDR" TOKEN_FILE="$ADDON_TOKEN" \
        FLEET_PEERS_API="$ADDON_API_ON" RETENTION_SECS=604800 \
        CAMERA_URL="rtsp://user:secret@cam.local:554/stream" CAMERA_FPS=10 \
        CAMERA_WIDTH=640 CAMERA_HEIGHT=480 CAMERA_ZONE="zone:front_boundary"
    if jq -e . "$TMP/addon_standalone.json" > /dev/null 2>&1; then
        ok "run.sh standalone block: renders as JSON"
        assert_keys_accepted "$TMP/addon_standalone.json" '.' \
            'run.sh standalone block (WitnessdConfigFile)' "${WITNESSD_TOP_KEYS[@]}"
        assert_keys_accepted "$TMP/addon_standalone.json" '.api' \
            'run.sh standalone block (ApiConfigFile)' "${API_KEYS[@]}"
        assert_eq "$ADDON_PEERS" "$(jq -r '.api.fleet_peers_path' "$TMP/addon_standalone.json")" \
            "run.sh standalone block: api.fleet_peers_path renders to the same fixed path"
        assert_eq "ruleset:homeassistant_v1" "$(jq -r '.ruleset_id' "$TMP/addon_standalone.json")" \
            "run.sh standalone block: ruleset_id is the standalone ruleset"
        assert_eq "true" "$(jq -r '(.rtsp.target_fps == 10) and (.rtsp.width == 640) and (.rtsp.height == 480)' "$TMP/addon_standalone.json")" \
            "run.sh standalone block: rtsp numbers render as numbers"
        assert_eq "zone:front_boundary" "$(jq -r '.zones.sensitive[0]' "$TMP/addon_standalone.json")" \
            "run.sh standalone block: the zone lands in zones.sensitive"
    else
        fail "run.sh standalone block: rendered text is not JSON: $(cat "$TMP/addon_standalone.json")"
    fi
else
    fail "run.sh: could not find the standalone config heredoc"
fi

# Both blocks must point the kernel at the file the bridge writes: same path.
if [ -s "$TMP/addon_frigate.json" ] && [ -s "$TMP/addon_standalone.json" ]; then
    SAME=$(jq -rs '(.[0].api.fleet_peers_path // "") as $a
                   | (.[1].api.fleet_peers_path // "") as $b
                   | if $a != "" and $a == $b then $a else "DIFFER: \($a) vs \($b)" end' \
        "$TMP/addon_frigate.json" "$TMP/addon_standalone.json")
    assert_eq "$ADDON_PEERS" "$SAME" \
        "run.sh: frigate and standalone blocks name one and the same peers file"
fi

# ============================================================================
# Docker sidecar (docker/sidecar/entrypoint.sh)
# ============================================================================
# The sidecar runs witness_api and event_mqtt_bridge in ONE container that
# owns /data, so the peers file sits in /data beside the database and the
# token — no second volume, and the directory exists before the first write.
assert_eq 1 "$(count_needle "$ENTRYPOINT_SH" 'FLEET_PEERS_FILE="$DATA_DIR/fleet_peers.json"')" \
    "entrypoint.sh: FLEET_PEERS_FILE lives in the shared \$DATA_DIR"
assert_eq 1 "$(count_needle "$ENTRYPOINT_SH" 'fleet_peers_api=", \"fleet_peers_path\": \"$FLEET_PEERS_FILE\""')" \
    "entrypoint.sh: the api fragment names fleet_peers_path from FLEET_PEERS_FILE, once, behind the publish switch"
assert_eq 1 "$(count_needle "$ENTRYPOINT_SH" '"token_path": "$TOKEN_FILE"$fleet_peers_api')" \
    "entrypoint.sh: the kernel config block carries the fragment"
assert_eq 0 "$(count_needle "$ENTRYPOINT_SH" '"fleet_peers_path"')" \
    "entrypoint.sh: nothing names fleet_peers_path outside the fragment (which spells it escaped)"
assert_eq 1 "$(count_needle "$ENTRYPOINT_SH" '--fleet-peers-path "$FLEET_PEERS_FILE"')" \
    "entrypoint.sh: pub_args names the flag once, from the same variable"

# ---- the bind switch: SECURACV_API_BIND → API_ADDR + the cleartext opt-in --
# The kernel refuses a non-loopback bind without WITNESS_API_ALLOW_INSECURE=1
# (src/api/mod.rs), so the entrypoint must flip the address and the export
# together, in one function, and export nothing in loopback mode. The
# function is lifted from the script and run under each mode; the heredoc
# below is then rendered with the address each mode produced.
SIDECAR_ADDR_LOOPBACK=""
SIDECAR_ADDR_ALL=""
if shell_function "$ENTRYPOINT_SH" configure_api_bind > "$TMP/configure_api_bind.sh"; then
    ok "entrypoint.sh: configure_api_bind is a top-level function (liftable, so this test runs the real mapping)"
    assert_eq 1 "$(count_needle "$ENTRYPOINT_SH" 'export WITNESS_API_ALLOW_INSECURE=1')" \
        "entrypoint.sh: the cleartext acknowledgment is exported in exactly one place"
    assert_eq 1 "$(count_needle "$TMP/configure_api_bind.sh" 'export WITNESS_API_ALLOW_INSECURE=1')" \
        "entrypoint.sh: that place is inside configure_api_bind, beside the address it acknowledges"
    assert_eq 1 "$(count_needle "$ENTRYPOINT_SH" '"addr": "$API_ADDR"')" \
        "entrypoint.sh: the kernel config block reads the address from API_ADDR"
    assert_eq 0 "$(count_needle "$ENTRYPOINT_SH" '"addr": "127.0.0.1:8799"')" \
        "entrypoint.sh: no block hard-codes the loopback address any more"

    # unset → loopback, nothing exported (the default posture, unchanged)
    if out=$(run_function "$TMP/configure_api_bind.sh" configure_api_bind); then
        assert_eq "127.0.0.1:8799 unset" "$out" \
            "entrypoint.sh: SECURACV_API_BIND unset → 127.0.0.1:8799, WITNESS_API_ALLOW_INSECURE not exported"
        SIDECAR_ADDR_LOOPBACK="${out%% *}"
    else
        fail "entrypoint.sh: configure_api_bind failed with SECURACV_API_BIND unset"
    fi
    # loopback spelled out → the same
    if out=$(run_function "$TMP/configure_api_bind.sh" configure_api_bind SECURACV_API_BIND=loopback); then
        assert_eq "127.0.0.1:8799 unset" "$out" \
            "entrypoint.sh: SECURACV_API_BIND=loopback → 127.0.0.1:8799, nothing exported"
    else
        fail "entrypoint.sh: configure_api_bind failed with SECURACV_API_BIND=loopback"
    fi
    # all → every interface AND the acknowledgment, together
    if out=$(run_function "$TMP/configure_api_bind.sh" configure_api_bind SECURACV_API_BIND=all); then
        assert_eq "0.0.0.0:8799 1" "$out" \
            "entrypoint.sh: SECURACV_API_BIND=all → 0.0.0.0:8799 AND WITNESS_API_ALLOW_INSECURE=1 exported"
        SIDECAR_ADDR_ALL="${out%% *}"
    else
        fail "entrypoint.sh: configure_api_bind failed with SECURACV_API_BIND=all"
    fi
    # anything else → refused, so a typo never silently means loopback
    if run_function "$TMP/configure_api_bind.sh" configure_api_bind SECURACV_API_BIND=0.0.0.0 > "$TMP/bogus.out" 2>&1; then
        fail "entrypoint.sh: SECURACV_API_BIND=0.0.0.0 (not a mode) was accepted: $(cat "$TMP/bogus.out")"
    else
        ok "entrypoint.sh: SECURACV_API_BIND=0.0.0.0 (an address, not a mode) is refused"
    fi
else
    fail "entrypoint.sh: could not lift configure_api_bind (the bind switch must be a top-level function)"
fi

if heredoc_body "$ENTRYPOINT_SH" 1 > "$TMP/sidecar.body"; then
    render "$TMP/sidecar.body" "$TMP/sidecar_off.json" \
        DB_PATH=/data/witness.db TOKEN_FILE=/data/api_token API_ADDR="${SIDECAR_ADDR_LOOPBACK:-127.0.0.1:8799}" \
        fleet_peers_api= retention_secs=604800
    assert_off_block "$TMP/sidecar_off.json" "entrypoint.sh block" "${WITNESS_API_TOP_KEYS[@]}"
    render "$TMP/sidecar.body" "$TMP/sidecar.json" \
        DB_PATH=/data/witness.db TOKEN_FILE=/data/api_token API_ADDR="${SIDECAR_ADDR_LOOPBACK:-127.0.0.1:8799}" \
        fleet_peers_api=", \"fleet_peers_path\": \"/data/fleet_peers.json\"" retention_secs=604800
    if jq -e . "$TMP/sidecar.json" > /dev/null 2>&1; then
        ok "entrypoint.sh block: renders as JSON"
        assert_keys_accepted "$TMP/sidecar.json" '.' \
            'entrypoint.sh block (WitnessApiConfigFile)' "${WITNESS_API_TOP_KEYS[@]}"
        assert_keys_accepted "$TMP/sidecar.json" '.api' \
            'entrypoint.sh block (ApiConfigFile)' "${API_KEYS[@]}"
        assert_eq "/data/fleet_peers.json" "$(jq -r '.api.fleet_peers_path' "$TMP/sidecar.json")" \
            "entrypoint.sh block: api.fleet_peers_path renders under /data"
        assert_eq "127.0.0.1:8799" "$(jq -r '.api.addr' "$TMP/sidecar.json")" \
            "entrypoint.sh block (SECURACV_API_BIND unset): the API binds loopback — the default is unchanged"
    else
        fail "entrypoint.sh block: rendered text is not JSON: $(cat "$TMP/sidecar.json")"
    fi
    # The same block in `all` mode: only the address moves.
    render "$TMP/sidecar.body" "$TMP/sidecar_all.json" \
        DB_PATH=/data/witness.db TOKEN_FILE=/data/api_token API_ADDR="${SIDECAR_ADDR_ALL:-0.0.0.0:8799}" \
        fleet_peers_api=", \"fleet_peers_path\": \"/data/fleet_peers.json\"" retention_secs=604800
    if jq -e . "$TMP/sidecar_all.json" > /dev/null 2>&1; then
        assert_keys_accepted "$TMP/sidecar_all.json" '.api' \
            'entrypoint.sh block (SECURACV_API_BIND=all, ApiConfigFile)' "${API_KEYS[@]}"
        assert_eq "0.0.0.0:8799" "$(jq -r '.api.addr' "$TMP/sidecar_all.json")" \
            "entrypoint.sh block (SECURACV_API_BIND=all): the API binds every interface on the same port"
        assert_eq "true" "$(jq -rs '(.[0] | del(.api.addr)) == (.[1] | del(.api.addr))' "$TMP/sidecar.json" "$TMP/sidecar_all.json")" \
            "entrypoint.sh block (SECURACV_API_BIND=all): nothing but api.addr differs from loopback mode"
    else
        fail "entrypoint.sh block (SECURACV_API_BIND=all): rendered text is not JSON: $(cat "$TMP/sidecar_all.json")"
    fi
else
    fail "entrypoint.sh: could not find the kernel config heredoc"
fi

# ----------------------------------------------------------------------------
if [ "$FAILURES" -gt 0 ]; then
    echo "$FAILURES config render check(s) failed" >&2
    exit 1
fi
echo "all config render checks passed"
