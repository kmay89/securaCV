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


def memory_free_bytes(health_payload: dict[str, Any]) -> int:
    """Free-memory reading across firmware payload spellings.

    canary-wap publishes `memory_free`; firmware/canary publishes
    `free_heap`. Read both so either firmware family reports correctly.
    """
    for key in ("memory_free", "free_heap"):
        value = health_payload.get(key)
        if value is not None:
            try:
                return int(value)
            except (TypeError, ValueError):
                continue
    return 0


def battery_percent(health_payload: dict[str, Any]) -> int | None:
    """Battery state of charge across firmware spellings.

    Returns None for mains-powered devices (no battery to alert on).

    canary-wap publishes `battery` (100 with `battery_present: false`
    on mains, real SoC otherwise). firmware/canary publishes
    `battery_soc`, but older builds report battery_soc=0 on USB-only
    devices, so that spelling is only trusted when `battery_present`
    is explicitly true.
    """
    present = health_payload.get("battery_present")
    if present is False:
        return None
    value = health_payload.get("battery")
    if value is None and present is True:
        value = health_payload.get("battery_soc")
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def battery_charging(health_payload: dict[str, Any]) -> bool:
    """True when the device reports its battery as charging or full.

    A charging battery is not a power-loss risk, so the health sensor
    skips the low-battery thresholds for it.
    """
    return str(health_payload.get("charge_state", "")).lower() in (
        "charging",
        "full",
    )
