"""Tests for the pure storage-health metric helpers.

health_metrics.py deliberately imports no homeassistant.* modules, so these
tests run directly under the stub conftest harness.
"""
from __future__ import annotations

from ..health_metrics import (
    STORAGE_STATUS_CRITICAL,
    STORAGE_STATUS_DEGRADED,
    STORAGE_STATUS_GOOD,
    STORAGE_STATUS_REPLACEMENT_RECOMMENDED,
    battery_charging,
    battery_percent,
    bytes_per_day_to_mb,
    canary_sd,
    canary_sd_replace_recommended,
    canary_sd_wear_pct,
    kernel_storage,
    kernel_thermal,
    memory_free_bytes,
    replacement_recommended,
    round_pct,
    storage_status,
)

KERNEL_STATUS = {
    "kernel_version": "0.5.0",
    "storage": {
        "status": "good",
        "free_bytes": 10_000_000_000,
        "total_bytes": 32_000_000_000,
        "free_pct": 31.25,
        "db_bytes": 1_048_576,
        "wal_bytes": 65_536,
        "write_errors": 0,
        "write_rate_bytes_per_day": 52_428_800,
        "lifetime_bytes_written": 128_000_000_000,
        "endurance_tbw": 64.0,
        "wear_pct": 0.2,
        "estimated_days_remaining": 24_000,
        "source_device": "mmcblk0",
    },
    "thermal": {"soc_temp_c": 52.1, "status": "ok"},
    "time_bucket": {"start_epoch_s": 1765400400, "size_s": 600},
}

CANARY_HEALTH = {
    "battery": 88,
    "free_heap": 123_456,
    "uptime": 3600,
    "temp_c": 41.5,
    "sd": {
        "mounted": True,
        "usage_pct": 12,
        "writes": 42_000,
        "errors": 0,
        "lifetime_kb": 9_000_000,
        "wear_pct": 2.8,
        "replace_recommended": False,
    },
}


def test_kernel_storage_extracts_section():
    assert kernel_storage(KERNEL_STATUS)["source_device"] == "mmcblk0"
    assert kernel_storage(None) is None
    assert kernel_storage({}) is None
    assert kernel_storage({"storage": "broken"}) is None


def test_kernel_thermal_extracts_section():
    assert kernel_thermal(KERNEL_STATUS)["soc_temp_c"] == 52.1
    assert kernel_thermal(None) is None
    assert kernel_thermal({"thermal": 7}) is None


def test_storage_status_tolerates_missing_data():
    assert storage_status(KERNEL_STATUS) == STORAGE_STATUS_GOOD
    assert storage_status(None) is None
    assert storage_status({"storage": {}}) is None


def test_replacement_recommended_statuses():
    assert not replacement_recommended(STORAGE_STATUS_GOOD)
    assert not replacement_recommended(STORAGE_STATUS_DEGRADED)
    assert replacement_recommended(STORAGE_STATUS_REPLACEMENT_RECOMMENDED)
    assert replacement_recommended(STORAGE_STATUS_CRITICAL)
    assert not replacement_recommended(None)
    assert not replacement_recommended("unexpected_value")


def test_round_pct_tolerates_junk():
    assert round_pct(31.2499) == 31.2
    assert round_pct("12.34") == 12.3
    assert round_pct(None) is None
    assert round_pct("junk") is None


def test_bytes_per_day_to_mb():
    assert bytes_per_day_to_mb(52_428_800) == 50.0
    assert bytes_per_day_to_mb(None) is None
    assert bytes_per_day_to_mb("junk") is None


def test_canary_sd_extraction_and_flags():
    assert canary_sd(CANARY_HEALTH)["mounted"] is True
    assert canary_sd_wear_pct(CANARY_HEALTH) == 2.8
    assert not canary_sd_replace_recommended(CANARY_HEALTH)

    worn = {"sd": {"wear_pct": 83.0, "replace_recommended": True}}
    assert canary_sd_replace_recommended(worn)

    # Firmware without SD reporting degrades cleanly.
    legacy = {"battery": 90, "memory_free": 50_000}
    assert canary_sd(legacy) is None
    assert canary_sd_wear_pct(legacy) is None
    assert not canary_sd_replace_recommended(legacy)


def test_memory_free_reads_both_firmware_spellings():
    assert memory_free_bytes({"memory_free": 1000}) == 1000
    assert memory_free_bytes({"free_heap": 2000}) == 2000
    # canary-wap spelling wins when both are present.
    assert memory_free_bytes({"memory_free": 1000, "free_heap": 2000}) == 1000
    assert memory_free_bytes({"memory_free": "junk", "free_heap": 2000}) == 2000
    assert memory_free_bytes({}) == 0


def test_battery_percent_reads_both_firmware_spellings():
    # canary-wap spelling: trusted with or without presence info.
    assert battery_percent({"battery": 88}) == 88
    assert battery_percent({"battery": 42, "battery_present": True}) == 42
    # Explicit mains power reads as "no battery to alert on".
    assert battery_percent({"battery": 100, "battery_present": False}) is None
    # firmware/canary spelling: only trusted with explicit presence —
    # older builds report battery_soc=0 on USB-only devices, which must
    # not surface as a critical battery.
    assert battery_percent({"battery_soc": 61, "battery_present": True}) == 61
    assert battery_percent({"battery_soc": 0}) is None
    assert battery_percent({"battery_soc": 0, "battery_present": False}) is None
    # Missing or junk values degrade to None, never to a false reading.
    assert battery_percent({}) is None
    assert battery_percent({"battery": "junk"}) is None


def test_battery_charging_states():
    assert battery_charging({"charge_state": "charging"})
    assert battery_charging({"charge_state": "full"})
    assert not battery_charging({"charge_state": "discharging"})
    assert not battery_charging({"charge_state": "critical"})
    assert not battery_charging({})
