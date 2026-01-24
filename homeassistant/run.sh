#!/usr/bin/with-contenv bashio
# Privacy Witness Kernel - Home Assistant Add-on Entry Script
# Supports two modes:
# - standalone: Process RTSP streams directly
# - frigate: Subscribe to Frigate MQTT events for detection
#
# Both modes can optionally publish events to MQTT with HA Discovery for
# automatic entity creation in Home Assistant.

set -e

CONFIG_FILE="/config/witness_config.json"
DB_PATH="/config/witness.db"
VAULT_PATH="/share/witness_vault"
TOKEN_FILE="/config/api_token"

# Log startup
bashio::log.info "Starting Privacy Witness Kernel add-on..."

# Read common configuration
DEVICE_KEY_SEED=$(bashio::config 'device_key_seed')
RETENTION_DAYS=$(bashio::config 'retention_days')
TIME_BUCKET_MIN=$(bashio::config 'time_bucket_minutes')
LOG_LEVEL=$(bashio::config 'log_level')
MODE=$(bashio::config 'mode')

# MQTT publishing configuration
MQTT_PUBLISH_ENABLED="false"
if bashio::config.has_value 'mqtt_publish.enabled'; then
    MQTT_PUBLISH_ENABLED=$(bashio::config 'mqtt_publish.enabled')
fi

# Validate device key seed
if [ -z "$DEVICE_KEY_SEED" ] || [ "$DEVICE_KEY_SEED" = "devkey:mvp" ]; then
    bashio::log.error "CRITICAL: device_key_seed must be set to a unique, secret value!"
    bashio::log.error "Generate one with: openssl rand -hex 32"
    bashio::log.error "This key protects the cryptographic identity of your device."
    exit 1
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
# Function to start MQTT publishing daemon (if enabled)
# ============================================================================
start_mqtt_publisher() {
    if [ "$MQTT_PUBLISH_ENABLED" != "true" ]; then
        return
    fi

    bashio::log.info "Starting MQTT event publisher with HA Discovery..."

    # Read MQTT publish configuration
    local PUBLISH_HOST PUBLISH_PORT PUBLISH_USER PUBLISH_PASS PUBLISH_PREFIX DISCOVERY_PREFIX

    if bashio::config.has_value 'mqtt_publish.host'; then
        PUBLISH_HOST=$(bashio::config 'mqtt_publish.host')
    else
        PUBLISH_HOST="core-mosquitto"
    fi

    if bashio::config.has_value 'mqtt_publish.port'; then
        PUBLISH_PORT=$(bashio::config 'mqtt_publish.port')
    else
        PUBLISH_PORT="1883"
    fi

    if bashio::config.has_value 'mqtt_publish.username'; then
        PUBLISH_USER=$(bashio::config 'mqtt_publish.username')
    fi

    if bashio::config.has_value 'mqtt_publish.password'; then
        PUBLISH_PASS=$(bashio::config 'mqtt_publish.password')
    fi

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
    for i in $(seq 1 30); do
        if curl -sf http://127.0.0.1:8799/health > /dev/null 2>&1; then
            break
        fi
        sleep 1
    done

    # Generate device ID from key seed
    local DEVICE_ID
    DEVICE_ID="pwk_${DEVICE_KEY_SEED:0:8}"

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
    )

    if [ -n "$PUBLISH_USER" ]; then
        MQTT_CMD_ARRAY+=(--mqtt-username "$PUBLISH_USER")
    fi

    if [ -n "$PUBLISH_PASS" ]; then
        MQTT_CMD_ARRAY+=(--mqtt-password "$PUBLISH_PASS")
    fi

    bashio::log.info "MQTT Publisher: $PUBLISH_HOST:$PUBLISH_PORT"
    bashio::log.info "  Topic prefix: $PUBLISH_PREFIX"
    bashio::log.info "  Discovery prefix: $DISCOVERY_PREFIX"
    bashio::log.info "  Device ID: $DEVICE_ID"

    # Start in background
    "${MQTT_CMD_ARRAY[@]}" &
    bashio::log.info "MQTT publisher started (PID: $!)"
}

# ============================================================================
# FRIGATE MODE
# ============================================================================
if [ "$MODE" = "frigate" ]; then
    bashio::log.info "=== Frigate Integration Mode ==="

    # Read Frigate configuration
    MQTT_HOST=$(bashio::config 'frigate.mqtt_host')
    MQTT_PORT=$(bashio::config 'frigate.mqtt_port')
    MQTT_TOPIC=$(bashio::config 'frigate.mqtt_topic')
    MIN_CONFIDENCE=$(bashio::config 'frigate.min_confidence')

    # MQTT authentication (optional)
    MQTT_USER=""
    MQTT_PASS=""
    if bashio::config.has_value 'frigate.mqtt_username'; then
        MQTT_USER=$(bashio::config 'frigate.mqtt_username')
    fi
    if bashio::config.has_value 'frigate.mqtt_password'; then
        MQTT_PASS=$(bashio::config 'frigate.mqtt_password')
    fi

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
        --frigate-topic "$MQTT_TOPIC"
        --db-path "$DB_PATH"
        --bucket-size-secs "$BUCKET_SECS"
        --min-confidence "$MIN_CONFIDENCE"
    )

    if [ -n "$MQTT_USER" ]; then
        CMD_ARRAY+=(--mqtt-username "$MQTT_USER")
    fi

    if [ -n "$MQTT_PASS" ]; then
        CMD_ARRAY+=(--mqtt-password "$MQTT_PASS")
    fi

    if [ -n "$FRIGATE_CAMERAS" ]; then
        CMD_ARRAY+=(--cameras "$FRIGATE_CAMERAS")
    fi

    if [ -n "$FRIGATE_LABELS" ]; then
        CMD_ARRAY+=(--labels "$FRIGATE_LABELS")
    fi

    # Start MQTT publisher in background if enabled (for HA Discovery)
    # Note: In Frigate mode, we run witnessd API in background for the publisher
    if [ "$MQTT_PUBLISH_ENABLED" = "true" ]; then
        bashio::log.info "MQTT publishing enabled - starting witnessd API in background..."
        /usr/local/bin/witnessd &
        WITNESSD_PID=$!
        sleep 2
        start_mqtt_publisher
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
    "addr": "0.0.0.0:8799",
    "token_path": "/config/api_token"
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

    bashio::log.info "Starting witnessd..."
    exec /usr/local/bin/witnessd
fi
