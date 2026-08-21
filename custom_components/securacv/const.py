"""Constants for the SecuraCV integration.

Supports multi-transport resilience architecture - Canary devices use ANY
available transport to get witness data out before being silenced.
"""

DOMAIN = "securacv"

# Config keys
CONF_MQTT_PREFIX = "mqtt_prefix"
CONF_ENABLE_MQTT = "enable_mqtt"
CONF_SETUP_MODE = "setup_mode"
# Optional URL of an adapter_host read-only stats endpoint (e.g. http://host:8799).
CONF_ADAPTER_STATS_URL = "adapter_stats_url"
# Optional path to the kernel's rotating capability-token file. The kernel
# rotates the token every 10 minutes and rewrites this file; configuring the
# path (instead of, or alongside, a static token) lets the integration re-read
# it on 401 and survive the rotation. The Privacy Witness Kernel add-on writes
# it to /config/api_token, which HA can read directly (shared /config volume).
CONF_TOKEN_FILE = "token_file"
DEFAULT_TOKEN_FILE = "/config/api_token"

# Setup modes
# `auto` is a flow-only choice: it probes for a running kernel and stores a
# concrete mode (both / mqtt) in the entry — no entry ever persists "auto".
SETUP_MODE_AUTO = "auto"
SETUP_MODE_MQTT = "mqtt"
SETUP_MODE_KERNEL = "kernel"
SETUP_MODE_BOTH = "both"

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
# The retained per-variant snapshot. This — not `presence` — is where the
# firmware actually publishes presence: canary-sense's topics.h builds
# `securacv/<id>/state`, and its own HA discovery reads `value_json.presence`.
TOPIC_STATE = "state"

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
]

# Declared for forward-compatibility but NOT implemented on any device firmware.
# Kept out of ALL_TRANSPORTS so the integration never advertises a transport that
# no device can ever report on. Move an entry up to ALL_TRANSPORTS only once it is
# actually wired end-to-end.
FUTURE_TRANSPORTS = [
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
]

# Declared for forward-compatibility but NOT implemented: no firmware emits these.
# Kept out of ALL_TAMPER_TYPES so the integration never advertises a tamper type no
# device can raise.
FUTURE_TAMPER_TYPES = [
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
# The kernel add-on's hostname as Supervisor DNS actually publishes it: the
# full slug (repo hash + name) with underscores mapped to dashes. The bare
# "privacy_witness_kernel" never resolved from HA core.
DEFAULT_KERNEL_URL = "http://d0491a67-privacy-witness-kernel:8799"
DEFAULT_MQTT_PREFIX = "securacv"

# =============================================================================
# Health Thresholds
# =============================================================================
CRITICAL_MEMORY_THRESHOLD_BYTES = 10000  # < 10KB free heap = critical
WARNING_MEMORY_THRESHOLD_BYTES = 1024    # < 1KB free = warning in health sensor
CRITICAL_BATTERY_THRESHOLD_PERCENT = 10  # < 10% battery = critical
WARNING_BATTERY_THRESHOLD_PERCENT = 25   # < 25% battery = warning

# =============================================================================
# SCQCS - Secure Coded Witness Squawk System (Future)
# Audio-based last-gasp communication when all other transports fail
# =============================================================================
SCQCS_PANIC = "panic"           # Emergency tone burst
SCQCS_TAMPER = "tamper"         # Tamper alert tone
SCQCS_WITNESS = "witness"       # Witness event tone
SCQCS_HEARTBEAT = "heartbeat"   # Periodic chirp for presence

# =============================================================================
# Witness Event Type Metadata
# Maps coarse, privacy-preserving event_type claims (emitted by the kernel and
# its sensor adapters - see spec/sensor_adapter_contract_v0.md) to a friendly
# label and Material Design icon for the Home Assistant "single pane of glass".
# These are claims, never identities: no plates, faces, or precise location.
# =============================================================================
EVENT_TYPE_METADATA = {
    "boundary_crossing_object_large": {
        "label": "Large object crossed boundary",
        "icon": "mdi:car",
    },
    "boundary_crossing_object_small": {
        "label": "Small object crossed boundary",
        "icon": "mdi:paw",
    },
    "acoustic_impulse_in_zone": {
        "label": "Acoustic impulse in zone",
        "icon": "mdi:waveform",
    },
    "presence_in_restricted_zone": {
        "label": "Presence in restricted zone",
        "icon": "mdi:account-alert",
    },
    "vehicle_presence_after_hours": {
        "label": "Vehicle presence after hours",
        "icon": "mdi:car-clock",
    },
    "contact_state_change": {
        "label": "Contact state change",
        "icon": "mdi:door",
    },
    "object_removed_from_zone": {
        "label": "Object removed from zone",
        "icon": "mdi:package-variant-closed-remove",
    },
    # The kernel emits TamperDetected (EventType) / the adapter tamper route
    # seals it; without this entry a tamper event rendered with the default
    # icon. Canonical label/icon live in spec/witness_dictionary.json.
    "tamper_detected": {
        "label": "Tamper detected",
        "icon": "mdi:shield-alert",
    },
    "vehicle_arrival_departure": {
        "label": "Vehicle arrival/departure",
        "icon": "mdi:car-side",
    },
}

DEFAULT_EVENT_ICON = "mdi:shield-eye"

# =============================================================================
# Sensing modality (Phase 3 — canary-sense / mixed-fleet timeline)
# =============================================================================
# A witness event carries an optional `modality` so the dashboard can show, at
# a glance, *what kind of sensor* produced a claim — radar presence reads very
# differently from a camera person-detection even when both map to the same
# coarse `PresenceInRestrictedZone` event_type. The field is advisory metadata
# only (never an identity surface); when absent the timeline renders exactly as
# it did before (no indicator).
#
# Wire contract (firmware + Rust adapter must match these literals):
#   - Preferred: an explicit `modality` string on the event payload, one of the
#     MODALITY_* values below.
#   - Fallback: the device's `device_type` (status/discovery payload) maps via
#     DEVICE_TYPE_MODALITY — so a device that only advertises device_type still
#     gets a correct glyph without changing every event publish.
MODALITY_CAMERA = "camera"        # on-module image inference (canary-vision)
MODALITY_WIFI_CSI = "wifi-csi"    # WiFi channel-state distortion (canary-wap)
MODALITY_RADAR = "radar"          # 60GHz FMCW mmWave (canary-sense / MR60BHA2)
MODALITY_CONTACT = "contact"      # reed/contact switch, door/window
MODALITY_OTHER = "other"          # a known-but-uncategorized sensing medium
MODALITY_UNKNOWN = "unknown"      # no modality info — render as before

# device_type (status payload) -> modality. The canary-sense design (§5) keys
# radar device awareness off `device_type: "canary-sense"`; the sibling vision
# and WiFi-CSI projects use the same status field, so they map here too.
DEVICE_TYPE_MODALITY = {
    "canary-sense": MODALITY_RADAR,
    "canary-vision": MODALITY_CAMERA,
    "canary-wap": MODALITY_WIFI_CSI,
    "canary-contact": MODALITY_CONTACT,
}

# {label, icon} per modality, for HA entity attrs and (mirrored) the JS card.
MODALITY_METADATA = {
    MODALITY_CAMERA: {"label": "Camera", "icon": "mdi:camera"},
    MODALITY_WIFI_CSI: {"label": "WiFi CSI", "icon": "mdi:wifi"},
    MODALITY_RADAR: {"label": "Radar", "icon": "mdi:radar"},
    MODALITY_CONTACT: {"label": "Contact", "icon": "mdi:electric-switch"},
    MODALITY_OTHER: {"label": "Other sensor", "icon": "mdi:access-point"},
}

# Canonical device_type literal for the MR60BHA2 radar witness (Track A + B).
DEVICE_TYPE_CANARY_SENSE = "canary-sense"

# Canonical device_type literal for the design-stage pool water-chemistry node
# (docs/research/pool_water_monitor.md). Reserved here so the name has ONE
# spelling across the firmware config, the Dash card layer, and this
# integration. Deliberately NOT added to DEVICE_TYPE_MODALITY: a pool node
# reports water chemistry, not a presence claim, so it has no sensing modality
# on the timeline — modality_for() correctly resolves it to MODALITY_UNKNOWN.
DEVICE_TYPE_CANARY_POOL = "canary-pool"


def normalize_modality(value: str | None) -> str:
    """Coerce a free-form modality string to a known MODALITY_* literal.

    Accepts a few common spellings (``wifi_csi``/``csi`` → ``wifi-csi``,
    ``mmwave``/``mmwave_radar`` → ``radar``) so the adapter and firmware sides
    have a little latitude, but anything unrecognized degrades to
    ``MODALITY_UNKNOWN`` rather than inventing a glyph. Unknown is the
    backward-compatible "render as before" sentinel.
    """
    if not value or not isinstance(value, str):
        return MODALITY_UNKNOWN
    key = value.strip().lower().replace("_", "-")
    if key in MODALITY_METADATA:
        return key
    aliases = {
        "csi": MODALITY_WIFI_CSI,
        "wifi": MODALITY_WIFI_CSI,
        "mmwave": MODALITY_RADAR,
        "mmwave-radar": MODALITY_RADAR,
        "60ghz": MODALITY_RADAR,
        "reed": MODALITY_CONTACT,
        "door": MODALITY_CONTACT,
    }
    return aliases.get(key, MODALITY_UNKNOWN)


def canonical_device_type(value) -> str | None:
    """Canonical device_type: lowercase, hyphenated. Firmware configs have
    shipped both "canary-sense" and "canary_sense" spellings; treat them as
    one so modality fallback and type-gated entities never miss on a dash."""
    if not isinstance(value, str):
        return None
    return value.strip().lower().replace("_", "-") or None


def modality_for(event: dict | None, device_type: str | None = None) -> str:
    """Resolve the sensing modality for an event.

    Priority: explicit ``event['modality']`` → the event's own ``device_type``
    → the supplied (device-level) ``device_type``. Returns ``MODALITY_UNKNOWN``
    when nothing resolves, so callers can omit the indicator entirely and keep
    pre-Phase-3 rendering for events that carry no modality hint.
    """
    if isinstance(event, dict):
        explicit = normalize_modality(event.get("modality"))
        if explicit != MODALITY_UNKNOWN:
            return explicit
        ev_dtype = canonical_device_type(event.get("device_type"))
        if ev_dtype in DEVICE_TYPE_MODALITY:
            return DEVICE_TYPE_MODALITY[ev_dtype]
    dtype = canonical_device_type(device_type)
    if dtype in DEVICE_TYPE_MODALITY:
        return DEVICE_TYPE_MODALITY[dtype]
    return MODALITY_UNKNOWN


def modality_metadata(modality: str | None) -> dict[str, str] | None:
    """{label, icon} for a modality, or None for unknown/unset (no indicator)."""
    if not modality:
        return None
    return MODALITY_METADATA.get(normalize_modality(modality))


# =============================================================================
# Attestation provenance (Phase 3 — honest trust-badge differentiation)
# =============================================================================
# The trust badge already distinguishes verified / unverified / failed. The
# canary-sense design (§3 Track B) adds a fourth, orthogonal axis: *who signed
# the claim*. Track A devices sign on-device (Ed25519, device-attested); Track B
# kit deployments are signed by the kernel/adapter at ingest, and the
# statestream path transits Home Assistant first. The badge must say so rather
# than implying a device cryptographically vouched for the claim.
#
# Wire contract: an optional `attestation` string on the event payload, one of:
ATTESTATION_DEVICE = "device"          # device-signed at source (Track A) — default
ATTESTATION_ADAPTER = "adapter"        # kernel/adapter-signed at ingest (Track B)
ATTESTATION_HA_BRIDGED = "ha-bridged"  # claim transited HA before ingest (statestream)

ATTESTATION_METADATA = {
    ATTESTATION_DEVICE: {"label": "Device-attested", "icon": "mdi:chip"},
    ATTESTATION_ADAPTER: {"label": "Adapter-attested", "icon": "mdi:hub"},
    ATTESTATION_HA_BRIDGED: {"label": "HA-bridged", "icon": "mdi:home-assistant"},
}


def normalize_attestation(value: str | None) -> str:
    """Coerce an attestation string to a known literal; default ``device``.

    Absent / unrecognized values resolve to ``ATTESTATION_DEVICE`` so existing
    device-signed events keep their current (green ✓) behavior unchanged — only
    a payload that *explicitly* says ``adapter``/``ha-bridged`` gets the
    distinct, weaker provenance.
    """
    if not value or not isinstance(value, str):
        return ATTESTATION_DEVICE
    key = value.strip().lower().replace("_", "-")
    if key in ATTESTATION_METADATA:
        return key
    if key in ("kernel", "ingest", "adapter-attested"):
        return ATTESTATION_ADAPTER
    if key in ("ha", "bridged", "statestream"):
        return ATTESTATION_HA_BRIDGED
    return ATTESTATION_DEVICE


def event_type_metadata(event_type: str | None) -> dict[str, str]:
    """Return {'label', 'icon'} for an event_type, with a sensible default.

    Accepts both snake_case (e.g. "acoustic_impulse_in_zone") and the kernel's
    CamelCase enum form (e.g. "AcousticImpulseInZone").
    """
    if not event_type:
        return {"label": "Unknown", "icon": DEFAULT_EVENT_ICON}
    key = event_type.strip()
    if key not in EVENT_TYPE_METADATA:
        # Normalize CamelCase -> snake_case so kernel enum names also resolve.
        snake = ""
        for i, ch in enumerate(key):
            if ch.isupper() and i > 0:
                snake += "_"
            snake += ch.lower()
        key = snake
    meta = EVENT_TYPE_METADATA.get(key)
    if meta is None:
        return {"label": event_type, "icon": DEFAULT_EVENT_ICON}
    return meta


# =============================================================================
# Apple Home projection — the egress vocabulary
# =============================================================================
# Which HomeKit signals an event asserts. This is a MIRROR of
# `signals_for_event` in src/bridge/homekit.rs, and both are generated from
# `homekit_projection.event_signals` in spec/witness_dictionary.json;
# scripts/lint_dictionary_sync.py fails CI on any drift between the three.
#
# Deliberately partial. An event with no honest HAP counterpart maps to an
# empty tuple rather than being forced into a sensor that would misdescribe
# it, and the two empties are empty for different reasons:
#
#   - AcousticImpulseInZone: HAP has no acoustic sensor, and publishing a
#     sound as "motion" would be a false statement about someone's home.
#   - ContactStateChange: it reports that a contact CHANGED, not what it
#     changed to. HAP's contact characteristic is a state, so deriving one
#     from the other would show a door as open every time it was closed.
#
# Keys are the kernel's CamelCase enum names, as they arrive on MQTT.
HOMEKIT_EVENT_SIGNALS = {
    "BoundaryCrossingObjectLarge": ("motion",),
    "BoundaryCrossingObjectSmall": ("motion",),
    "ObjectRemovedFromZone": ("motion",),
    "VehiclePresenceAfterHours": ("motion",),
    "VehicleArrivalDeparture": ("motion",),
    "PresenceInRestrictedZone": ("occupancy",),
    "TamperDetected": ("tamper",),
    "AcousticImpulseInZone": (),
    "ContactStateChange": (),
}

# How long a motion signal stays asserted after the event that raised it.
# Matches the kernel's default hold window (10 ticks x 1 s), which in turn
# matches the ~10 s dwell commodity HomeKit motion sensors use — so an
# automation written against a Canary behaves like one written against a
# sensor the user already owns.
HOMEKIT_MOTION_HOLD_SECONDS = 10


def homekit_signals_for_event(event_type: str) -> tuple[str, ...]:
    """Return the HomeKit signals an event asserts, or () for none.

    Accepts the kernel's CamelCase enum names and the snake_case form, so a
    payload from either producer resolves. An unrecognized event asserts
    NOTHING rather than falling back to motion — inventing a claim about
    someone's home is worse than staying quiet.
    """
    if not event_type:
        return ()
    key = event_type.strip()
    if key in HOMEKIT_EVENT_SIGNALS:
        return HOMEKIT_EVENT_SIGNALS[key]
    # snake_case -> CamelCase, so "boundary_crossing_object_large" resolves.
    camel = "".join(part[:1].upper() + part[1:] for part in key.split("_"))
    return HOMEKIT_EVENT_SIGNALS.get(camel, ())
