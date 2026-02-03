"""Constants for the SecuraCV integration."""

DOMAIN = "securacv"

# Config keys
CONF_MQTT_PREFIX = "mqtt_prefix"

# MQTT Topics (relative to prefix/{device_id}/)
TOPIC_STATUS = "status"
TOPIC_EVENTS = "events"
TOPIC_HEALTH = "health"
TOPIC_CHAIN = "chain"
TOPIC_COUNTS = "counts"
TOPIC_COMMAND = "command"

# Attributes
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

# Device info
MANUFACTURER = "ERRERlabs"
MODEL_CANARY = "SecuraCV Canary"
