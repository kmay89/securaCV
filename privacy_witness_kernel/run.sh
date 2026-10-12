#!/usr/bin/with-contenv bashio
set -euo pipefail
# SecuraCV app — the shared setup, run ONCE at container start as an s6
# cont-init step (rootfs/etc/cont-init.d/securacv-setup, a copy of this
# file). It renders everything the long-running services need and leaves
# it under /run/securacv:
#
#   /run/securacv/setup.sh     bash-sourceable: MODE, the kernel config
#                              paths, the argv arrays and the per-broker
#                              passwords of each service (0600, tmpfs)
#   /run/securacv/configured   present only when the app is configured;
#                              every kernel service and the watchdog probe
#                              gate on it, so an unconfigured app runs the
#                              setup wizard alone without flapping
#
# The services (rootfs/etc/services.d/*): panel (always), witness_api +
# frigate_bridge (frigate mode), witnessd (standalone mode),
# event_mqtt_bridge (when publishing), discovery (announces the API to the
# Supervisor once it answers /health). Each sources setup.sh and execs its
# binary; none of them renders config.
#
# Two things this step also does on every start, before the gate:
#   * places the bundled blueprints under /config/blueprints/automation/
#     securacv (never over a copy the user edited; run_lib.sh install_blueprints)
#   * updates the SecuraCV integration the image carries, but ONLY a copy
#     this app placed itself and only to a newer version, then asks for a
#     restart with a persistent notification (integration_carry.py; D15).
#     The first install is a click in the wizard, never a boot.


CONFIG_FILE="/config/witness_config.json"
DB_PATH="/config/witness.db"
VAULT_PATH="/share/witness_vault"
TOKEN_FILE="/config/api_token"
# The fleet roll-call summary: event_mqtt_bridge writes it and the kernel's
# GET /api/fleet reads it (schema securacv/fleet_peers/v1), which is how an
# add-on install lists its Canaries for the Witness Wall. It lives in
# /config beside the database and the token because that directory exists
# before the first process starts (the bridge's atomic write never creates
# a parent directory), it is rw-mapped, and HA backups capture it — on
# purpose: the file holds the public key pinned on first sight for every
# Canary this install has heard, so a restore keeps that trust instead of
# re-pinning every device. It also carries per-room wellbeing words, which
# is why the bridge writes it 0600. Both kernel config blocks below and the
# bridge's argv read this one variable, so they can never name two files.
FLEET_PEERS_FILE="/config/fleet_peers.json"
DEVICE_KEY_FILE="/config/.securacv/device_key"
INGRESS_PORT="${INGRESS_PORT:-8788}"
RUN_DIR="/run/securacv"
SETUP_FILE="$RUN_DIR/setup.sh"
CONFIGURED_MARKER="$RUN_DIR/configured"
BLUEPRINT_SRC="/usr/share/securacv/blueprints"
BLUEPRINT_DEST="/config/blueprints/automation/securacv"

# shellcheck source=privacy_witness_kernel/run_lib.sh disable=SC1091
source /usr/local/bin/run_lib.sh

# Log startup
bashio::log.info "SecuraCV: preparing the app..."

# The Supervisor provides SUPERVISOR_TOKEN directly; HASSIO_TOKEN is the
# deprecated alias and only a fallback. Never let the alias clobber the
# real token — on modern Supervisors HASSIO_TOKEN can be empty, and
# overwriting SUPERVISOR_TOKEN with it broke every Supervisor API call
# (wizard saves, discovery, restarts).
export SUPERVISOR_TOKEN="${SUPERVISOR_TOKEN:-${HASSIO_TOKEN:-}}"

# A fresh run directory: nothing from a previous start survives, so a
# service can never read a stale gate or a stale argv.
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR"
chmod 700 "$RUN_DIR"

# ============================================================================
# Bundled blueprints → /config/blueprints/automation/securacv. Missing or
# app-owned-and-unedited copies follow the image; an edited copy is kept.
# ============================================================================
if [ -d "$BLUEPRINT_SRC" ]; then
    while IFS= read -r line; do
        bashio::log.info "Blueprints: $line"
    done < <(install_blueprints "$BLUEPRINT_SRC" "$BLUEPRINT_DEST")
fi

# ============================================================================
# The carried integration (D15): update the app's own copy when the image
# carries a newer version; never a first install, never a HACS or edited
# copy, never a Core restart — the CLI raises a persistent notification
# asking for one. Skipped entirely when the option is off.
# ============================================================================
INSTALL_INTEGRATION="true"
if bashio::config.has_value 'install_integration'; then
    INSTALL_INTEGRATION=$(bashio::config 'install_integration')
fi
if [ "$INSTALL_INTEGRATION" = "true" ]; then
    carry_result=$(/usr/bin/python3 /usr/local/bin/integration_carry.py on-start --notify 2>&1 || true)
    bashio::log.info "Integration: ${carry_result}"
else
    bashio::log.info "Integration: install_integration is off; the app leaves /config/custom_components alone (HACS manages it)."
fi

# ============================================================================
# First-run detection: the device key is auto-generated, so its absence no
# longer marks a fresh install. The app is considered configured when the
# user has made any deliberate choice: a key (option or wizard-written file),
# frigate mode, or manually configured cameras. Otherwise only the setup
# wizard (the panel service) runs, so the kernel doesn't crash-loop on
# factory defaults.
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
    bashio::log.info "=== First-run detected: only the setup wizard runs (port ${INGRESS_PORT}) ==="
    bashio::log.info "Open the app's Web UI to complete setup."
    {
        declare -p MODE
        printf 'CONFIGURED="false"\n'
    } > "$SETUP_FILE"
    chmod 600 "$SETUP_FILE"
    exit 0
fi

# Resolve the device key: option → wizard-written key file → auto-generate
# and persist (0600). /config is captured by HA backups; the panel reminds
# the user to back the key up.
DEVICE_KEY_SEED=$(resolve_device_key_seed "$DEVICE_KEY_SEED" "$DEVICE_KEY_FILE")
if [ -z "$DEVICE_KEY_SEED" ]; then
    bashio::log.error "Failed to resolve or generate a device key seed."
    exit 1
fi

# The public device id every producer shares (run_lib.sh ha_device_id):
# the Supervisor discovery payload, the bridge's MQTT discovery and the
# kernel's own /api/identity all name this seed the same way. Computed
# here, once, before anything announces it.
DEVICE_ID=$(ha_device_id "$DEVICE_KEY_SEED")

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

# The kernel is pointed at the roll-call file only while a bridge keeps it.
# With publishing off, both api blocks omit fleet_peers_path, so GET
# /api/fleet lists this kernel alone; rows a past bridge pinned are neither
# served nor deleted (their first-sight keys wait for publishing to return).
FLEET_PEERS_API=""
if [ "$MQTT_PUBLISH_ENABLED" = "true" ]; then
    FLEET_PEERS_API=", \"fleet_peers_path\": \"$FLEET_PEERS_FILE\""
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

# Common environment of every kernel process (exported into setup.sh below).
export DEVICE_KEY_SEED
export WITNESS_CONFIG="$CONFIG_FILE"
export WITNESS_VAULT_PATH="$VAULT_PATH"
export RUST_LOG="witness_kernel=$LOG_LEVEL"

bashio::log.info "Mode: $MODE"
bashio::log.info "Database: $DB_PATH"
bashio::log.info "Retention: $RETENTION_DAYS days"
bashio::log.info "Time bucket: $TIME_BUCKET_MIN minutes"
bashio::log.info "Device ID: $DEVICE_ID"
if [ "$MQTT_PUBLISH_ENABLED" = "true" ]; then
    bashio::log.info "Fleet peers: $FLEET_PEERS_FILE (the MQTT publisher writes it; GET /api/fleet lists the Canaries it hears)"
else
    bashio::log.info "Fleet peers: MQTT publishing is disabled, so no bridge listens for Canaries; GET /api/fleet lists this kernel only (a roll-call file from an earlier run is kept for its pins, not served)."
fi

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

# The Supervisor-internal hostname the discovery payload and the MQTT
# announce carry (contract C1/C6). Not a LAN address.
ADDON_HOST=$(bashio::addon.hostname 2>/dev/null || hostname)

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
    "token_path": "$TOKEN_FILE"$FLEET_PEERS_API
  },
  "retention": {
    "seconds": $RETENTION_SECS
  }
}
EOF
}

# ============================================================================
# The MQTT publisher's argv (the event_mqtt_bridge service execs it).
# ============================================================================
PUBLISHER_CMD_ARRAY=()
PUBLISHER_MQTT_PASSWORD=""
PUBLISH_PREFIX="witness"
prepare_mqtt_publisher() {
    if [ "$MQTT_PUBLISH_ENABLED" != "true" ]; then
        return
    fi

    # Read MQTT publish configuration: explicit option → discovered → default
    local PUBLISH_HOST PUBLISH_PORT DISCOVERY_PREFIX
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
    fi

    if bashio::config.has_value 'mqtt_publish.discovery_prefix'; then
        DISCOVERY_PREFIX=$(bashio::config 'mqtt_publish.discovery_prefix')
    else
        DISCOVERY_PREFIX="homeassistant"
    fi

    # Scheduled sealed-log verification cadence (hours; 0 disables). Hourly
    # by default so the kernel's self-report stays current beside the
    # integration's own verification.
    local VERIFY_INTERVAL_HOURS=1
    if bashio::config.has_value 'verify_interval_hours'; then
        VERIFY_INTERVAL_HOURS=$(bashio::config 'verify_interval_hours')
    fi

    # Build command using array for safe argument handling. The fleet-peers
    # element is the roll-call: with it the bridge subscribes to each
    # Canary's availability/status/health/chain/state/meta topics (never
    # events or sensing) and keeps the summary the kernel's GET /api/fleet
    # reads. Both kernel config blocks name the same file — one variable.
    # The announce flags (contract C1): the API binds every interface inside
    # the container, HA reads the rotating token file (no pairing), and the
    # host is the Supervisor-internal name.
    PUBLISHER_CMD_ARRAY=(
        "/usr/local/bin/event_mqtt_bridge"
        --daemon
        --allow-remote-mqtt
        --mqtt-broker-addr "$PUBLISH_HOST:$PUBLISH_PORT"
        --mqtt-topic-prefix "$PUBLISH_PREFIX"
        --ha-discovery-prefix "$DISCOVERY_PREFIX"
        --ha-device-id "$DEVICE_ID"
        --api-token-path "$TOKEN_FILE"
        --fleet-peers-path "$FLEET_PEERS_FILE"
        --poll-interval 30
        --verify-interval-secs "$((VERIFY_INTERVAL_HOURS * 3600))"
        --announce-api-bind all
        --announce-pairing token_file
        --announce-api-host "$ADDON_HOST"
    )

    if [ -n "$PUBLISH_USER" ]; then
        PUBLISHER_CMD_ARRAY+=(--mqtt-username "$PUBLISH_USER")
    fi

    # The password travels in the environment, never on argv: argv is readable
    # from /proc/<pid>/cmdline by anything in the container and shows up in
    # process listings and crash reports. event_mqtt_bridge reads MQTT_PASSWORD
    # itself (clap `env`), the same way it already takes DEVICE_KEY_SEED. The
    # service sets it for that one child only: in Frigate mode the frigate
    # bridge may talk to a different broker with a different password.
    PUBLISHER_MQTT_PASSWORD="$PUBLISH_PASS"

    bashio::log.info "MQTT Publisher: $PUBLISH_HOST:$PUBLISH_PORT"
    bashio::log.info "  Topic prefix: $PUBLISH_PREFIX"
    bashio::log.info "  Discovery prefix: $DISCOVERY_PREFIX"
    bashio::log.info "  Fleet peers file: $FLEET_PEERS_FILE"
}

prepare_mqtt_publisher

# ============================================================================
# FRIGATE MODE
# ============================================================================
FRIGATE_CMD_ARRAY=()
FRIGATE_MQTT_PASSWORD=""
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

    # Get cameras and labels as comma-separated strings. Cameras are the
    # shared slugs the wizard wrote (contract C8); the bridge slugs both
    # sides before comparing, so a hand-typed name still meets Frigate's.
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
    bashio::log.info "Seals: $([ "$ENABLE_REVIEWS" = "true" ] && echo "only Frigate alerts (reviews)" || echo "every tracked object (events)")"

    # Build frigate_bridge command using array for safe argument handling
    # Note: --allow-remote-mqtt is safe in HA because:
    # 1. The MQTT broker (core-mosquitto) is a trusted HA component
    # 2. All event data is still sanitized before logging
    # 3. No raw media ever flows through this path
    # The state prefix and device id are the publisher's (contract C5): the
    # bridge's retained `<prefix>/frigate_link` must land beside the
    # publisher's state topics and name the same `pwk_` id, or Home
    # Assistant shows the Frigate link on a device it does not have.
    FRIGATE_CMD_ARRAY=(
        "/usr/local/bin/frigate_bridge"
        --allow-remote-mqtt
        --mqtt-broker-addr "$MQTT_HOST:$MQTT_PORT"
        --frigate-topic-prefix "$TOPIC_PREFIX"
        --mqtt-topic-prefix "$PUBLISH_PREFIX"
        --ha-device-id "$DEVICE_ID"
        --db-path "$DB_PATH"
        --bucket-size-secs "$BUCKET_SECS"
        --min-confidence "$MIN_CONFIDENCE"
        --retention-secs "$RETENTION_SECS"
    )

    # Legacy full-topic override wins over the prefix when explicitly set.
    if [ -n "$OPT_FRIGATE_TOPIC" ]; then
        FRIGATE_CMD_ARRAY+=(--frigate-topic "$OPT_FRIGATE_TOPIC")
    fi

    if [ "$ENABLE_REVIEWS" = "true" ]; then
        FRIGATE_CMD_ARRAY+=(--enable-reviews)
    fi

    if [ -n "$MQTT_USER" ]; then
        FRIGATE_CMD_ARRAY+=(--mqtt-username "$MQTT_USER")
    fi

    # The broker password reaches frigate_bridge through the environment,
    # never argv — the service sets it right at its exec, so it is THIS
    # broker's password and not the publisher's.
    FRIGATE_MQTT_PASSWORD="$MQTT_PASS"

    if [ -n "$FRIGATE_CAMERAS" ]; then
        FRIGATE_CMD_ARRAY+=(--cameras "$FRIGATE_CAMERAS")
    fi

    if [ -n "$FRIGATE_LABELS" ]; then
        FRIGATE_CMD_ARRAY+=(--labels "$FRIGATE_LABELS")
    fi

    write_frigate_api_config
    bashio::log.info "Configuration written to $CONFIG_FILE (witness_api + frigate_bridge start next)"

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
        bashio::log.error "  1. Add cameras manually in the app configuration"
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
    "token_path": "$TOKEN_FILE"$FLEET_PEERS_API
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

    bashio::log.info "Configuration written to $CONFIG_FILE (witnessd starts next)"
fi

# ============================================================================
# Hand everything to the services. `declare -p` serializes the arrays and
# keeps the exports exported; the file is root-only on tmpfs because it
# carries the seed and the broker passwords.
# ============================================================================
{
    printf '# Rendered by run.sh at container start; sourced by the s6 services.\n'
    printf 'CONFIGURED="true"\n'
    declare -p MODE DEVICE_ID ADDON_HOST TOKEN_FILE PUBLISH_PREFIX \
        MQTT_PUBLISH_ENABLED INSTALL_INTEGRATION
    declare -p DEVICE_KEY_SEED WITNESS_CONFIG WITNESS_VAULT_PATH RUST_LOG \
        WITNESS_API_ALLOW_INSECURE
    declare -p PUBLISHER_CMD_ARRAY PUBLISHER_MQTT_PASSWORD
    declare -p FRIGATE_CMD_ARRAY FRIGATE_MQTT_PASSWORD
} > "$SETUP_FILE"
chmod 600 "$SETUP_FILE"
touch "$CONFIGURED_MARKER"
bashio::log.info "Setup complete; starting the services."
