"""Tests for the pure storage-health metric helpers.

health_metrics.py deliberately imports no homeassistant.* modules, so these
tests run directly under the stub conftest harness.
"""
from __future__ import annotations

import json

import pytest

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
    memory_free_reading,
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


def test_memory_free_reading_tells_no_reading_from_zero():
    """The same reader as memory_free_bytes, but None without a reading, so
    Memory Critical can leave a health with no memory field off (HA48)."""
    assert memory_free_reading({"memory_free": 1000, "free_heap": 2000}) == 1000
    assert memory_free_reading({"memory_free": "junk", "free_heap": 2000}) == 2000
    assert memory_free_reading({"memory_free": 0}) == 0
    for body in ({}, {"memory_free": None}, {"free_heap": "junk"}, {"memory_free": [1]}):
        assert memory_free_reading(body) is None, body
        assert memory_free_bytes(body) == 0, body


def test_memory_free_reading_takes_a_non_finite_number_as_no_reading():
    """json.loads takes Infinity, NaN and 1e400 (an infinity), and int()
    raises OverflowError on the infinities: a non-finite value is no
    reading, so the other spelling is read, else None, and nothing raises
    out of the Health or Memory Critical callback."""
    for raw in ('{"memory_free": Infinity}', '{"free_heap": -Infinity}',
                '{"memory_free": 1e400}', '{"free_heap": NaN}'):
        body = json.loads(raw)
        assert memory_free_reading(body) is None, raw
        assert memory_free_bytes(body) == 0, raw
    assert memory_free_reading(json.loads('{"memory_free": Infinity, "free_heap": 4096}')) == 4096


def test_memory_free_reading_takes_a_bool_as_no_reading():
    """A JSON true or false is not a byte count: int() read them as 1 and 0
    bytes free, which both the Health sensor and Memory Critical showed as
    critical (sweep HA53). A bool is skipped like junk, so the other
    spelling is read, else None."""
    for raw in ('{"memory_free": true}', '{"memory_free": false}', '{"free_heap": true}'):
        body = json.loads(raw)
        assert memory_free_reading(body) is None, raw
        assert memory_free_bytes(body) == 0, raw
    assert memory_free_reading(json.loads('{"memory_free": true, "free_heap": 4096}')) == 4096
    # A number is still a number, in either spelling and as a numeric string.
    assert memory_free_reading({"memory_free": 1}) == 1
    assert memory_free_reading({"free_heap": "1023"}) == 1023


def test_memory_free_reading_takes_a_negative_count_as_no_reading():
    """Free heap is unsigned in both firmware (uint32_t, the WAP publishing
    it with %lu), so a negative count is not one a unit sends: it is skipped
    like junk, not read as low memory on both entities. The sign is read
    before int() truncates, so -0.5 is not 0 bytes free."""
    for raw in ('{"memory_free": -1}', '{"free_heap": -1}', '{"memory_free": -0.5}',
                '{"memory_free": "-1"}', '{"free_heap": -1e9}'):
        body = json.loads(raw)
        assert memory_free_reading(body) is None, raw
        assert memory_free_bytes(body) == 0, raw
    assert memory_free_reading(json.loads('{"memory_free": -1, "free_heap": 512}')) == 512
    assert memory_free_reading({"memory_free": 0}) == 0
    assert memory_free_reading({"memory_free": 0.5}) == 0


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


# Every JSON scalar (and two containers) a broker can put in a battery key,
# as the text json.loads takes (it takes NaN, Infinity and 1e400, an
# infinity), and the reading it is (sweep HA57): the number int() reads,
# truncated toward zero, or None for no reading. The same table as HA53's
# memory readings, and memory_free_reading gives the same answer for each.
_BATTERY_READINGS = [
    # No reading: not a number a unit's uint8_t state of charge can be.
    ("true", None),
    ("false", None),
    ("null", None),
    ("NaN", None),
    ("Infinity", None),
    ("-Infinity", None),
    ("1e400", None),
    ("-1e400", None),
    ("-1", None),
    ("-0.5", None),
    ('"-1"', None),
    ('""', None),
    ('"junk"', None),
    ('"50.5"', None),
    ("[50]", None),
    ('{"pct": 50}', None),
    # A reading. A numeric string reads as its number, a fraction truncates,
    # and minus zero is zero (it is equal to 0, so it is no negative). Above
    # 100 is read as sent: no unit sends it, and no threshold sits above.
    ("0", 0),
    ("-0.0", 0),
    ("0.5", 0),
    ("8", 8),
    ("50.9", 50),
    ('"50"', 50),
    ("100", 100),
    ("101", 101),
]


@pytest.mark.parametrize(("raw", "reading"), _BATTERY_READINGS, ids=[r for r, _ in _BATTERY_READINGS])
def test_battery_percent_reads_only_a_number_in_either_spelling(raw, reading):
    """Sweep HA57: battery_percent did `int(value)` under TypeError and
    ValueError only, so `Infinity` and `1e400` raised OverflowError out of
    the Health callback, `true` read as 1 % (critical) and `false` as 0 %,
    and a negative charge read as critical. It now reads a key's value as
    memory_free_reading does, under `battery` and under `battery_soc` alike
    (the latter only when battery_present is true, as before)."""
    value = json.loads(raw)
    for body in (
        {"battery": value},
        {"battery": value, "battery_present": True},
        {"battery_soc": value, "battery_present": True},
    ):
        assert battery_percent(body) == reading, body
        assert type(battery_percent(body)) is type(reading), body
    # The gate stays: mains power is no battery, whatever the key says,
    # and battery_soc without battery_present true is not trusted.
    assert battery_percent({"battery": value, "battery_present": False}) is None
    assert battery_percent({"battery_soc": value, "battery_present": False}) is None
    assert battery_percent({"battery_soc": value}) is None
    # One reader: free memory reads the same value the same way.
    assert memory_free_reading({"memory_free": value}) == reading
    assert memory_free_reading({"free_heap": value}) == reading


_NO_BATTERY_NUMBER = [raw for raw, reading in _BATTERY_READINGS if reading is None]


@pytest.mark.parametrize("raw", _NO_BATTERY_NUMBER, ids=_NO_BATTERY_NUMBER)
def test_battery_percent_reads_past_a_key_with_no_number(raw):
    """A `battery` that holds no number is read past like an absent one, as
    memory_free_reading reads past a junk memory_free: battery_soc is then
    the reading when battery_present is true, and only then."""
    body = json.loads('{"battery": ' + raw + ', "battery_soc": 61, "battery_present": true}')
    assert battery_percent(body) == 61
    body = json.loads('{"battery": ' + raw + ', "battery_soc": 61}')
    assert battery_percent(body) is None
    # And a battery_soc with no number is no reading either.
    body = json.loads('{"battery": ' + raw + ', "battery_soc": ' + raw + ', "battery_present": true}')
    assert battery_percent(body) is None


def test_a_number_in_battery_still_wins_over_battery_soc():
    assert battery_percent({"battery": 9, "battery_soc": 61, "battery_present": True}) == 9
    assert battery_percent({"battery": 0, "battery_soc": 61, "battery_present": True}) == 0


def test_battery_charging_states():
    assert battery_charging({"charge_state": "charging"})
    assert battery_charging({"charge_state": "full"})
    assert not battery_charging({"charge_state": "discharging"})
    assert not battery_charging({"charge_state": "critical"})
    assert not battery_charging({})
