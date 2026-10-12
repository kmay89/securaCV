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

# =============================================================================
# Kernel identity, pairing and the TOFU pin (contracts C1, C2, C4, C6)
# =============================================================================
# How the integration authenticates to the witness kernel. `token_file` reads
# the kernel's rotating capability token off a shared volume (the app); `paired`
# holds a long-lived paired token minted by POST /api/pair from a 6-digit code
# (the Docker sidecar); `static` is a pasted capability token, which the kernel
# rotates away within ten minutes (remote kernels only).
CONF_AUTH_MODE = "auth_mode"
AUTH_MODE_TOKEN_FILE = "token_file"
AUTH_MODE_PAIRED = "paired"
AUTH_MODE_STATIC = "static"
# The kernel's device id (`pwk_<8 hex>`, served by GET /api/identity). It keys
# the config entry's unique_id (`kernel_<device_id>`), the device registry
# identifier and the retained claim topic.
CONF_KERNEL_DEVICE_ID = "kernel_device_id"
# The kernel's Ed25519 verifying key, 64 lowercase hex, pinned trust-on-first-
# use over the AUTHENTICATED HTTP channel (the /api/pair response, or
# /api/identity through the token file) — never from the retained MQTT announce.
CONF_KERNEL_VERIFYING_KEY = "kernel_verifying_key"
# Keys the pin was rotated away from (newest last), kept so a postmortem can
# recover the identity a sealed log was signed under before a rotation.
CONF_KERNEL_PREVIOUS_KEYS = "kernel_previous_verifying_keys"
# When the current pin was taken, as a 10-minute bucket start (coarse time,
# like every other "at" the integration keeps).
CONF_KERNEL_KEY_PINNED_BUCKET_START = "kernel_key_pinned_bucket_start"
# The paired token's id (8 hex), so async_remove_entry can revoke exactly its
# own token with DELETE /api/pair/<id>.
CONF_KERNEL_TOKEN_ID = "kernel_token_id"
# The bridge's state prefix: the topic tree the kernel's MQTT bridge publishes
# under (witness/last_event, witness/digest, witness/status, ...). Distinct from
# CONF_MQTT_PREFIX, which is the Canaries' prefix (securacv/<id>/...).
CONF_MQTT_TOPIC_PREFIX = "mqtt_topic_prefix"
DEFAULT_MQTT_TOPIC_PREFIX = "witness"
# The random per-entry nonce the retained claim carries instead of an entry id
# (an entry id is a stable per-installation identifier; the nonce is not).
CONF_CLAIM_NONCE = "claim_nonce"
# Set once the one-time `entities_migrated` Repairs notice has been raised.
CONF_MIGRATION_NOTICE_SHOWN = "migration_notice_shown"
# Set once the one-time "add the dashboard" notification has been posted.
CONF_DASHBOARD_NOTICE_SHOWN = "dashboard_notice_shown"
# The pairing form's code field (never persisted).
CONF_PAIRING_CODE = "pairing_code"
PAIRING_CODE_LENGTH = 6
# The label the kernel files the paired token under.
PAIRED_TOKEN_LABEL = "home-assistant"
# The port the kernel's event API listens on, for the pairing-step prefill.
DEFAULT_KERNEL_API_PORT = 8799
# Evidence exports kept on disk (options flow; the oldest zip goes first).
CONF_EVIDENCE_KEEP = "evidence_keep"
DEFAULT_EVIDENCE_KEEP = 20
MIN_EVIDENCE_KEEP = 1
MAX_EVIDENCE_KEEP = 200

# =============================================================================
# The integration's OWN verification (D10) — vocabulary and cadence
# =============================================================================
# `verification` on every kernel event row. "verified" is earned only by the
# integration's chain walk (Ed25519 + hash chain against the pinned key);
# "sealed" is kernel-signed but not yet covered by a walk; "failed" is the
# last walk rejecting an entry. A kernel self-report never reaches this word.
VERIFICATION_SEALED = "sealed"
VERIFICATION_VERIFIED = "verified"
VERIFICATION_FAILED = "failed"
# `trust_reason` beside it: why the row carries the verification it does.
TRUST_REASON_HA_VERIFIED_CHAIN = "ha_verified_chain"
TRUST_REASON_KERNEL_SEALED = "kernel_sealed"
TRUST_REASON_HA_VERIFY_FAILED = "ha_verify_failed"
TRUST_REASON_KERNEL_KEY_MISMATCH = "kernel_key_mismatch"

# The verification event entity's types (contract C10 as amended).
VERIFICATION_EVENT_CHAIN_VERIFIED = "chain_verified"
VERIFICATION_EVENT_CHAIN_FAILED = "chain_verification_failed"
VERIFICATION_EVENT_KEY_CHANGED = "witness_key_changed"
VERIFICATION_EVENT_TYPES: tuple[str, ...] = (
    VERIFICATION_EVENT_CHAIN_VERIFIED,
    VERIFICATION_EVENT_CHAIN_FAILED,
    VERIFICATION_EVENT_KEY_CHANGED,
)
# What started a walk, carried on the verification event as `trigger`.
VERIFY_TRIGGER_BUTTON = "button"
VERIFY_TRIGGER_SERVICE = "service"
VERIFY_TRIGGER_SCHEDULED = "scheduled"
VERIFY_TRIGGER_IDLE = "idle"
VERIFY_TRIGGER_BRIDGE = "bridge"
# The walk's cadence: a 15-minute backstop, and 60 s after new entries go
# idle (a few thousand Ed25519 checks is sub-second, so "Verified" is what a
# person normally sees).
VERIFY_BACKSTOP_INTERVAL_SECONDS = 15 * 60
VERIFY_IDLE_DELAY_SECONDS = 60
# What the chain_valid sensor's `error` (and the verify action's verdict)
# says while the kernel serves a key other than the pin: a key change is
# a Repairs matter, never a chain failure, and the card/strategy header
# read it as such.
VERIFY_ERROR_KEY_CHANGED = "the witness key changed; confirm it in Settings → Repairs"
# How long the covered-entry index remembers an entry when the kernel's
# digest does not say its retention; and a hard cap on the index.
VERIFY_COVERED_DEFAULT_RETENTION_SECONDS = 30 * 24 * 3600
VERIFY_COVERED_MAX_ENTRIES = 100_000
# The Frigate link is "on" while the bridge says connected AND Frigate was
# heard within this long (a quiet night is not a dead link, an hour is).
FRIGATE_LINK_STALE_AFTER_SECONDS = 3600

# =============================================================================
# Evidence export (contract C7 as replaced by the delta)
# =============================================================================
# The export directory under the Home Assistant configuration directory —
# never under www/ (served without authentication) — 0700, files 0600.
EVIDENCE_DIR_PARTS = ("securacv", "evidence")
EVIDENCE_INDEX_FILENAME = "index.json"
# The authenticated view that streams a zip, and the signed-link lifetime.
EVIDENCE_VIEW_URL = "/api/securacv/evidence/{export_id}"
EVIDENCE_VIEW_NAME = "api:securacv:evidence"
EVIDENCE_LINK_HOURS = 24
# The export window, in whole hours.
EXPORT_WINDOW_MIN_HOURS = 1
EXPORT_WINDOW_MAX_HOURS = 720
EXPORT_WINDOW_DEFAULT_HOURS = 24
# What W5 ships under www/ and what each zip carries: the offline viewer
# (byte-equal to viewer/evidence_viewer.html), the recipient's recipe, and
# the pinned witness key so "self-attested" can become "authorship confirmed".
EVIDENCE_VIEWER_SOURCE = "securacv-evidence-viewer.html"
EVIDENCE_README_SOURCE = "evidence-README.txt"
EVIDENCE_ZIP_VIEWER = "evidence_viewer.html"
EVIDENCE_ZIP_README = "README.txt"
EVIDENCE_ZIP_KEY = "WITNESS_KEY.txt"
# The kernel's bundle download must stay bounded: one coarse log is small.
EXPORT_BUNDLE_MAX_BYTES = 64 * 1024 * 1024

# The kernel's 10-minute time bucket: every "at" the integration records is a
# bucket start, never an event-precise epoch (coarse time, invariant III).
TIME_BUCKET_SECONDS = 600

# The bridge's retained state topics, relative to the state prefix. The claim
# and the announce carry the kernel device id as a segment.
KERNEL_TOPIC_LAST_EVENT = "last_event"
KERNEL_TOPIC_EVENTS = "events"
KERNEL_TOPIC_DIGEST = "digest"
KERNEL_TOPIC_CHAIN_PROBLEM = "chain_problem"
KERNEL_TOPIC_CHAIN_PROBLEM_ATTRS = "chain_problem/attrs"
KERNEL_TOPIC_STATUS = "status"
KERNEL_TOPIC_CLAIM_STATE = "claim_state"
KERNEL_TOPIC_FRIGATE_LINK = "frigate_link"
KERNEL_TOPIC_ANNOUNCE = "announce"
KERNEL_TOPIC_CLAIM = "claim"
# The availability LWT payloads on <prefix>/status (unchanged bridge contract).
KERNEL_STATUS_ONLINE = "online"
KERNEL_STATUS_OFFLINE = "offline"
# claim_state payloads.
CLAIM_STATE_CLAIMED = "claimed"
CLAIM_STATE_UNCLAIMED = "unclaimed"

# Repairs issue translation keys. Entry-scoped ids are `<key>_<entry_id>`;
# `no_kernel_found` is global (it is raised by a config flow with no entry).
ISSUE_NO_KERNEL_FOUND = "no_kernel_found"
ISSUE_KERNEL_UNREACHABLE = "kernel_unreachable"
ISSUE_TOFU_MISMATCH = "tofu_mismatch"
ISSUE_STORAGE_REPLACE = "storage_replace"
ISSUE_ENTITIES_MIGRATED = "entities_migrated"
ISSUE_MQTT_SUBSCRIBE_FAILED = "mqtt_subscribe_failed"

# Where the Repairs issues and the flow's final step send people. The my-HA
# links open the user's own instance; the docs links are this repository.
ADDON_REPOSITORY_LINK = (
    "https://my.home-assistant.io/redirect/supervisor_add_addon_repository/"
    "?repository_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV"
)
DOCKER_QUICKSTART_LINK = (
    "https://github.com/kmay89/securaCV/blob/main/docs/frigate_integration.md"
)
ENTITY_MIGRATION_DOCS_LINK = (
    "https://github.com/kmay89/securaCV/blob/main/docs/homeassistant_setup.md"
)
KERNEL_UNREACHABLE_DOCS_LINK = (
    "https://github.com/kmay89/securaCV/blob/main/docs/homeassistant_setup.md"
)
STORAGE_REPLACE_DOCS_LINK = (
    "https://github.com/kmay89/securaCV/blob/main/docs/homeassistant_setup.md"
)
# Settings → Dashboards on the user's own instance, where the SecuraCV
# dashboard strategy is one "Add dashboard" click away (J1).
DASHBOARDS_LINK = "https://my.home-assistant.io/redirect/lovelace_dashboards/"

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
# The canary base's last-will topic (securacv_mqtt.cpp): a bare
# `online`/`offline`, retained; the other products say it in their status.
TOPIC_AVAILABILITY = "availability"
TOPIC_EVENTS = "events"
TOPIC_HEALTH = "health"
TOPIC_CHAIN = "chain"
TOPIC_COUNTS = "counts"
TOPIC_TAMPER = "tamper"
TOPIC_MESH = "mesh"
TOPIC_CHIRP = "chirp"
TOPIC_TRANSPORT = "transport"
# The canary-wap's committed-event egress counters (firmware sweep F149),
# retained, beside its health: the object the canary base carries in its
# health as `csi_event_egress`. Its health body has no room for them.
TOPIC_EGRESS = "egress"
# The retained per-variant snapshot. This — not a `presence` topic — is where
# the firmware actually publishes presence: canary-sense's topics.h builds
# `securacv/<id>/state`, and its own HA discovery reads `value_json.presence`.
TOPIC_STATE = "state"

# =============================================================================
# Event egress counters (firmware sweeps F109 and F149; HA24)
# =============================================================================
# What a device's committed-event egress dropped and sent since its last boot,
# in the firmware's own names: the canary-wap's csi_event_egress::Stats (the
# canary base's CsiEventEgressStats mirrors it field for field), the SD
# backfill planner's Stats (nested as `planner`), and the canary base's MQTT
# offline queue's drop counters. The health sensor shows them as attributes.
# A counter the firmware adds is not shown until it is named here; in the
# monorepo tests/test_egress_health.py holds these to the firmware structs.
EGRESS_COUNTERS = ("dropped", "held_dropped", "ambient_dropped", "unsent_dropped")
EGRESS_PLANNER_COUNTERS = (
    "live",
    "held",
    "queued",
    "replayed",
    "skipped",
    "untrusted",
    "unsendable",
    "truncated_unsent",
    "read_giveups",
)
OFFLINE_QUEUE_COUNTERS = ("dropped_overflow", "dropped_oversize", "dropped_flushed")

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

# The advertised transport set: binary_sensor.py creates one transport
# sensor for each entry (its TRANSPORT_SENSORS table is keyed by this list),
# and tests/test_feature_flags.py plus scripts/lint_feature_flags.sh (check B)
# enforce that no FUTURE_* transport is ever advertised; the registry is
# docs/feature-flags.md.
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
TAMPER_SD_REMOVE = "sd_remove"             # SD card removed
TAMPER_SD_ERROR = "sd_error"               # SD card write failure
TAMPER_GPS_JAMMING = "gps_jamming"         # GPS signal lost/jammed
TAMPER_MOTION = "motion"                   # Unexpected movement (accelerometer)
TAMPER_ENCLOSURE = "enclosure"             # Physical enclosure opened
TAMPER_GPIO = "gpio"                       # GPIO tamper pin triggered
TAMPER_WATCHDOG = "watchdog"               # System hang detected
TAMPER_REBOOT = "unexpected_reboot"        # Unexpected reboot
TAMPER_MEMORY = "memory_critical"          # Critical memory exhaustion
TAMPER_AUDIO = "audio_anomaly"             # Future: unusual audio (jamming?)

# The advertised tamper set: binary_sensor.py creates one per-type tamper
# sensor for each entry (its TAMPER_TYPE_SENSORS table is keyed by this
# list), and tests/test_feature_flags.py plus scripts/lint_feature_flags.sh
# (check B) enforce that no FUTURE_* tamper type is ever advertised; the
# registry is docs/feature-flags.md.
ALL_TAMPER_TYPES = [
    TAMPER_POWER_LOSS,
    TAMPER_SD_REMOVE,
    TAMPER_SD_ERROR,
    TAMPER_GPS_JAMMING,
    TAMPER_MOTION,
    TAMPER_ENCLOSURE,
    TAMPER_GPIO,
    TAMPER_WATCHDOG,
    TAMPER_REBOOT,
    TAMPER_MEMORY,
]

# Declared for forward-compatibility but NOT implemented: no firmware emits
# these, and no tamper sensor is created for them — the integration never
# advertises a tamper type no device can raise. Wire one end-to-end (a row in
# binary_sensor.py's TAMPER_TYPE_SENSORS and a health/tamper field the
# firmware actually publishes) before moving it up to ALL_TAMPER_TYPES.
FUTURE_TAMPER_TYPES = [
    TAMPER_AUDIO,
]

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
# Witness Event Type Metadata
# Maps coarse, privacy-preserving event_type claims (emitted by the kernel and
# its sensor adapters - see spec/sensor_adapter_contract_v0.md) to a friendly
# label and Material Design icon for the Home Assistant "single pane of glass".
# These are claims, never identities: no plates, faces, or precise location.
# =============================================================================
EVENT_TYPE_METADATA = {
    # D13: a Frigate person and a Frigate car both seal as the large class,
    # a dog or a cat as the small one. The label says what the coarse claim
    # means to a person; the dictionary id (and the kernel's variant) is
    # unchanged. The honest coarsening is explained once, in the strategy
    # header, not per row.
    "boundary_crossing_object_large": {
        "label": "Person or vehicle",
        "icon": "mdi:car",
    },
    "boundary_crossing_object_small": {
        "label": "Animal or small object",
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
# canary-sentinel FUSES several physically independent media (PIR, radar,
# WiFi CSI/RF, BLE, light), so no single-medium glyph is honest for it: it is
# "other" until the dictionary grows a fusion modality (a vocabulary decision
# that starts in spec/witness_dictionary.json, not here). Its events carry
# the corroborating classes themselves, as the signed `modality_bits`.
DEVICE_TYPE_MODALITY = {
    "canary-sense": MODALITY_RADAR,
    "canary-vision": MODALITY_CAMERA,
    "canary-wap": MODALITY_WIFI_CSI,
    "canary-contact": MODALITY_CONTACT,
    "canary-sentinel": MODALITY_OTHER,
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

# Canonical device_type literal for the multi-sensor fusion guardian
# (firmware/configs/canary-sentinel/*/config.h SENT_DEVICE_TYPE).
DEVICE_TYPE_CANARY_SENTINEL = "canary-sentinel"

# Canonical device_type literal for the design-stage pool water-chemistry node
# (docs/research/pool_water_monitor.md). Reserved here so the name has ONE
# spelling: the Dash's card layer already recognizes it (fleet_cards.cpp),
# no firmware config publishes it yet, and no code here reads it. Deliberately
# NOT added to DEVICE_TYPE_MODALITY: a
# pool node reports water chemistry, not a presence claim, so it has no
# sensing modality on the timeline — modality_for() correctly resolves it to
# MODALITY_UNKNOWN.
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


# The witness event-type ids, DERIVED from the metadata table above (which
# scripts/lint_dictionary_sync.py holds to spec/witness_dictionary.json). The
# event platform's `event_types` and the verifier's covered-entry index both
# read this tuple; neither retypes an id (the linter asserts event.py holds
# no literal list).
WITNESS_EVENT_TYPES: tuple[str, ...] = tuple(EVENT_TYPE_METADATA)


def normalize_event_type(event_type: object) -> str | None:
    """The snake_case dictionary id of an event type, or None.

    The kernel and its bridge spell event types as the Rust enum variant
    (``BoundaryCrossingObjectLarge``); Home Assistant's event entities and
    the dictionary spell them snake_case (``boundary_crossing_object_large``).
    One fold, used by every consumer, so an event pushed by the bridge, an
    entry walked out of the sealed log and the entity's ``event_types`` list
    agree on the spelling they are compared by. Unknown types fold the same
    way (the caller decides whether an unknown id is an error).
    """
    if not isinstance(event_type, str):
        return None
    key = event_type.strip()
    if not key:
        return None
    if key in EVENT_TYPE_METADATA:
        return key
    snake = ""
    for i, ch in enumerate(key):
        if ch.isupper() and i > 0:
            snake += "_"
        snake += ch.lower()
    return snake


def event_type_metadata(event_type: str | None) -> dict[str, str]:
    """Return {'label', 'icon'} for an event_type, with a sensible default.

    Accepts both snake_case (e.g. "acoustic_impulse_in_zone") and the kernel's
    CamelCase enum form (e.g. "AcousticImpulseInZone").
    """
    key = normalize_event_type(event_type)
    if key is None:
        return {"label": "Unknown", "icon": DEFAULT_EVENT_ICON}
    meta = EVENT_TYPE_METADATA.get(key)
    if meta is None:
        return {"label": str(event_type), "icon": DEFAULT_EVENT_ICON}
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
