"""Constants for the SecuraCV integration.

Supports multi-transport resilience architecture - Canary devices use ANY
available transport to get witness data out before being silenced.
"""

DOMAIN = "securacv"

# Config keys
CONF_MQTT_PREFIX = "mqtt_prefix"
CONF_ENABLE_MQTT = "enable_mqtt"

# =============================================================================
# MQTT Topics (relative to prefix/{device_id}/)
# =============================================================================
TOPIC_STATUS = "status"
TOPIC_EVENTS = "events"
TOPIC_HEALTH = "health"
TOPIC_CHAIN = "chain"
TOPIC_COUNTS = "counts"
TOPIC_COMMAND = "command"
TOPIC_TAMPER = "tamper"
TOPIC_MESH = "mesh"
TOPIC_CHIRP = "chirp"
TOPIC_TRANSPORT = "transport"
TOPIC_PRESENCE = "presence"

# =============================================================================
# Transport Types - Multi-path resilience
# Canary will use ANY available transport to communicate witness data
# =============================================================================
TRANSPORT_WIFI_AP = "wifi_ap"       # Direct WiFi AP mode
TRANSPORT_WIFI_STA = "wifi_sta"     # WiFi station (to router)
TRANSPORT_MQTT = "mqtt"             # MQTT broker connection
TRANSPORT_BLE = "ble"               # Bluetooth Low Energy
TRANSPORT_MESH = "mesh"             # Opera mesh network (peer-to-peer)
TRANSPORT_CHIRP = "chirp"           # Community alert network
TRANSPORT_LORA = "lora"             # Future: LoRa radio
TRANSPORT_AUDIO = "audio"           # Future: SCQCS audio squawks

ALL_TRANSPORTS = [
    TRANSPORT_WIFI_AP,
    TRANSPORT_WIFI_STA,
    TRANSPORT_MQTT,
    TRANSPORT_BLE,
    TRANSPORT_MESH,
    TRANSPORT_CHIRP,
    TRANSPORT_LORA,
    TRANSPORT_AUDIO,
]

# =============================================================================
# Tamper Event Types - Survivability triggers
# Device attempts to log/broadcast witness data before being silenced
# =============================================================================
TAMPER_POWER_LOSS = "power_loss"           # Power removed/brownout
TAMPER_BATTERY_REMOVE = "battery_remove"   # Battery disconnected
TAMPER_SD_REMOVE = "sd_remove"             # SD card removed
TAMPER_SD_ERROR = "sd_error"               # SD card write failure
TAMPER_GPS_JAMMING = "gps_jamming"         # GPS signal lost/jammed
TAMPER_GPS_SPOOF = "gps_spoof"             # GPS location spoofing detected
TAMPER_MOTION = "motion"                   # Unexpected movement (accelerometer)
TAMPER_ENCLOSURE = "enclosure"             # Physical enclosure opened
TAMPER_CAPACITIVE = "capacitive"           # Capacitive touch on unused pins
TAMPER_GPIO = "gpio"                       # GPIO tamper pin triggered
TAMPER_WATCHDOG = "watchdog"               # System hang detected
TAMPER_REBOOT = "unexpected_reboot"        # Unexpected reboot
TAMPER_MEMORY = "memory_critical"          # Critical memory exhaustion
TAMPER_AUDIO = "audio_anomaly"             # Future: unusual audio (jamming?)

ALL_TAMPER_TYPES = [
    TAMPER_POWER_LOSS,
    TAMPER_BATTERY_REMOVE,
    TAMPER_SD_REMOVE,
    TAMPER_SD_ERROR,
    TAMPER_GPS_JAMMING,
    TAMPER_GPS_SPOOF,
    TAMPER_MOTION,
    TAMPER_ENCLOSURE,
    TAMPER_CAPACITIVE,
    TAMPER_GPIO,
    TAMPER_WATCHDOG,
    TAMPER_REBOOT,
    TAMPER_MEMORY,
    TAMPER_AUDIO,
]

# =============================================================================
# Health Log Categories (from firmware health_log.h)
# =============================================================================
HEALTH_CAT_SYSTEM = "system"         # Boot, shutdown, watchdog
HEALTH_CAT_CRYPTO = "crypto"         # Key generation, signing, verification
HEALTH_CAT_CHAIN = "chain"           # Hash chain operations
HEALTH_CAT_GPS = "gps"               # GNSS fix, satellites, time sync
HEALTH_CAT_STORAGE = "storage"       # SD card, NVS operations
HEALTH_CAT_NETWORK = "network"       # WiFi, HTTP server
HEALTH_CAT_SENSOR = "sensor"         # PIR, tamper, environmental
HEALTH_CAT_USER = "user"             # User actions
HEALTH_CAT_WITNESS = "witness"       # Witness record creation
HEALTH_CAT_MESH = "mesh"             # Mesh network operations
HEALTH_CAT_BLUETOOTH = "bluetooth"   # BLE operations
HEALTH_CAT_CHIRP = "chirp"           # Chirp channel operations

# Health Log Severity Levels (escalating)
HEALTH_LEVEL_DEBUG = "debug"
HEALTH_LEVEL_INFO = "info"
HEALTH_LEVEL_NOTICE = "notice"
HEALTH_LEVEL_WARNING = "warning"
HEALTH_LEVEL_ERROR = "error"
HEALTH_LEVEL_CRITICAL = "critical"
HEALTH_LEVEL_ALERT = "alert"
HEALTH_LEVEL_TAMPER = "tamper"       # Highest priority - security/integrity

# =============================================================================
# Chirp Network - Community witness alerts
# Ephemeral identities, templated messages only, 3-hop relay
# =============================================================================
CHIRP_CAT_AUTHORITY = "authority"    # Police, federal, helicopters, road blocks
CHIRP_CAT_INFRASTRUCTURE = "infra"   # Power, water, gas, internet, road issues
CHIRP_CAT_EMERGENCY = "emergency"    # Fire, medical, evacuation, shelter
CHIRP_CAT_WEATHER = "weather"        # Severe weather, tornado, flood
CHIRP_CAT_MUTUAL_AID = "mutual_aid"  # Welfare checks, supplies, offerings
CHIRP_CAT_ALL_CLEAR = "all_clear"    # De-escalation

# Chirp message templates (no free text - abuse prevention)
CHIRP_TEMPLATES = {
    # Authority presence
    "auth_police": "Police presence observed",
    "auth_federal": "Federal agents observed",
    "auth_helicopter": "Helicopter overhead",
    "auth_roadblock": "Road block ahead",
    # Infrastructure
    "infra_power": "Power outage",
    "infra_water": "Water service issue",
    "infra_gas": "Gas leak/smell",
    "infra_internet": "Internet outage",
    "infra_road": "Road hazard",
    # Emergency
    "emerg_fire": "Fire/smoke visible",
    "emerg_medical": "Medical emergency",
    "emerg_evacuation": "Evacuation in progress",
    "emerg_shelter": "Shelter-in-place",
    # Weather
    "weather_severe": "Severe weather",
    "weather_tornado": "Tornado warning",
    "weather_flood": "Flooding",
    "weather_lightning": "Lightning storm",
    # Mutual aid
    "aid_welfare": "Welfare check needed",
    "aid_supplies": "Supplies needed",
    "aid_offering": "Supplies available",
    # Status
    "all_clear": "All clear",
}

# =============================================================================
# Mesh Network (Opera Protocol)
# Ed25519 authenticated, ChaCha20-Poly1305 encrypted peer network
# =============================================================================
MESH_MSG_DISCOVERY = "discovery"
MESH_MSG_PAIRING_REQUEST = "pairing_request"
MESH_MSG_PAIRING_RESPONSE = "pairing_response"
MESH_MSG_PAIRING_CONFIRM = "pairing_confirm"
MESH_MSG_HEARTBEAT = "heartbeat"
MESH_MSG_WITNESS_RECORD = "witness_record"
MESH_MSG_ALERT = "alert"

# =============================================================================
# Motion States (from GPS/accelerometer)
# =============================================================================
MOTION_STATE_NO_FIX = "no_fix"
MOTION_STATE_FIX_ACQUIRED = "fix_acquired"
MOTION_STATE_STATIONARY = "stationary"
MOTION_STATE_MOVING = "moving"
MOTION_STATE_FIX_LOST = "fix_lost"

# =============================================================================
# Future Sensors - Architecture is open for expansion
# =============================================================================
SENSOR_SMOKE_ALARM = "smoke_alarm"   # Audio detection of smoke/fire alarms
SENSOR_HEARTBEAT = "heartbeat"       # Person detection via audio/RF
SENSOR_VIBRATION = "vibration"       # Building/structure vibration
SENSOR_ACOUSTIC = "acoustic"         # General audio anomaly detection

# =============================================================================
# Attributes
# =============================================================================
ATTR_DEVICE_ID = "device_id"
ATTR_CHAIN_LENGTH = "chain_length"
ATTR_WITNESS_COUNT = "witness_count"
ATTR_LAST_EVENT = "last_event"
ATTR_LAST_EVENT_TIME = "last_event_time"
ATTR_GPS_FIX = "gps_fix"
ATTR_CHAIN_VALID = "chain_valid"
ATTR_TAMPER = "tamper_detected"
ATTR_BATTERY = "battery_level"
ATTR_MEMORY_FREE = "memory_free"
ATTR_UPTIME = "uptime"
ATTR_PUBLIC_KEY = "public_key"
ATTR_FIRMWARE_VERSION = "firmware_version"

# Transport health attributes
ATTR_TRANSPORT_STATUS = "transport_status"
ATTR_LAST_SEEN = "last_seen"
ATTR_MESSAGE_COUNT = "message_count"
ATTR_ERROR_COUNT = "error_count"
ATTR_RSSI = "rssi"
ATTR_PEERS = "peers"
ATTR_HOPS = "hops"

# Chirp attributes
ATTR_CHIRP_SESSION_ID = "session_emoji"  # 3-emoji ephemeral identity
ATTR_CHIRP_COOLDOWN_TIER = "cooldown_tier"
ATTR_CHIRP_CONFIRMATIONS = "confirmations"
ATTR_CHIRP_SUPPRESSED = "suppressed"

# =============================================================================
# Device info
# =============================================================================
MANUFACTURER = "ERRERlabs"
MODEL_KERNEL = "Privacy Witness Kernel"
MODEL_CANARY = "SecuraCV Canary"

# =============================================================================
# Default values
# =============================================================================
DEFAULT_KERNEL_URL = "http://privacy_witness_kernel:8799"
DEFAULT_MQTT_PREFIX = "securacv"

# =============================================================================
# SCQCS - Secure Coded Witness Squawk System (Future)
# Audio-based last-gasp communication when all other transports fail
# =============================================================================
SCQCS_PANIC = "panic"           # Emergency tone burst
SCQCS_TAMPER = "tamper"         # Tamper alert tone
SCQCS_WITNESS = "witness"       # Witness event tone
SCQCS_HEARTBEAT = "heartbeat"   # Periodic chirp for presence
