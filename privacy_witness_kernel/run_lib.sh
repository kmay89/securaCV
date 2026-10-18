#!/usr/bin/env bash
# Pure helpers for the app's setup script (run.sh) and its s6 services.
#
# No bashio dependency: everything here takes plain arguments and touches
# only the paths it is given, so the resolution logic can be unit-tested
# outside the app container (tests/test_run_lib.sh) and shellchecked.

# resolve_value EXPLICIT DISCOVERED FALLBACK
# Echo the first non-empty value. Used for MQTT broker settings where an
# explicitly configured option always wins over Supervisor service
# discovery, which in turn wins over the historical default.
resolve_value() {
    if [ -n "${1:-}" ]; then
        echo "$1"
        return
    fi
    if [ -n "${2:-}" ]; then
        echo "$2"
        return
    fi
    echo "${3:-}"
}

# resolve_device_key_seed OPTION_VALUE KEY_FILE
# Resolution order: explicit option (unless empty or the "devkey:mvp"
# placeholder) → persisted key file (written by the setup wizard) → a
# freshly generated 64-hex seed persisted to KEY_FILE with 0600 perms.
# Always echoes the resolved seed; never fails the caller for a missing
# key, so a fresh install starts without manual openssl steps.
resolve_device_key_seed() {
    local option_value="${1:-}"
    local key_file="${2:?resolve_device_key_seed: key file path required}"

    if [ -n "$option_value" ] && [ "$option_value" != "devkey:mvp" ]; then
        echo "$option_value"
        return
    fi

    if [ -s "$key_file" ]; then
        head -n1 "$key_file" | tr -d '[:space:]'
        return
    fi

    # The app runtime image has python3 but not the openssl CLI (the
    # libcrypto pin forbids the metapackage), so try openssl first for
    # bare-shell use and fall back to python3, then od.
    local seed
    if ! seed=$(openssl rand -hex 32 2>/dev/null); then
        if ! seed=$(python3 -c 'import secrets; print(secrets.token_hex(32))' 2>/dev/null); then
            seed=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n')
        fi
    fi

    mkdir -p "$(dirname "$key_file")"
    (
        umask 077
        printf '%s\n' "$seed" > "$key_file"
    )
    chmod 600 "$key_file"
    echo "$seed"
}

# ha_device_id SEED
# The public Home Assistant device id of this kernel. It lands in retained
# MQTT discovery topics and in the Supervisor discovery payload, both of
# which other clients can read, so it carries ZERO bits of the seed: the
# first 8 hex of sha256 over the seed under a domain-separation prefix.
# The same derivation as the Docker sidecar (docker/sidecar/entrypoint.sh),
# event_mqtt_bridge's own fallback and the kernel's GET /api/identity
# (src/lib.rs ha_device_id), so every deployment names a seed one way.
ha_device_id() {
    local seed="${1:?ha_device_id: seed required}"
    printf 'pwk_%s' "$(printf '%s' "securacv:ha-device-id:v1:${seed}" | sha256sum | cut -c1-8)"
}

# compose_discovery_payload HOST [PORT] [TOKEN_FILE] [TOPIC_PREFIX] [DEVICE_ID]
# Build the JSON body for the Supervisor /discovery announcement that lets
# the SecuraCV integration show a zero-click discovered card (contract C6):
#   {"service":"securacv","config":{host, port, token_file,
#                                   mqtt_topic_prefix, device_id}}
# Keys in that order; `port` is a JSON integer, everything else a string.
# `token_file` is the PATH of the rotating capability token (never a token
# value), `mqtt_topic_prefix` is the bridge's state prefix (always present,
# even when publishing is off — the integration then knows no bridge
# publishes and relies on HTTP), and `device_id` is the kernel's public
# id (ha_device_id). An empty DEVICE_ID omits the key rather than sending
# an empty id, so the integration's legacy (no id) path stays intact. jq
# handles the quoting so an unusual hostname can't corrupt the JSON. Pure:
# no network, no bashio — unit-tested in tests/test_run_lib.sh.
compose_discovery_payload() {
    local host="${1:?compose_discovery_payload: host required}"
    local port="${2:-8799}"
    local token_file="${3:-/config/api_token}"
    local topic_prefix="${4:-witness}"
    local device_id="${5:-}"
    jq -cn --arg host "$host" --argjson port "$port" \
        --arg token_file "$token_file" --arg prefix "$topic_prefix" \
        --arg device_id "$device_id" \
        '{service: "securacv",
          config: ({host: $host, port: $port, token_file: $token_file,
                    mqtt_topic_prefix: $prefix}
                   + (if $device_id == "" then {} else {device_id: $device_id} end))}'
}

# resolve_frigate_topic LEGACY_TOPIC TOPIC_PREFIX
# The historical `frigate.mqtt_topic` option (a full topic) still wins when
# set, so saved configs keep working; otherwise the topic is derived from
# the `frigate.topic_prefix` option (default "frigate").
resolve_frigate_topic() {
    local legacy_topic="${1:-}"
    local topic_prefix="${2:-frigate}"

    if [ -n "$legacy_topic" ]; then
        echo "$legacy_topic"
        return
    fi
    echo "${topic_prefix}/events"
}

# wait_for_kernel_health URL [TRIES] [SLEEP_SECS]
# Poll the kernel's unauthenticated /health until it answers 200 or the
# tries run out. Returns 0 when healthy, 1 when it never answered.
wait_for_kernel_health() {
    local url="${1:?wait_for_kernel_health: url required}"
    local tries="${2:-60}" pause="${3:-2}"
    local _i
    for _i in $(seq 1 "$tries"); do
        if curl -sf "$url" > /dev/null 2>&1; then
            return 0
        fi
        sleep "$pause"
    done
    return 1
}

# install_blueprints SRC_DIR DEST_DIR
# Place the blueprints the image carries (SRC_DIR/*.yaml) under DEST_DIR
# (/config/blueprints/automation/securacv) without ever overwriting a copy
# the user edited. DEST_DIR/.securacv_app_sha256sums records the sha256 of
# every file THIS app installed; a file on disk is replaced only when it is
# missing, or when it still matches that record (so it is the app's own,
# unedited copy) and the shipped copy differs. A file with no record (placed
# by scripts/install.sh or by hand) or one that no longer matches its record
# is left alone and reported as kept. Prints one line per blueprint:
#   installed|updated|unchanged|kept <name>
# Pure: paths only, no bashio — unit-tested in tests/test_run_lib.sh.
install_blueprints() {
    local src="${1:?install_blueprints: source dir required}"
    local dest="${2:?install_blueprints: destination dir required}"
    local record="$dest/.securacv_app_sha256sums"
    local f name shipped_sum disk_sum recorded_sum verdict
    mkdir -p "$dest"
    [ -f "$record" ] || : > "$record"
    for f in "$src"/*.yaml; do
        [ -f "$f" ] || continue
        name=$(basename "$f")
        shipped_sum=$(sha256sum "$f" | cut -d' ' -f1)
        recorded_sum=$(awk -v n="$name" '$2 == n { print $1; exit }' "$record")
        if [ ! -f "$dest/$name" ]; then
            verdict="installed"
        else
            disk_sum=$(sha256sum "$dest/$name" | cut -d' ' -f1)
            if [ "$disk_sum" = "$shipped_sum" ]; then
                verdict="unchanged"
            elif [ -n "$recorded_sum" ] && [ "$disk_sum" = "$recorded_sum" ]; then
                verdict="updated"
            else
                verdict="kept"
            fi
        fi
        if [ "$verdict" = "installed" ] || [ "$verdict" = "updated" ]; then
            cp "$f" "$dest/$name.tmp.$$"
            mv -f "$dest/$name.tmp.$$" "$dest/$name"
        fi
        if [ "$verdict" != "kept" ]; then
            # Record (or refresh) the hash of the copy the app owns.
            awk -v n="$name" '$2 != n' "$record" > "$record.tmp.$$"
            printf '%s  %s\n' "$shipped_sum" "$name" >> "$record.tmp.$$"
            mv -f "$record.tmp.$$" "$record"
        fi
        echo "$verdict $name"
    done
}
