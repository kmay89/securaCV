#!/usr/bin/env bash
# Pure helpers for the add-on entry script (run.sh).
#
# No bashio dependency: everything here takes plain arguments and touches
# only the paths it is given, so the resolution logic can be unit-tested
# outside the add-on container (tests/test_run_lib.sh) and shellchecked.

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

    # The add-on runtime image has python3 but not the openssl CLI (the
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

# compose_discovery_payload HOST [PORT]
# Build the JSON body for the Supervisor /discovery announcement that lets
# the SecuraCV integration show a zero-click discovered card. jq handles
# the quoting so an unusual hostname can't corrupt the JSON. Pure: no
# network, no bashio — unit-tested in tests/test_run_lib.sh.
compose_discovery_payload() {
    local host="${1:?compose_discovery_payload: host required}"
    local port="${2:-8799}"
    jq -cn --arg host "$host" --argjson port "$port" \
        '{service: "securacv", config: {host: $host, port: $port}}'
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
