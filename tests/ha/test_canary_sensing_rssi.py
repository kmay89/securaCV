"""The canary's Sensing RSSI before and after a CSI window, on a real core (sweep F411).

firmware/canary announces Sensing RSSI over MQTT discovery itself
(lib/securacv_mqtt/src/securacv_mqtt.cpp, mqtt_send_ha_discovery): a sensor
on the retained sensing topic, dBm, device_class signal_strength, no
state_class. src/main.cpp's mqtt_publish_sensing_update() builds that
topic's one body, and carries rssi_dbm only when the last CSI window's mean
was taken over a frame (sensing_rssi_measured(), which
firmware/tests_host/test_canary_sensing_rssi.cpp runs on real CSI windows):
before the first window, after a reboot until the next one, after a window
that heard nothing or only frames below the noise floor, and for good on a
build without CSI (FEATURE_CSI defaults to 0), the body has no rssi_dbm,
where it used to carry the snapshot's unmeasured zero.

The row's template renders a body without the key as nothing. This module
announces the row, read out of securacv_mqtt.cpp, to the real MQTT
integration beside Motion Score (another sensor on the same topic, read out
the same way) and drives it on those bodies: Home Assistant's MQTT sensor
(homeassistant/components/mqtt/sensor.py ``_update_state``) ignores an empty
rendering on a sensor whose state must be numeric, which its unit makes it.
So the entity reads unknown until a reading, keeps the last reading when a
body leaves the key out, never shows 0 dBm, and logs no warning, while
Motion Score follows every body. The old ``default(0)`` rendered 0 there.

The same keep-the-last rule has one cost, shown here too: an install
upgraded from firmware that published the unmeasured 0 keeps showing 0 dBm
until the first reading, since neither a new discovery config nor a body
without the key resets the state (on a build without CSI, until Home
Assistant restarts).

The body's key is held to main.cpp, and every other sensing template to its
absence, by scripts/tests/test_ha_discovery_canary_sensing_render.py.

Host-tested: a real Home Assistant core over a mocked paho client, not a
unit on a bench and a real broker; main.cpp is read, not built.
"""
from __future__ import annotations

import json
import logging
import re
from typing import Any

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import REPO_ROOT

MQTT_CPP = REPO_ROOT / "firmware" / "canary" / "lib" / "securacv_mqtt" / "src" / "securacv_mqtt.cpp"
DEVICE = "c0ffee02"
SENSING = f"securacv/{DEVICE}/sensing"

pytestmark = pytest.mark.skipif(
    not MQTT_CPP.is_file(), reason="monorepo-only: the HACS mirror carries no firmware source"
)


def _c_string(text: str) -> str:
    """The adjacent C string literals in `text`, concatenated and decoded."""
    return "".join(
        re.sub(r"\\(.)", lambda m: m.group(1), lit) for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text)
    )


def row(object_id: str) -> dict[str, str]:
    """mqtt_send_ha_discovery's block for the sensor `object_id`: the doc keys
    it sets to a string literal, and its state topic's variable."""
    src = MQTT_CPP.read_text(encoding="utf-8")
    call = src.find(f'publish_discovery("sensor", device_id, "{object_id}",')
    assert call != -1, f"securacv_mqtt.cpp announces no sensor {object_id}; re-read it"
    block = src[src.rfind("if (all_ok) {", 0, call):call]
    keys = {k: _c_string(v) for k, v in re.findall(r'doc\["(\w+)"\]\s*=\s*((?:\s*"(?:[^"\\]|\\.)*")+);', block)}
    topic = re.search(r'doc\["stat_t"\]\s*=\s*(\w+);', block)
    assert topic, f"the {object_id} block has no state topic"
    keys["stat_t"] = topic.group(1)
    return keys


def discovery_config(object_id: str, keys: dict[str, str]) -> str:
    """The config the block publishes for DEVICE (add_device_info's device)."""
    config: dict[str, Any] = {k: v for k, v in keys.items() if k != "stat_t"}
    config.update(
        stat_t=SENSING,
        uniq_id=f"securacv_{DEVICE}_{object_id}",
        dev={"ids": [f"securacv_canary_{DEVICE}"], "name": f"SecuraCV Canary {DEVICE}",
             "mf": "ERRERlabs", "mdl": "Canary v1", "sw": "2.4.19"},
    )
    return json.dumps(config)


async def announce(hass: HomeAssistant, object_id: str) -> str:
    keys = row(object_id)
    assert keys["stat_t"] == "s_topic_sensing", f"{object_id} moved off the sensing topic"
    async_fire_mqtt_message(
        hass,
        f"homeassistant/sensor/securacv_{DEVICE}/{object_id}/config",
        discovery_config(object_id, keys),
        retain=True,
    )
    await hass.async_block_till_done()
    entity_id = er.async_get(hass).async_get_entity_id("sensor", "mqtt", f"securacv_{DEVICE}_{object_id}")
    assert entity_id is not None, f"the discovery config created no {object_id} sensor"
    return entity_id


def sensing_body(motion: int, rssi_dbm: int | None) -> str:
    """mqtt_publish_sensing_update()'s body (its headline keys), with rssi_dbm
    only for a window whose mean was taken over a frame (None: no window yet,
    or one that heard nothing, so frames_in_window 0)."""
    body: dict[str, Any] = {"label": "quiet" if motion < 10 else "motion", "motion": motion, "breathing": 0}
    if rssi_dbm is not None:
        body["rssi_dbm"] = rssi_dbm
    body.update(frames_in_window=0 if rssi_dbm is None else 48, channel=6, time_bucket=61,
                acoustic_event="none", acoustic_conf=0,
                touch_event="none", touch_conf=0, touch_pad=0, ir_protocol="none", ir_bucket=0,
                ir_conf=0, temp_drift_active=False, temp_drift_conf=0, wake_reason="cold_boot")
    return json.dumps(body, separators=(",", ":"))


async def publish(hass: HomeAssistant, body: str) -> None:
    async_fire_mqtt_message(hass, SENSING, body, qos=0, retain=False)
    await hass.async_block_till_done()


def warnings_from_the_core(caplog: pytest.LogCaptureFixture) -> list[str]:
    """Every warning any Home Assistant logger wrote (a template that reads
    a key the body lacks is warned about by homeassistant.helpers.template)."""
    return [
        f"{r.name}: {r.getMessage()}"
        for r in caplog.records
        if r.levelno >= logging.WARNING and r.name.startswith("homeassistant")
    ]


async def test_sensing_rssi_is_unknown_until_a_window_and_keeps_its_reading(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, caplog: pytest.LogCaptureFixture
) -> None:
    caplog.set_level(logging.DEBUG, logger="homeassistant.components.mqtt.sensor")
    keys = row("sensing_rssi")
    assert (keys["name"], keys["unit_of_meas"], keys["dev_cla"]) == ("Sensing RSSI", "dBm", "signal_strength")
    assert "stat_cla" not in keys, "a state_class would change nothing here; re-read the rule if one is added"
    rssi = await announce(hass, "sensing_rssi")
    motion = await announce(hass, "motion_score")

    # Before the first CSI window: no reading, so unknown, never 0 dBm.
    await publish(hass, sensing_body(3, None))
    assert hass.states.get(rssi).state == "unknown"
    assert hass.states.get(motion).state == "3"
    assert (hass.states.get(rssi).attributes["unit_of_measurement"],
            hass.states.get(rssi).attributes["device_class"]) == ("dBm", "signal_strength")

    # The first window's reading, then the next.
    await publish(hass, sensing_body(40, -61))
    assert (hass.states.get(rssi).state, hass.states.get(motion).state) == ("-61", "40")
    await publish(hass, sensing_body(42, -58))
    assert hass.states.get(rssi).state == "-58"

    # A body without the key (a window that heard nothing, or a reboot before
    # its first window): the reading is kept by the core's numeric-sensor
    # rule; the other sensor moves on.
    caplog.clear()
    await publish(hass, sensing_body(5, None))
    assert hass.states.get(rssi).state == "-58", "the reading the body does not carry is kept"
    assert hass.states.get(motion).state == "5", "the body still updates the topic's other sensors"
    assert any(f"Ignore empty state from '{SENSING}'" in r.getMessage() for r in caplog.records)
    assert warnings_from_the_core(caplog) == []

    await publish(hass, sensing_body(6, -70))
    assert hass.states.get(rssi).state == "-70"


async def test_sensing_rssi_on_a_build_without_csi_stays_unknown(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, caplog: pytest.LogCaptureFixture
) -> None:
    """FEATURE_CSI 0: the entity is still announced, and no body ever carries
    rssi_dbm. It reads unknown for good, never 0 dBm, and warns about nothing."""
    rssi = await announce(hass, "sensing_rssi")
    motion = await announce(hass, "motion_score")
    for n in range(4):
        await publish(hass, sensing_body(n, None))
        assert hass.states.get(rssi).state == "unknown", n
        assert hass.states.get(motion).state == str(n)
    assert warnings_from_the_core(caplog) == []


async def test_sensing_rssi_after_an_upgrade_keeps_the_old_zero_until_a_reading(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, caplog: pytest.LogCaptureFixture
) -> None:
    """The cost of keeping the last reading, stated in
    docs/getting_started_canary.md: firmware from before the fix announced
    ``default(0)`` and published rssi_dbm 0 before a window; after the OTA
    the new config and bodies without the key leave that 0 standing, and the
    first reading replaces it."""
    keys = row("sensing_rssi")
    old_keys = {**keys, "val_tpl": "{{ value_json.rssi_dbm | default(0) }}"}
    assert old_keys["val_tpl"] != keys["val_tpl"]
    async_fire_mqtt_message(
        hass,
        f"homeassistant/sensor/securacv_{DEVICE}/sensing_rssi/config",
        discovery_config("sensing_rssi", old_keys),
        retain=True,
    )
    await hass.async_block_till_done()
    rssi = er.async_get(hass).async_get_entity_id("sensor", "mqtt", f"securacv_{DEVICE}_sensing_rssi")
    assert rssi is not None
    await publish(hass, sensing_body(3, 0))  # the old body's unmeasured zero
    assert hass.states.get(rssi).state == "0"

    assert await announce(hass, "sensing_rssi") == rssi  # the OTA: same entity, new template
    for n in range(3):
        await publish(hass, sensing_body(n, None))
        assert hass.states.get(rssi).state == "0", "the core keeps the old firmware's last state"
    await publish(hass, sensing_body(4, -61))
    assert hass.states.get(rssi).state == "-61"
    assert warnings_from_the_core(caplog) == []
