#!/usr/bin/with-contenv bashio
set -euo pipefail
# Privacy Witness Kernel - Home Assistant Add-on Entry Script
# Supports two modes:
# - standalone: Process RTSP streams directly
# - frigate: Subscribe to Frigate MQTT events for detection
#
# Both modes can optionally publish events to MQTT with HA Discovery for
# automatic entity creation in Home Assistant.


CONFIG_FILE="/config/witness_config.json"
DB_PATH="/config/witness.db"
VAULT_PATH="/share/witness_vault"
TOKEN_FILE="/config/api_token"
DEVICE_KEY_FILE="/config/.securacv/device_key"
INGRESS_PORT="${INGRESS_PORT:-8788}"

# shellcheck source=privacy_witness_kernel/run_lib.sh disable=SC1091
source /usr/local/bin/run_lib.sh

# Log startup
bashio::log.info "Starting Privacy Witness Kernel add-on..."

# The Supervisor provides SUPERVISOR_TOKEN directly; HASSIO_TOKEN is the
# deprecated alias and only a fallback. Never let the alias clobber the
# real token — on modern Supervisors HASSIO_TOKEN can be empty, and
# overwriting SUPERVISOR_TOKEN with it broke every Supervisor API call
# (wizard saves, discovery, restarts).
export SUPERVISOR_TOKEN="${SUPERVISOR_TOKEN:-${HASSIO_TOKEN:-}}"

# ============================================================================
# First-run detection: the device key is auto-generated, so its absence no
# longer marks a fresh install. The add-on is considered configured when the
# user has made any deliberate choice: a key (option or wizard-written file),
# frigate mode, or manually configured cameras. Otherwise serve the setup
# wizard exclusively so the kernel doesn't crash-loop on factory defaults.
# ============================================================================
DEVICE_KEY_SEED=$(bashio::config 'device_key_seed' || echo "")
MODE=$(bashio::config 'mode')

CONFIGURED="false"
if [ -n "$DEVICE_KEY_SEED" ] && [ "$DEVICE_KEY_SEED" != "devkey:mvp" ]; then
    CONFIGURED="true"
elif [ -s "$DEVICE_KEY_FILE" ]; then
    CONFIGURED="true"
elif [ "$MODE" = "frigate" ]; then
    CONFIGURED="true"
elif bashio::config.has_value 'cameras' && [ "$(bashio::config 'cameras' | jq length)" -gt 0 ]; then
    CONFIGURED="true"
fi

if [ "$CONFIGURED" != "true" ]; then
    bashio::log.info "=== First-run detected: serving setup wizard on port ${INGRESS_PORT} ==="
    bashio::log.info "Open the add-on Web UI to complete setup."

    exec /usr/bin/python3 /usr/local/bin/serve_wizard.py
fi

# Configured: keep the panel available alongside the kernel (status view,
# reconfiguration, health checks). It is a stdlib-only Python process.
/usr/bin/python3 /usr/local/bin/serve_wizard.py &
bashio::log.info "Status panel started on port ${INGRESS_PORT} (PID: $!)"

# Resolve the device key: option → wizard-written key file → auto-generate
# and persist (0600). /config is captured by HA backups; the panel reminds
# the user to back the key up.
DEVICE_KEY_SEED=$(resolve_device_key_seed "$DEVICE_KEY_SEED" "$DEVICE_KEY_FILE")
if [ -z "$DEVICE_KEY_SEED" ]; then
    bashio::log.error "Failed to resolve or generate a device key seed."
    exit 1
fi

# Read common configuration
RETENTION_DAYS=$(bashio::config 'retention_days')
TIME_BUCKET_MIN=$(bashio::config 'time_bucket_minutes')
LOG_LEVEL=$(bashio::config 'log_level')

# MQTT publishing configuration: default ON in frigate mode (events are
# invisible in HA without it), OFF in standalone unless explicitly enabled.
MQTT_PUBLISH_ENABLED=""
if bashio::config.has_value 'mqtt_publish.enabled'; then
    MQTT_PUBLISH_ENABLED=$(bashio::config 'mqtt_publish.enabled')
fi
if [ -z "$MQTT_PUBLISH_ENABLED" ] || [ "$MQTT_PUBLISH_ENABLED" = "null" ]; then
    if [ "$MODE" = "frigate" ]; then
        MQTT_PUBLISH_ENABLED="true"
    else
        MQTT_PUBLISH_ENABLED="false"
    fi
fi

# ============================================================================
# Supervisor MQTT service discovery (requires `services: mqtt:want` in
# config.yaml). Explicit option values always win; discovery fills the gaps
# so Mosquitto add-on users never type broker credentials.
# ============================================================================
DISCOVERED_MQTT_HOST=""
DISCOVERED_MQTT_PORT=""
DISCOVERED_MQTT_USER=""
DISCOVERED_MQTT_PASS=""
if bashio::services.available "mqtt" 2>/dev/null; then
    DISCOVERED_MQTT_HOST=$(bashio::services "mqtt" "host" 2>/dev/null || echo "")
    DISCOVERED_MQTT_PORT=$(bashio::services "mqtt" "port" 2>/dev/null || echo "")
    DISCOVERED_MQTT_USER=$(bashio::services "mqtt" "username" 2>/dev/null || echo "")
    DISCOVERED_MQTT_PASS=$(bashio::services "mqtt" "password" 2>/dev/null || echo "")
    bashio::log.info "MQTT broker auto-discovered via Supervisor: ${DISCOVERED_MQTT_HOST}:${DISCOVERED_MQTT_PORT}"
else
    bashio::log.info "No Supervisor MQTT service available; using configured/default broker settings."
fi

# Calculate retention in seconds
RETENTION_SECS=$((RETENTION_DAYS * 86400))
BUCKET_SECS=$((TIME_BUCKET_MIN * 60))

# Ensure vault directory exists
mkdir -p "$VAULT_PATH"

# Export common environment variables
export DEVICE_KEY_SEED
export WITNESS_CONFIG="$CONFIG_FILE"
export WITNESS_VAULT_PATH="$VAULT_PATH"
export RUST_LOG="witness_kernel=$LOG_LEVEL"

bashio::log.info "Mode: $MODE"
bashio::log.info "Database: $DB_PATH"
bashio::log.info "Retention: $RETENTION_DAYS days"
bashio::log.info "Time bucket: $TIME_BUCKET_MIN minutes"

# ============================================================================
# Event API exposure. The API binds all interfaces inside the container so
# the SecuraCV integration (running in HA core) can reach it over the
# internal add-on network at http://d0491a67-privacy-witness-kernel:8799.
# This is NOT the LAN: config.yaml ships the 8799 host port mapping
# disabled, so nothing outside the Supervisor network can connect unless
# the user enables the port explicitly. Every data endpoint still requires
# the rotating capability token from $TOKEN_FILE — the bind address never
# relaxes authentication. The kernel refuses a non-loopback bind unless
# this opt-in is set (its config file has no such field; the env var is
# the implemented switch).
# ============================================================================
export WITNESS_API_ALLOW_INSECURE=1
API_BIND_ADDR="0.0.0.0:8799"

# ============================================================================
# Supervisor discovery announcement. Once the kernel's Event API answers
# /health, tell the Supervisor where it lives so the SecuraCV integration
# can show a zero-click discovered card. Non-fatal: setup works without
# it — discovery only removes typing on the integration side.
# ============================================================================
announce_discovery() {
    local addon_host payload
    addon_host=$(bashio::addon.hostname 2>/dev/null || hostname)
    if [ -z "$addon_host" ]; then
        bashio::log.warning "Discovery: could not determine the add-on hostname; skipping announcement."
        return 0
    fi

    bashio::log.info "Discovery: waiting for the Event API before announcing to the Supervisor..."
    local ready="false"
    for _ in $(seq 1 60); do
        if curl -sf http://127.0.0.1:8799/health > /dev/null 2>&1; then
            ready="true"
            break
        fi
        sleep 2
    done
    if [ "$ready" != "true" ]; then
        bashio::log.warning "Discovery: Event API never became healthy; skipping announcement."
        return 0
    fi

    payload=$(compose_discovery_payload "$addon_host" 8799)
    if curl -sf -X POST \
        -H "Authorization: Bearer ${SUPERVISOR_TOKEN}" \
        -H "Content-Type: application/json" \
        -d "$payload" \
        http://supervisor/discovery > /dev/null 2>&1; then
        bashio::log.info "Discovery: announced the Event API to the Supervisor (${addon_host}:8799)."
        bashio::log.info "Discovery: the SecuraCV integration will now appear under Settings -> Devices & Services."
    else
        bashio::log.warning "Discovery: announcement to the Supervisor failed (non-fatal)."
        bashio::log.warning "Discovery: add the SecuraCV integration manually; the kernel itself is unaffected."
    fi
    return 0
}

# ============================================================================
# Function to write API config for Frigate mode
# ============================================================================
write_frigate_api_config() {
    cat > "$CONFIG_FILE" << EOF
{
  "db_path": "$DB_PATH",
  "ruleset_id": "ruleset:frigate_v1",
  "api": {
    "addr": "$API_BIND_ADDR",
    "token_path": "$TOKEN_FILE"
  },
  "retention": {
    "seconds": $RETENTION_SECS
  }
}
EOF
}

# ============================================================================
# Function to start MQTT publishing daemon (if enabled)
# ============================================================================
start_mqtt_publisher() {
    if [ "$MQTT_PUBLISH_ENABLED" != "true" ]; then
        return
    fi

    bashio::log.info "Starting MQTT event publisher with HA Discovery..."

    # Read MQTT publish configuration: explicit option → discovered → default
    local PUBLISH_HOST PUBLISH_PORT PUBLISH_PREFIX DISCOVERY_PREFIX
    local PUBLISH_USER="" PUBLISH_PASS=""
    local OPT_HOST="" OPT_PORT="" OPT_USER="" OPT_PASS=""

    if bashio::config.has_value 'mqtt_publish.host'; then
        OPT_HOST=$(bashio::config 'mqtt_publish.host')
    fi
    if bashio::config.has_value 'mqtt_publish.port'; then
        OPT_PORT=$(bashio::config 'mqtt_publish.port')
    fi
    if bashio::config.has_value 'mqtt_publish.username'; then
        OPT_USER=$(bashio::config 'mqtt_publish.username')
    fi
    if bashio::config.has_value 'mqtt_publish.password'; then
        OPT_PASS=$(bashio::config 'mqtt_publish.password')
    fi

    PUBLISH_HOST=$(resolve_value "$OPT_HOST" "$DISCOVERED_MQTT_HOST" "core-mosquitto")
    PUBLISH_PORT=$(resolve_value "$OPT_PORT" "$DISCOVERED_MQTT_PORT" "1883")
    PUBLISH_USER=$(resolve_value "$OPT_USER" "$DISCOVERED_MQTT_USER" "")
    PUBLISH_PASS=$(resolve_value "$OPT_PASS" "$DISCOVERED_MQTT_PASS" "")

    if bashio::config.has_value 'mqtt_publish.topic_prefix'; then
        PUBLISH_PREFIX=$(bashio::config 'mqtt_publish.topic_prefix')
    else
        PUBLISH_PREFIX="witness"
    fi

    if bashio::config.has_value 'mqtt_publish.discovery_prefix'; then
        DISCOVERY_PREFIX=$(bashio::config 'mqtt_publish.discovery_prefix')
    else
        DISCOVERY_PREFIX="homeassistant"
    fi

    # Wait for API to be ready
    bashio::log.info "Waiting for witness API..."
    for _ in $(seq 1 30); do
        if curl -sf http://127.0.0.1:8799/health > /dev/null 2>&1; then
            break
        fi
        sleep 1
    done

    # Public HA device id. It lands in retained MQTT discovery topics that every
    # broker client can read, so it must carry ZERO bits of the seed: sha256 of
    # the seed under a domain-separation prefix — the same derivation as the
    # Docker sidecar (docker/sidecar/entrypoint.sh) and event_mqtt_bridge's own
    # fallback, so every deployment names a given seed the same way. This line
    # used to publish the first 8 characters of the seed itself.
    #
    # Entity-id MIGRATION: `pwk_<seed[0:8]>` becomes `pwk_<sha256(...)[0:8]>`,
    # so entity_ids (sensor.pwk_<id>_*) change once on upgrade — old entities
    # orphan and re-create under the new id. Re-point dashboards or automations
    # that named the old ids, or delete the orphans in HA.
    local DEVICE_ID
    DEVICE_ID="pwk_$(printf '%s' "securacv:ha-device-id:v1:${DEVICE_KEY_SEED}" | sha256sum | cut -c1-8)"

    # Scheduled sealed-log verification cadence (hours; 0 disables)
    local VERIFY_INTERVAL_HOURS=24
    if bashio::config.has_value 'verify_interval_hours'; then
        VERIFY_INTERVAL_HOURS=$(bashio::config 'verify_interval_hours')
    fi

    # Build command using array for safe argument handling
    local MQTT_CMD_ARRAY
    MQTT_CMD_ARRAY=(
        "/usr/local/bin/event_mqtt_bridge"
        --daemon
        --allow-remote-mqtt
        --mqtt-broker-addr "$PUBLISH_HOST:$PUBLISH_PORT"
        --mqtt-topic-prefix "$PUBLISH_PREFIX"
        --ha-discovery-prefix "$DISCOVERY_PREFIX"
        --ha-device-id "$DEVICE_ID"
        --api-token-path "$TOKEN_FILE"
        --poll-interval 30
        --verify-interval-secs "$((VERIFY_INTERVAL_HOURS * 3600))"
    )

    if [ -n "$PUBLISH_USER" ]; then
        MQTT_CMD_ARRAY+=(--mqtt-username "$PUBLISH_USER")
    fi

    # The password travels in the environment, never on argv: argv is readable
    # from /proc/<pid>/cmdline by anything in the container and shows up in
    # process listings and crash reports. event_mqtt_bridge reads MQTT_PASSWORD
    # itself (clap `env`), the same way it already takes DEVICE_KEY_SEED. Set
    # for this one child only (`env` below), not exported: in Frigate mode the
    # frigate bridge may talk to a different broker with a different password.
    local -a PUB_ENV=()
    if [ -n "$PUBLISH_PASS" ]; then
        PUB_ENV=(MQTT_PASSWORD="$PUBLISH_PASS")
    fi

    bashio::log.info "MQTT Publisher: $PUBLISH_HOST:$PUBLISH_PORT"
    bashio::log.info "  Topic prefix: $PUBLISH_PREFIX"
    bashio::log.info "  Discovery prefix: $DISCOVERY_PREFIX"
    bashio::log.info "  Device ID: $DEVICE_ID"

    # Start in background
    env "${PUB_ENV[@]}" "${MQTT_CMD_ARRAY[@]}" &
    bashio::log.info "MQTT publisher started (PID: $!)"
}

# ============================================================================
# FRIGATE MODE
# ============================================================================
if [ "$MODE" = "frigate" ]; then
    bashio::log.info "=== Frigate Integration Mode ==="

    # Read Frigate configuration: explicit option → discovered → default
    OPT_FRIGATE_HOST=""
    OPT_FRIGATE_PORT=""
    OPT_FRIGATE_USER=""
    OPT_FRIGATE_PASS=""
    OPT_FRIGATE_TOPIC=""
    if bashio::config.has_value 'frigate.mqtt_host'; then
        OPT_FRIGATE_HOST=$(bashio::config 'frigate.mqtt_host')
    fi
    if bashio::config.has_value 'frigate.mqtt_port'; then
        OPT_FRIGATE_PORT=$(bashio::config 'frigate.mqtt_port')
    fi
    if bashio::config.has_value 'frigate.mqtt_username'; then
        OPT_FRIGATE_USER=$(bashio::config 'frigate.mqtt_username')
    fi
    if bashio::config.has_value 'frigate.mqtt_password'; then
        OPT_FRIGATE_PASS=$(bashio::config 'frigate.mqtt_password')
    fi
    if bashio::config.has_value 'frigate.mqtt_topic'; then
        OPT_FRIGATE_TOPIC=$(bashio::config 'frigate.mqtt_topic')
    fi

    TOPIC_PREFIX="frigate"
    if bashio::config.has_value 'frigate.topic_prefix'; then
        TOPIC_PREFIX=$(bashio::config 'frigate.topic_prefix')
    fi

    ENABLE_REVIEWS="false"
    if bashio::config.has_value 'frigate.enable_reviews'; then
        ENABLE_REVIEWS=$(bashio::config 'frigate.enable_reviews')
    fi

    MQTT_HOST=$(resolve_value "$OPT_FRIGATE_HOST" "$DISCOVERED_MQTT_HOST" "core-mosquitto")
    MQTT_PORT=$(resolve_value "$OPT_FRIGATE_PORT" "$DISCOVERED_MQTT_PORT" "1883")
    MQTT_USER=$(resolve_value "$OPT_FRIGATE_USER" "$DISCOVERED_MQTT_USER" "")
    MQTT_PASS=$(resolve_value "$OPT_FRIGATE_PASS" "$DISCOVERED_MQTT_PASS" "")
    MQTT_TOPIC=$(resolve_frigate_topic "$OPT_FRIGATE_TOPIC" "$TOPIC_PREFIX")
    MIN_CONFIDENCE=$(bashio::config 'frigate.min_confidence')

    # Get cameras and labels as comma-separated strings
    FRIGATE_CAMERAS=""
    if bashio::config.has_value 'frigate.cameras'; then
        FRIGATE_CAMERAS=$(bashio::config 'frigate.cameras' | jq -r 'join(",")')
    fi

    FRIGATE_LABELS=""
    if bashio::config.has_value 'frigate.labels'; then
        FRIGATE_LABELS=$(bashio::config 'frigate.labels' | jq -r 'join(",")')
    fi

    bashio::log.info "MQTT Broker: $MQTT_HOST:$MQTT_PORT"
    bashio::log.info "Frigate Topic: $MQTT_TOPIC"
    bashio::log.info "Min Confidence: $MIN_CONFIDENCE"
    bashio::log.info "Cameras: ${FRIGATE_CAMERAS:-all}"
    bashio::log.info "Labels: ${FRIGATE_LABELS:-default}"
    bashio::log.info "MQTT Auth: $([ -n "$MQTT_USER" ] && echo "enabled" || echo "disabled")"

    # Build frigate_bridge command using array for safe argument handling
    # Note: --allow-remote-mqtt is safe in HA because:
    # 1. The MQTT broker (core-mosquitto) is a trusted HA component
    # 2. All event data is still sanitized before logging
    # 3. No raw media ever flows through this path
    CMD_ARRAY=(
        "/usr/local/bin/frigate_bridge"
        --allow-remote-mqtt
        --mqtt-broker-addr "$MQTT_HOST:$MQTT_PORT"
        --frigate-topic-prefix "$TOPIC_PREFIX"
        --db-path "$DB_PATH"
        --bucket-size-secs "$BUCKET_SECS"
        --min-confidence "$MIN_CONFIDENCE"
        --retention-secs "$RETENTION_SECS"
    )

    # Legacy full-topic override wins over the prefix when explicitly set.
    if [ -n "$OPT_FRIGATE_TOPIC" ]; then
        CMD_ARRAY+=(--frigate-topic "$OPT_FRIGATE_TOPIC")
    fi

    if [ "$ENABLE_REVIEWS" = "true" ]; then
        CMD_ARRAY+=(--enable-reviews)
    fi

    if [ -n "$MQTT_USER" ]; then
        CMD_ARRAY+=(--mqtt-username "$MQTT_USER")
    fi

    # The broker password reaches frigate_bridge through the environment,
    # never argv — set right at the exec below, so it is THIS broker's
    # password and not the publisher's.

    if [ -n "$FRIGATE_CAMERAS" ]; then
        CMD_ARRAY+=(--cameras "$FRIGATE_CAMERAS")
    fi

    if [ -n "$FRIGATE_LABELS" ]; then
        CMD_ARRAY+=(--labels "$FRIGATE_LABELS")
    fi

    bashio::log.info "Starting witness API service for Frigate mode..."
    write_frigate_api_config
    /usr/local/bin/witness_api &
    WITNESS_API_PID=$!
    bashio::log.info "Witness API started (PID: $WITNESS_API_PID)"

    # Announce the Event API for zero-click integration setup (background;
    # waits for /health itself, never blocks or fails startup).
    announce_discovery &

    # Start MQTT publisher in background if enabled (for HA Discovery)
    if [ "$MQTT_PUBLISH_ENABLED" = "true" ]; then
        start_mqtt_publisher
    fi

    # Password through the environment, not argv (readable from
    # /proc/<pid>/cmdline by anything in the container). Set — or cleared —
    # right here so the exec'd bridge sees its own broker's password only.
    if [ -n "$MQTT_PASS" ]; then
        export MQTT_PASSWORD="$MQTT_PASS"
    else
        unset MQTT_PASSWORD
    fi
    bashio::log.info "Starting frigate_bridge..."
    exec "${CMD_ARRAY[@]}"

# ============================================================================
# STANDALONE MODE (Direct RTSP)
# ============================================================================
else
    bashio::log.info "=== Standalone Mode (Direct RTSP) ==="

    GO2RTC_DISCOVERY=$(bashio::config 'go2rtc_discovery')
    GO2RTC_URL=$(bashio::config 'go2rtc_url')

    # Build camera configuration
    build_camera_config() {
        local cameras_json="[]"

        # Check if cameras are manually configured
        if bashio::config.has_value 'cameras'; then
            cameras_json=$(bashio::config 'cameras')
            bashio::log.info "Found $(echo "$cameras_json" | jq length) manually configured cameras"
        fi

        # Try go2rtc discovery if enabled
        if [ "$GO2RTC_DISCOVERY" = "true" ]; then
            bashio::log.info "Attempting go2rtc camera discovery from $GO2RTC_URL..."
            discovered=$(/usr/local/bin/discover_cameras.sh "$GO2RTC_URL" 2>/dev/null || echo "[]")
            discovered_count=$(echo "$discovered" | jq length)

            if [ "$discovered_count" -gt 0 ]; then
                bashio::log.info "Discovered $discovered_count cameras from go2rtc"
                cameras_json=$(echo "$cameras_json $discovered" | jq -s 'add | unique_by(.name)')
            else
                bashio::log.warning "No cameras discovered from go2rtc"
            fi
        fi

        echo "$cameras_json"
    }

    CAMERAS_JSON=$(build_camera_config)
    CAMERA_COUNT=$(echo "$CAMERAS_JSON" | jq length)

    if [ "$CAMERA_COUNT" -eq 0 ]; then
        bashio::log.error "No cameras configured!"
        bashio::log.error "Either:"
        bashio::log.error "  1. Add cameras manually in the add-on configuration"
        bashio::log.error "  2. Enable go2rtc_discovery and ensure go2rtc is running"
        bashio::log.error "  3. Switch to 'frigate' mode if using Frigate NVR"
        exit 1
    fi

    bashio::log.info "Configuring $CAMERA_COUNT camera(s)..."

    # For now, use the first camera (multi-camera support in future version)
    FIRST_CAMERA=$(echo "$CAMERAS_JSON" | jq -r '.[0]')
    CAMERA_NAME=$(echo "$FIRST_CAMERA" | jq -r '.name // "camera"')
    CAMERA_URL=$(echo "$FIRST_CAMERA" | jq -r '.url')
    CAMERA_ZONE=$(echo "$FIRST_CAMERA" | jq -r '.zone_id // "zone:front_boundary"')
    CAMERA_FPS=$(echo "$FIRST_CAMERA" | jq -r '.fps // 10')
    CAMERA_WIDTH=$(echo "$FIRST_CAMERA" | jq -r '.width // 640')
    CAMERA_HEIGHT=$(echo "$FIRST_CAMERA" | jq -r '.height // 480')

    bashio::log.info "Primary camera: $CAMERA_NAME"
    bashio::log.info "  URL: ${CAMERA_URL%%@*}@[redacted]"
    bashio::log.info "  Zone: $CAMERA_ZONE"
    bashio::log.info "  Resolution: ${CAMERA_WIDTH}x${CAMERA_HEIGHT} @ ${CAMERA_FPS}fps"

    # Validate zone ID format
    if ! echo "$CAMERA_ZONE" | grep -qE '^zone:[a-z0-9_-]{1,64}$'; then
        bashio::log.error "Invalid zone_id format: $CAMERA_ZONE"
        bashio::log.error "Zone IDs must match: zone:[a-z0-9_-]{1,64}"
        exit 1
    fi

    # Build configuration file
    cat > "$CONFIG_FILE" << EOF
{
  "db_path": "$DB_PATH",
  "ruleset_id": "ruleset:homeassistant_v1",
  "api": {
    "addr": "$API_BIND_ADDR",
    "token_path": "$TOKEN_FILE"
  },
  "rtsp": {
    "url": "$CAMERA_URL",
    "target_fps": $CAMERA_FPS,
    "width": $CAMERA_WIDTH,
    "height": $CAMERA_HEIGHT
  },
  "zones": {
    "module_zone_id": "$CAMERA_ZONE",
    "sensitive": ["$CAMERA_ZONE"]
  },
  "retention": {
    "seconds": $RETENTION_SECS
  }
}
EOF

    bashio::log.info "Configuration written to $CONFIG_FILE"

    # Start MQTT publisher in background if enabled (for HA Discovery)
    if [ "$MQTT_PUBLISH_ENABLED" = "true" ]; then
        (
            # Wait for witnessd to start, then launch publisher
            sleep 5
            start_mqtt_publisher
        ) &
    fi

    # Announce the Event API for zero-click integration setup (background;
    # waits for /health itself, never blocks or fails startup).
    announce_discovery &

    bashio::log.info "Starting witnessd..."
    exec /usr/local/bin/witnessd
fi
