#!/usr/bin/with-contenv bashio
# Privacy Witness Kernel - Home Assistant Add-on Entry Script
# Reads HA add-on options and starts witnessd with proper configuration

set -e

CONFIG_FILE="/config/witness_config.json"
DB_PATH="/config/witness.db"
VAULT_PATH="/share/witness_vault"

# Log startup
bashio::log.info "Starting Privacy Witness Kernel add-on..."

# Read add-on configuration
DEVICE_KEY_SEED=$(bashio::config 'device_key_seed')
RETENTION_DAYS=$(bashio::config 'retention_days')
TIME_BUCKET_MIN=$(bashio::config 'time_bucket_minutes')
LOG_LEVEL=$(bashio::config 'log_level')
GO2RTC_DISCOVERY=$(bashio::config 'go2rtc_discovery')
GO2RTC_URL=$(bashio::config 'go2rtc_url')

# Validate device key seed
if [ -z "$DEVICE_KEY_SEED" ] || [ "$DEVICE_KEY_SEED" = "devkey:mvp" ]; then
    bashio::log.error "CRITICAL: device_key_seed must be set to a unique, secret value!"
    bashio::log.error "Generate one with: openssl rand -hex 32"
    bashio::log.error "This key protects the cryptographic identity of your device."
    exit 1
fi

# Calculate retention in seconds
RETENTION_SECS=$((RETENTION_DAYS * 86400))

# Ensure vault directory exists
mkdir -p "$VAULT_PATH"

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
            # Merge discovered cameras with manual ones (manual takes precedence)
            cameras_json=$(echo "$cameras_json $discovered" | jq -s 'add | unique_by(.name)')
        else
            bashio::log.warning "No cameras discovered from go2rtc (this is OK if cameras are manually configured)"
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
bashio::log.info "  URL: ${CAMERA_URL%%@*}@[redacted]" # Don't log credentials
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

# Export environment variables
export DEVICE_KEY_SEED
export WITNESS_CONFIG="$CONFIG_FILE"
export WITNESS_VAULT_PATH="$VAULT_PATH"
export RUST_LOG="witness_kernel=$LOG_LEVEL"

# Print startup summary
bashio::log.info "=== Privacy Witness Kernel Configuration ==="
bashio::log.info "  Database: $DB_PATH"
bashio::log.info "  Vault: $VAULT_PATH"
bashio::log.info "  Retention: $RETENTION_DAYS days"
bashio::log.info "  Time bucket: $TIME_BUCKET_MIN minutes"
bashio::log.info "  Log level: $LOG_LEVEL"
bashio::log.info "============================================="

# Start the daemon
bashio::log.info "Starting witnessd..."
exec /usr/local/bin/witnessd
