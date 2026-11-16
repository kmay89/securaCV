"""A Canary that reads no battery shows an unknown battery, never a full one.

canary-sense, canary-vision and canary-sentinel published ``"battery": 100``
beside ``"battery_present": false`` in their retained health: a charge nothing
measured. The health sensor's ``battery_percent`` attribute then turned every
"no battery" into 100, the WAP's on mains power included. Since device-details
§1.4 (PR 6) the trio's health omits ``battery`` and the attribute reads
``None`` (unknown) whenever the health reports no battery, the way
``health_metrics.battery_percent`` already read it for the thresholds.

The monorepo-only tests hold the three firmware bodies to the same rule (each
says ``battery_present`` and names no ``battery`` value), and the battery
blueprint's "recovered" push to a Canary that has a battery: its Health
sensor also leaves "warning" when low memory clears, and on a mains Canary
that push read "Battery back to 0%" once the attribute stopped saying 100.
The HACS mirror carries neither firmware source nor docs/, so they skip
there.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest
import yaml

from .test_mqtt_payload_hardening import _entity, _msg

from .. import sensor as sensor_platform
from ..health_metrics import battery_percent

_REPO = Path(__file__).resolve().parents[3]
_FW = _REPO / "firmware" / "projects"
_BATTERY_BLUEPRINT = _REPO / "docs" / "blueprints" / "canary_battery_health.yaml"
_TRIO = ("canary-sense", "canary-vision", "canary-sentinel")

monorepo_only = pytest.mark.skipif(
    not (_FW / "canary-sense" / "src" / "net" / "mqtt_mgr.cpp").is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)

# The trio's health, as it is published now: no battery value at all.
TRIO_HEALTH = {
    "battery_present": False, "memory_free": 180_000, "uptime": 3600,
    "firmware_version": "2.4.16", "public_key": "ab" * 32,
}
# What they published before, and what a WAP on mains power still sends.
OLD_HEALTH = dict(TRIO_HEALTH, battery=100)


def _health(payload: dict):
    sensor = _entity(sensor_platform.SecuraCVCanaryHealthSensor)
    sensor._handle_message(_msg(json.dumps(payload)))
    return sensor


def test_an_absent_battery_reads_unknown() -> None:
    assert battery_percent(TRIO_HEALTH) is None
    sensor = _health(TRIO_HEALTH)
    attrs = sensor._attr_extra_state_attributes
    assert attrs["battery_percent"] is None, "no battery is unknown, never 100"
    assert attrs["battery_present"] is False
    # No battery is no power-loss risk: the sensor stays healthy, as it was.
    assert sensor._attr_native_value == "healthy"


def test_a_battery_reported_absent_reads_unknown_whatever_its_value() -> None:
    # The old trio body and a WAP on mains: 100 beside battery_present false.
    sensor = _health(OLD_HEALTH)
    assert sensor._attr_extra_state_attributes["battery_percent"] is None
    assert sensor._attr_native_value == "healthy"


def test_a_real_battery_still_reads_its_charge() -> None:
    sensor = _health(dict(TRIO_HEALTH, battery_present=True, battery=42))
    assert sensor._attr_extra_state_attributes["battery_percent"] == 42
    assert sensor._attr_native_value == "healthy"
    sensor = _health(dict(TRIO_HEALTH, battery_present=True, battery=8))
    assert sensor._attr_extra_state_attributes["battery_percent"] == 8
    assert sensor._attr_native_value == "critical"


@monorepo_only
@pytest.mark.parametrize("product", _TRIO)
def test_the_trio_health_names_no_battery_value(product: str) -> None:
    src = (_FW / product / "src" / "net" / "mqtt_mgr.cpp").read_text(encoding="utf-8")
    start = src.index("void publish_health_retained(")
    body = src[start:src.index("\n}\n", start)]
    assert '\\"battery_present\\":false,' in body, f"{product}: says it has no battery"
    assert not re.search(r'\\"battery\\":', body), (
        f"{product}: publishes a battery value beside battery_present false")


class _BlueprintLoader(yaml.SafeLoader):
    """Reads a blueprint's ``!input name`` tags as the name."""


_BlueprintLoader.add_constructor("!input", lambda loader, node: loader.construct_scalar(node))


@pytest.mark.skipif(not _BATTERY_BLUEPRINT.is_file(),
                    reason="monorepo-only: the HACS mirror carries no docs/")
def test_the_battery_blueprint_says_no_charge_of_a_canary_without_one() -> None:
    blueprint = yaml.load(_BATTERY_BLUEPRINT.read_text(encoding="utf-8"), Loader=_BlueprintLoader)
    options = blueprint["action"][0]["choose"]
    by_trigger = {}
    for option in options:
        ids = [c.get("id") for c in option["conditions"] if c.get("condition") == "trigger"]
        templates = " ".join(c.get("value_template", "") for c in option["conditions"]
                             if c.get("condition") == "template")
        for trigger_id in ids:
            by_trigger[trigger_id] = templates
    present = "state_attr(health_sensor, 'battery_present') == true"
    assert present in by_trigger["low"], "the low-battery push needs a battery"
    assert present in by_trigger["recovered"], (
        "the recovered push names a charge, so it fires only for a Canary with a battery")
    # battery_percent is unknown when a present battery reports no number,
    # and the blueprint's battery_pct reads unknown as 0: both pushes name
    # battery_pct, so both need a charge that was read.
    read = "state_attr(health_sensor, 'battery_percent') is number"
    assert read in by_trigger["low"], "a low-battery push needs a charge that was read"
    assert read in by_trigger["recovered"], "a recovered push needs a charge that was read"
    assert "battery_percent') | int(0)" in blueprint["variables"]["battery_pct"]
