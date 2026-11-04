"""Pure helpers for SD-card / storage health metrics.

The kernel's token-gated `GET /status` endpoint and the Canary firmware's
MQTT `health` payload both carry storage endurance metrics (free space,
estimated wear against the card's TBW rating, write errors, thermal state).
These helpers normalize those payloads for the sensor/binary_sensor
platforms.

Deliberately free of any `homeassistant.*` imports so they run under the
stub-based test harness in tests/conftest.py.
"""
from __future__ import annotations

from typing import Any

from .const import EGRESS_COUNTERS, EGRESS_PLANNER_COUNTERS, OFFLINE_QUEUE_COUNTERS

# Kernel storage health statuses (ordered by severity; mirrors the kernel's
# StorageHealthStatus enum in src/storage_health.rs).
STORAGE_STATUS_GOOD = "good"
STORAGE_STATUS_DEGRADED = "degraded"
STORAGE_STATUS_REPLACEMENT_RECOMMENDED = "replacement_recommended"
STORAGE_STATUS_CRITICAL = "critical"

# Statuses that should surface as a "replace the SD card" problem signal.
REPLACEMENT_STATUSES = frozenset(
    {STORAGE_STATUS_REPLACEMENT_RECOMMENDED, STORAGE_STATUS_CRITICAL}
)

BYTES_PER_MB = 1024 * 1024


def kernel_storage(status_payload: dict[str, Any] | None) -> dict[str, Any] | None:
    """Extract the `storage` section from a kernel /status payload."""
    if not isinstance(status_payload, dict):
        return None
    storage = status_payload.get("storage")
    return storage if isinstance(storage, dict) else None


def kernel_thermal(status_payload: dict[str, Any] | None) -> dict[str, Any] | None:
    """Extract the `thermal` section from a kernel /status payload."""
    if not isinstance(status_payload, dict):
        return None
    thermal = status_payload.get("thermal")
    return thermal if isinstance(thermal, dict) else None


def storage_status(status_payload: dict[str, Any] | None) -> str | None:
    """Return the storage health status string, if present."""
    storage = kernel_storage(status_payload)
    if storage is None:
        return None
    status = storage.get("status")
    return str(status) if status is not None else None


def replacement_recommended(status: str | None) -> bool:
    """True when the status calls for replacing the SD card."""
    return status in REPLACEMENT_STATUSES


def round_pct(value: Any) -> float | None:
    """Round a percentage metric to one decimal, tolerating junk."""
    try:
        return round(float(value), 1)
    except (TypeError, ValueError):
        return None


def bytes_per_day_to_mb(value: Any) -> float | None:
    """Convert a bytes/day write rate to MB/day for display."""
    try:
        return round(float(value) / BYTES_PER_MB, 2)
    except (TypeError, ValueError):
        return None


def canary_sd(health_payload: dict[str, Any] | None) -> dict[str, Any] | None:
    """Extract the `sd` object from a Canary MQTT health payload."""
    if not isinstance(health_payload, dict):
        return None
    sd = health_payload.get("sd")
    return sd if isinstance(sd, dict) else None


def canary_sd_wear_pct(health_payload: dict[str, Any] | None) -> float | None:
    """Wear percentage from a Canary health payload, if reported."""
    sd = canary_sd(health_payload)
    if sd is None:
        return None
    return round_pct(sd.get("wear_pct"))


def canary_sd_replace_recommended(health_payload: dict[str, Any] | None) -> bool:
    """True when the Canary firmware flags its SD card for replacement."""
    sd = canary_sd(health_payload)
    if sd is None:
        return False
    return bool(sd.get("replace_recommended", False))


# The largest integer Home Assistant carries in a state attribute: its JSON
# writer (orjson) refuses one past 64 bits, TypeError "Integer exceeds 64-bit
# range" from State.as_dict_json, so a reading above this written into the
# Health sensor fails the recorder and every websocket subscription on the
# entity, downstream of the callback that read it. Held on the 2026.5 floor
# and the current release in tests/ha/test_entities.py.
HA_ATTRIBUTE_INT_MAX = 0xFFFF_FFFF_FFFF_FFFF


def _unsigned_reading(value: Any) -> int | None:
    """One health key's value as a count that cannot go below zero, or None.

    The number int() reads from it, truncated toward zero, when the value is
    an int, a float or a string of decimal digits; None when it is no such
    number: absent or null, a JSON `true` or `false` (int() would read 1
    and 0), a string or container int() refuses, a string int() reads only
    through a sign or its digit separator (`"+5"`, `"5_0"`, which it reads
    as 5 and 50: a unit publishes a number, never a spelling of one), NaN
    (ValueError), an infinity (`Infinity`, `-Infinity`, `1e400`:
    OverflowError), a negative number (the sign read before truncation as
    well, since -0.5 truncates to 0), or a number above HA_ATTRIBUTE_INT_MAX
    (`1e300`, 2**64), which Home Assistant cannot carry; both firmware count
    in a uint32_t at most, so no reading a unit sends is lost to the bound.
    The one reader behind memory_free_reading and battery_percent, so the
    Health sensor reads the two keys alike (sweeps HA53, HA57).
    """
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, str) and not value.strip().isdigit():
        return None
    try:
        reading = int(value)
    except (TypeError, ValueError, OverflowError):
        return None
    if reading < 0 or (isinstance(value, float) and value < 0):
        return None
    if reading > HA_ATTRIBUTE_INT_MAX:
        return None
    return reading


def memory_free_reading(health_payload: dict[str, Any]) -> int | None:
    """Free-memory reading across firmware payload spellings, or None.

    canary-wap publishes `memory_free`; firmware/canary publishes
    `free_heap`. The first of the two that holds a number is the reading;
    None when the health carries neither as a number, so a reader can tell
    "no reading" from "0 bytes free". A non-finite number is no number:
    json.loads takes `Infinity`, `NaN` and `1e400`, and int() raises
    OverflowError on the infinities, which out of an MQTT callback would be
    a traceback on every such publish an untrusted broker sends. Nor is a
    JSON `true` or `false`, which int() would read as 1 or 0 bytes free and
    both Memory Critical and the Health sensor as critical, nor a negative
    number: both firmware count free heap unsigned (uint32_t, published
    with %lu), so a count below zero is not one either firmware can send,
    and it is skipped like junk rather than read as low memory (sweep HA53).
    """
    for key in ("memory_free", "free_heap"):
        if (reading := _unsigned_reading(health_payload.get(key))) is not None:
            return reading
    return None


def memory_free_bytes(health_payload: dict[str, Any]) -> int:
    """Free-memory reading across firmware payload spellings (0 without one).

    Read both spellings (memory_free_reading) so either firmware family
    reports correctly.
    """
    reading = memory_free_reading(health_payload)
    return 0 if reading is None else reading


def battery_percent(health_payload: dict[str, Any]) -> int | None:
    """Battery state of charge across firmware spellings, or None.

    Returns None for mains-powered devices (no battery to alert on).

    canary-wap publishes `battery` (100 with `battery_present: false`
    on mains, real SoC otherwise). firmware/canary publishes
    `battery_soc`, but older builds report battery_soc=0 on USB-only
    devices, so that spelling is only trusted when `battery_present`
    is explicitly true.

    A key's value is read as memory_free_reading reads one (sweep HA57):
    a JSON `true` or `false` is no reading (int() read `true` as 1 %, which
    the Health sensor showed as critical and as its battery_percent), nor
    is a non-finite number (an infinity raised OverflowError out of the
    Health callback on every such publish), nor a negative one: both
    firmware hold their state of charge in a uint8_t (the canary-wap's
    soc_pct, published with %u; firmware/canary's battery_soc), so a
    charge below zero is not one a unit sends. A key that holds no number
    is read past like an absent one, so `battery_soc` is still read when
    `battery_present` is true; else None, the no-battery reading.
    """
    present = health_payload.get("battery_present")
    if present is False:
        return None
    keys = ("battery", "battery_soc") if present is True else ("battery",)
    for key in keys:
        if (reading := _unsigned_reading(health_payload.get(key))) is not None:
            return reading
    return None


def battery_charging(health_payload: dict[str, Any]) -> bool:
    """True when the device reports its battery as charging or full.

    A charging battery is not a power-loss risk, so the health sensor
    skips the low-battery thresholds for it.
    """
    return str(health_payload.get("charge_state") or "").lower() in (
        "charging",
        "full",
    )


def _counters(obj: Any, names: tuple[str, ...]) -> dict[str, int] | None:
    """The named counters of a JSON object that are non-negative integers
    (a bool is not a count). None when `obj` is not an object."""
    if not isinstance(obj, dict):
        return None
    out: dict[str, int] = {}
    for name in names:
        value = obj.get(name)
        if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
            out[name] = value
    return out


def egress_counters(obj: Any) -> dict[str, Any] | None:
    """A committed-event egress's counters since the device's boot.

    `obj` is the canary base's health `csi_event_egress` object or the
    canary-wap's `egress` topic body (the same shape, firmware sweeps F109
    and F149): `dropped`, `held_dropped`, `ambient_dropped`,
    `unsent_dropped`, and the SD backfill planner's counters under
    `planner`. Only the counters const.py names are kept. None when `obj`
    is not an object (older firmware sends none).
    """
    out: dict[str, Any] | None = _counters(obj, EGRESS_COUNTERS)
    if out is None:
        return None
    planner = _counters(obj.get("planner"), EGRESS_PLANNER_COUNTERS)
    if planner is not None:
        out["planner"] = planner
    return out


def _uint(value: Any) -> int | None:
    """A JSON non-negative integer (a bool is not one), else None."""
    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
        return value
    return None


def health_identity(health_payload: Any) -> tuple[str, int] | None:
    """The (firmware_version, uptime) a health payload names, or None when
    it lacks either as a string and a non-negative integer."""
    if not isinstance(health_payload, dict):
        return None
    version = health_payload.get("firmware_version")
    uptime = _uint(health_payload.get("uptime"))
    if not isinstance(version, str) or uptime is None:
        return None
    return version, uptime


def egress_topic(payload: Any) -> tuple[tuple[str, int], dict[str, Any]] | None:
    """The canary-wap's ``egress`` topic body (firmware sweep F149).

    ``{"firmware_version": v, "uptime": s, "csi_event_egress": {...}}``: the
    firmware version and uptime of the health publish it follows, then the
    counters under the canary base's health key. Returns ((version, uptime),
    counters), or None when the body is not that shape.
    """
    ident = health_identity(payload)
    if ident is None:
        return None
    counters = egress_counters(payload.get("csi_event_egress"))
    if counters is None:
        return None
    return ident, counters


def egress_topic_pairs(topic_ident: tuple[str, int], health_ident: tuple[str, int] | None) -> bool:
    """Whether an ``egress`` body's counters belong to the device's current
    health: the same firmware version, sent no later in the boot than that
    health. A retained body an earlier boot left behind names a later uptime
    (or another version), and one a firmware with no egress topic left
    behind names another version."""
    if health_ident is None:
        return False
    return topic_ident[0] == health_ident[0] and topic_ident[1] <= health_ident[1]


def offline_queue_counters(obj: Any) -> dict[str, int] | None:
    """The canary base's MQTT offline queue drops since its boot (its health
    `offline_queue` object): `dropped_overflow`, `dropped_oversize`,
    `dropped_flushed`. None when `obj` is not an object."""
    return _counters(obj, OFFLINE_QUEUE_COUNTERS)


def event_id_space_low(health_payload: Any) -> bool | None:
    """The `event_id_space_low` flag of a health payload (firmware sweep F82).

    True once the device's event-id allocator reaches its hold limit, and
    after its ids wrap; False while there is room. None when the payload
    does not carry the flag as a JSON boolean (firmware that predates it).
    """
    if not isinstance(health_payload, dict):
        return None
    value = health_payload.get("event_id_space_low")
    return value if isinstance(value, bool) else None
