#!/usr/bin/env bash
# SecuraCV Frigate sidecar entrypoint.
#
# Supervises the three daemons that turn Frigate MQTT events into a sealed,
# privacy-preserving witness log:
#   witness_api        — event API + capability token (loopback :8799)
#   frigate_bridge     — Frigate MQTT events → sealed log
#   event_mqtt_bridge  — sealed log → HA MQTT Discovery sensors (optional)
#
# Environment contract (only FRIGATE_MQTT_HOST is required):
#   FRIGATE_MQTT_HOST       broker hostname Frigate publishes to   (required*)
#   FRIGATE_MQTT_PORT       broker port                            (1883)
#   MQTT_BROKER_ADDR        full host:port override; replaces the two above
#   MQTT_USERNAME           broker auth                            (none)
#   MQTT_PASSWORD           broker auth                            (none)
#   FRIGATE_TOPIC_PREFIX    Frigate's mqtt.topic_prefix            (frigate)
#   FRIGATE_MQTT_TOPIC      full topic override                    (<prefix>/events)
#   FRIGATE_ENABLE_REVIEWS  also ingest <prefix>/reviews (0.14+)   (false)
#   VERIFY_INTERVAL_SECS    auto-verification cadence, 0 disables  (86400)
#   FRIGATE_CAMERAS         comma-separated camera allowlist       (all)
#   FRIGATE_LABELS          comma-separated label allowlist        (bridge default)
#   FRIGATE_MIN_CONFIDENCE  minimum detection confidence           (0.5)
#   RETENTION_DAYS          sealed-event retention                 (7)
#   TIME_BUCKET_MINUTES     timestamp coarsening bucket            (10)
#   SECURACV_PUBLISH        publish HA Discovery sensors           (true)
#   HA_DISCOVERY_PREFIX     HA discovery prefix                    (homeassistant)
#   MQTT_TOPIC_PREFIX       state topic prefix                     (witness)
#   POLL_INTERVAL           publisher poll seconds                 (30)
#   BROKER_WAIT_SECS        max seconds to wait for the broker     (30)
#   DEVICE_KEY_SEED         64-hex signing seed; if unset, read from
#                           /run/secrets/device_key_seed, then /data/device_key,
#                           else auto-generated and persisted (0600)
#
# Subcommands:
#   entrypoint.sh           run the sidecar (default)
#   entrypoint.sh doctor    diagnose broker/Frigate/sealed-log health
set -euo pipefail

DATA_DIR="${DATA_DIR:-/data}"
DB_PATH="$DATA_DIR/witness.db"
KEY_FILE="$DATA_DIR/device_key"
TOKEN_FILE="$DATA_DIR/api_token"
CONFIG_FILE="$DATA_DIR/witness_config.json"

# Logs go to stderr: several helpers are called inside $(...) command
# substitutions, where anything on stdout would pollute the captured value.
log() { echo "[securacv] $*" >&2; }
die() { echo "[securacv] ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Configuration resolution
# ---------------------------------------------------------------------------

resolve_broker_addr() {
    if [ -n "${MQTT_BROKER_ADDR:-}" ]; then
        echo "$MQTT_BROKER_ADDR"
        return
    fi
    if [ -z "${FRIGATE_MQTT_HOST:-}" ]; then
        die "FRIGATE_MQTT_HOST is not set.
  Point it at the MQTT broker Frigate publishes to (the 'mqtt: host:' value
  in your frigate config), e.g.  FRIGATE_MQTT_HOST=mosquitto
  Or set MQTT_BROKER_ADDR=host:port directly."
    fi
    echo "${FRIGATE_MQTT_HOST}:${FRIGATE_MQTT_PORT:-1883}"
}

resolve_device_key_seed() {
    if [ -n "${DEVICE_KEY_SEED:-}" ]; then
        echo "$DEVICE_KEY_SEED"
        return
    fi
    if [ -s /run/secrets/device_key_seed ]; then
        head -n1 /run/secrets/device_key_seed | tr -d '[:space:]'
        return
    fi
    if [ -s "$KEY_FILE" ]; then
        head -n1 "$KEY_FILE" | tr -d '[:space:]'
        return
    fi
    local seed
    if ! seed=$(head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n'); then
        die "failed to generate a device key seed"
    fi
    (
        umask 077
        printf '%s\n' "$seed" > "$KEY_FILE"
    )
    chmod 600 "$KEY_FILE"
    log "Generated a new device key seed and persisted it to $KEY_FILE"
    log "BACK THIS FILE UP: without it, sealed-log signatures cannot be re-derived."
    echo "$seed"
}

tcp_check() {
    local host="$1" port="$2"
    (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null
}

split_addr_host() { echo "${1%:*}"; }
split_addr_port() { echo "${1##*:}"; }

# ---------------------------------------------------------------------------
# doctor — diagnose the integration without changing anything
# ---------------------------------------------------------------------------

doctor() {
    local failures=0
    local addr host port topic prefix
    addr=$(resolve_broker_addr)
    host=$(split_addr_host "$addr")
    port=$(split_addr_port "$addr")
    prefix="${FRIGATE_TOPIC_PREFIX:-frigate}"
    topic="${FRIGATE_MQTT_TOPIC:-${prefix}/events}"
    local listen_secs="${DOCTOR_LISTEN_SECS:-30}"

    echo "SecuraCV sidecar doctor"
    echo "  broker: $addr   frigate topic: $topic"
    echo

    # 1) Broker TCP reachability
    if tcp_check "$host" "$port"; then
        echo "✓ Broker $addr is reachable"
    else
        echo "✗ Cannot reach $addr (TCP)"
        echo "  - Is the broker container running and on the same docker network?"
        echo "  - Does FRIGATE_MQTT_HOST match the broker's service name?"
        failures=$((failures + 1))
    fi

    # 2) Broker auth: a short subscribe must not be rejected
    local auth_args=()
    [ -n "${MQTT_USERNAME:-}" ] && auth_args+=(-u "$MQTT_USERNAME")
    [ -n "${MQTT_PASSWORD:-}" ] && auth_args+=(-P "$MQTT_PASSWORD")
    # '$SYS' below is a literal MQTT topic, not a shell variable.
    # shellcheck disable=SC2016
    if mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t '$SYS/#' -C 1 -W 5 >/dev/null 2>&1 \
       || mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t "$topic" -E -W 5 >/dev/null 2>&1; then
        echo "✓ Broker accepts our credentials"
    else
        echo "✗ Broker rejected the connection (auth?)"
        echo "  - Set MQTT_USERNAME / MQTT_PASSWORD to match the broker's password file."
        failures=$((failures + 1))
    fi

    # 3) Is Frigate actually publishing?
    echo "… Listening on $topic for ${listen_secs}s (walk past a camera to trigger an event)"
    if mosquitto_sub -h "$host" -p "$port" "${auth_args[@]}" -t "$topic" -C 1 -W "$listen_secs" >/dev/null 2>&1; then
        echo "✓ Frigate traffic seen on $topic"
    else
        echo "✗ No messages on $topic within ${listen_secs}s"
        echo "  - Is 'mqtt: enabled: true' set in your frigate config?"
        echo "  - Does Frigate's mqtt.topic_prefix match FRIGATE_TOPIC_PREFIX ('$prefix')?"
        echo "  - Debug by hand: mosquitto_sub -h $host -p $port -t '$topic' -v"
        failures=$((failures + 1))
    fi

    # 4) Sealed log integrity (only if events have been ingested already)
    if [ -s "$DB_PATH" ]; then
        local seed
        seed=$(resolve_device_key_seed)
        if DEVICE_KEY_SEED="$seed" log_verify --db "$DB_PATH" >/dev/null 2>&1; then
            echo "✓ Sealed log at $DB_PATH verifies (hash chain + signatures)"
        else
            echo "✗ Sealed log verification FAILED — run 'log_verify --db $DB_PATH' for details"
            failures=$((failures + 1))
        fi
    else
        echo "- No sealed log yet at $DB_PATH (nothing ingested so far)"
    fi

    echo
    if [ "$failures" -eq 0 ]; then
        echo "All checks passed."
    else
        echo "$failures check(s) failed."
    fi
    return "$failures"
}

# ---------------------------------------------------------------------------
# run — supervise the three daemons
# ---------------------------------------------------------------------------

run() {
    local addr host port
    addr=$(resolve_broker_addr)
    host=$(split_addr_host "$addr")
    port=$(split_addr_port "$addr")

    DEVICE_KEY_SEED=$(resolve_device_key_seed)
    export DEVICE_KEY_SEED

    local retention_days="${RETENTION_DAYS:-7}"
    local retention_secs=$((retention_days * 86400))
    local bucket_min="${TIME_BUCKET_MINUTES:-10}"
    local bucket_secs=$((bucket_min * 60))
    local prefix="${FRIGATE_TOPIC_PREFIX:-frigate}"
    local topic="${FRIGATE_MQTT_TOPIC:-${prefix}/events}"
    local publish="${SECURACV_PUBLISH:-true}"

    log "broker=$addr topic=$topic retention=${retention_days}d bucket=${bucket_min}m publish=$publish"

    # The broker may still be starting: compose `depends_on` (and the e2e
    # harness) only order container startup, they don't wait for mosquitto
    # to be listening. Retry briefly before giving up.
    local broker_wait="${BROKER_WAIT_SECS:-30}" waited=0
    until tcp_check "$host" "$port"; do
        if [ "$waited" -ge "$broker_wait" ]; then
            die "MQTT broker $addr is not reachable (waited ${waited}s).
  - Is the broker container running and on the same docker network?
  - Run the diagnostics:  docker compose run securacv doctor"
        fi
        if [ "$waited" -eq 0 ]; then
            log "waiting for MQTT broker $addr to accept connections (up to ${broker_wait}s)..."
        fi
        sleep 1
        waited=$((waited + 1))
    done

    cat > "$CONFIG_FILE" <<EOF
{
  "db_path": "$DB_PATH",
  "ruleset_id": "ruleset:frigate_v1",
  "api": {
    "addr": "127.0.0.1:8799",
    "token_path": "$TOKEN_FILE"
  },
  "retention": {
    "seconds": $retention_secs
  }
}
EOF
    export WITNESS_CONFIG="$CONFIG_FILE"

    # The broker is another container, so a non-loopback address is expected.
    # This is safe for the same reason as in the HA add-on: only sanitized
    # event metadata flows over MQTT, never raw media, and every event still
    # passes contract enforcement before it is sealed.
    export ALLOW_REMOTE_MQTT=true

    local pids=()

    witness_api &
    pids+=($!)
    log "witness_api started (PID ${pids[-1]})"

    for _ in $(seq 1 30); do
        [ -s "$TOKEN_FILE" ] && break
        sleep 1
    done
    [ -s "$TOKEN_FILE" ] || die "witness_api did not write its capability token to $TOKEN_FILE"

    local bridge_args=(
        --mqtt-broker-addr "$addr"
        --frigate-topic-prefix "$prefix"
        --db-path "$DB_PATH"
        --bucket-size-secs "$bucket_secs"
        --retention-secs "$retention_secs"
    )
    # Legacy full-topic override wins over the prefix when explicitly set.
    [ -n "${FRIGATE_MQTT_TOPIC:-}" ] && bridge_args+=(--frigate-topic "$FRIGATE_MQTT_TOPIC")
    [ "${FRIGATE_ENABLE_REVIEWS:-false}" = "true" ] && bridge_args+=(--enable-reviews)
    [ -n "${FRIGATE_MIN_CONFIDENCE:-}" ] && bridge_args+=(--min-confidence "$FRIGATE_MIN_CONFIDENCE")
    [ -n "${FRIGATE_CAMERAS:-}" ] && bridge_args+=(--cameras "$FRIGATE_CAMERAS")
    [ -n "${FRIGATE_LABELS:-}" ] && bridge_args+=(--labels "$FRIGATE_LABELS")
    [ -n "${MQTT_USERNAME:-}" ] && bridge_args+=(--mqtt-username "$MQTT_USERNAME")
    [ -n "${MQTT_PASSWORD:-}" ] && bridge_args+=(--mqtt-password "$MQTT_PASSWORD")

    frigate_bridge "${bridge_args[@]}" &
    pids+=($!)
    log "frigate_bridge started (PID ${pids[-1]})"

    if [ "$publish" = "true" ]; then
        # The public HA device id must carry ZERO bits of the signing seed.
        # DEVICE_KEY_SEED is the secret Ed25519 seed users are told to back up,
        # and the device id is published in MQTT discovery topics every broker
        # client can read — so derive the id from sha256(seed) under a
        # domain-separation prefix. Stable per seed, but the seed is not
        # recoverable from it (previously it exposed the first 32 seed bits).
        #
        # HA entity-id MIGRATION: this changes the id from `pwk_<seed[0:8]>` to
        # `pwk_<sha256(...)[0:8]>`, so entity_ids (sensor.pwk_<id>_*) change on
        # upgrade — old entities orphan and re-create under the new id. Re-map
        # any dashboards/automations that referenced the old ids once, or delete
        # the orphaned entities in HA.
        local ha_device_id
        ha_device_id="pwk_$(printf '%s' "securacv:ha-device-id:v1:${DEVICE_KEY_SEED}" | sha256sum | cut -c1-8)"
        local pub_args=(
            --daemon
            --mqtt-broker-addr "$addr"
            --api-token-path "$TOKEN_FILE"
            --ha-discovery-prefix "${HA_DISCOVERY_PREFIX:-homeassistant}"
            --mqtt-topic-prefix "${MQTT_TOPIC_PREFIX:-witness}"
            --ha-device-id "$ha_device_id"
            --poll-interval "${POLL_INTERVAL:-30}"
            --verify-interval-secs "${VERIFY_INTERVAL_SECS:-86400}"
        )
        [ -n "${MQTT_USERNAME:-}" ] && pub_args+=(--mqtt-username "$MQTT_USERNAME")
        [ -n "${MQTT_PASSWORD:-}" ] && pub_args+=(--mqtt-password "$MQTT_PASSWORD")
        event_mqtt_bridge "${pub_args[@]}" &
        pids+=($!)
        log "event_mqtt_bridge started (PID ${pids[-1]})"
    else
        log "HA Discovery publishing disabled (SECURACV_PUBLISH=$publish)"
    fi

    # Propagate the first child exit so the container restarts on failure.
    local status=0
    wait -n "${pids[@]}" || status=$?
    log "a service exited (status $status); shutting the sidecar down"
    kill "${pids[@]}" 2>/dev/null || true
    wait || true
    exit "$status"
}

case "${1:-run}" in
    doctor) doctor ;;
    run) run ;;
    *) exec "$@" ;;
esac
