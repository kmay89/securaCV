"""The canary-wap's Signal Strength across a reconnect, on a real core (sweep F368).

The canary-wap announces Signal Strength over MQTT discovery itself
(csi_mqtt.cpp's ENTITIES table, publish_one_discovery): a sensor on the
retained status topic, device_class signal_strength, state_class measurement,
dBm. Only the loop's publish_status() measures rssi. The connect burst's
status (``{"online":true,"device_type":"canary-wap"}``) and the last will
(``{"online":false}``) carry none, and the burst replaces the retained status
on the broker until the loop's next one.

So the row's template renders nothing when rssi is absent. This module
announces that row, read out of csi_mqtt.cpp, to the real MQTT integration
and drives it on what a reconnect sends: Home Assistant's MQTT sensor
(homeassistant/components/mqtt/sensor.py ``_update_state``, the same in the
2026.5 floor and 2026.10) ignores an empty rendering on a sensor whose state
must be numeric, which a state_class makes it. It logs that at debug level,
warns about nothing and keeps the last reading. A core that starts inside
that window has no reading to keep and shows unknown. The old template's
``default(0)`` rendered 0 dBm there.

The body shapes (the burst's status, the will, the loop's status) are held
to the real csi_mqtt.cpp's publishes by
custom_components/securacv/tests/test_wap_mqtt_contract.py and rendered with
jinja2 by scripts/tests/test_ha_discovery_wap_render.py; this lane renders
them through the core itself.

Host-tested: a real Home Assistant core over a mocked paho client, not a
unit on a bench and a real broker.
"""
from __future__ import annotations

import json
import logging
import re

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import REPO_ROOT

CSI_MQTT = REPO_ROOT / "firmware" / "projects" / "canary-wap" / "arduino" / "canary_wap" / "csi_mqtt.cpp"
DEVICE = "c0ffee01"
PREFIX = "securacv"
STATUS = f"{PREFIX}/{DEVICE}/status"

pytestmark = pytest.mark.skipif(
    not CSI_MQTT.is_file(), reason="monorepo-only: the HACS mirror carries no firmware source"
)


def _c_string(literal: str) -> str:
    """A C string literal's text (the escapes csi_mqtt.cpp's rows use)."""
    return literal.replace('\\"', '"').replace("\\\\", "\\")


def _source() -> str:
    return CSI_MQTT.read_text(encoding="utf-8")


def rssi_row() -> dict[str, str]:
    """csi_mqtt.cpp's Signal Strength row of ENTITIES, as publish_one_discovery reads it."""
    m = re.search(
        r'\{\s*"sensor",\s*"rssi",\s*"([^"]+)",\s*"(\w+)",\s*"((?:[^"\\]|\\.)*)",\s*'
        r'"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*nullptr\s*\}',
        _source(),
    )
    assert m, "csi_mqtt.cpp's Signal Strength row moved; re-read ENTITIES"
    name, topic, template, unit, device_class, state_class = m.groups()
    return {
        "name": name,
        "topic": topic,
        "template": _c_string(template),
        "unit": unit,
        "device_class": device_class,
        "state_class": state_class,
    }


def connect_burst_status() -> str:
    """The status MQTT_EVENT_CONNECTED publishes before the loop runs."""
    m = re.search(r'const char\* online = "((?:[^"\\]|\\.)*)";', _source())
    assert m, "csi_mqtt.cpp's connect-burst status moved; re-read MQTT_EVENT_CONNECTED"
    return _c_string(m.group(1))


def last_will() -> str:
    m = re.search(r'static const char will_msg\[\] = "((?:[^"\\]|\\.)*)";', _source())
    assert m, "csi_mqtt.cpp's last will moved; re-read its session config"
    return _c_string(m.group(1))


def loop_status(rssi: int) -> str:
    """publish_status()'s body: the loop's status, the one that carries rssi."""
    return json.dumps(
        {"online": True, "device_type": "canary-wap", "csi_running": True, "wifi_connected": True, "rssi": rssi},
        separators=(",", ":"),
    )


def discovery_config(row: dict[str, str]) -> str:
    """The config publish_one_discovery sends for the row (its abbreviated keys;
    def_ent_id is <component>.<device id slug>_<object_id>, and DEVICE is its
    own slug)."""
    return json.dumps(
        {
            "name": row["name"],
            "uniq_id": f"canary_{DEVICE}_rssi",
            "def_ent_id": f"sensor.{DEVICE}_rssi",
            "stat_t": f"{PREFIX}/{DEVICE}/{row['topic']}",
            "val_tpl": row["template"],
            "avty_t": STATUS,
            "avty_tpl": "{{ 'online' if value_json.online else 'offline' }}",
            "unit_of_meas": row["unit"],
            "dev_cla": row["device_class"],
            "stat_cla": row["state_class"],
            "dev": {
                "ids": [f"canary_{DEVICE}"],
                "name": f"Canary {DEVICE}",
                "mf": "SecuraCV",
                "mdl": "Canary WAP",
                "sw": "2.4.19",
            },
        }
    )


async def announce(hass: HomeAssistant) -> str:
    """Announce the row as the WAP does; the entity id the core gave it."""
    row = rssi_row()
    assert (row["topic"], row["device_class"], row["state_class"], row["unit"]) == (
        "status",
        "signal_strength",
        "measurement",
        "dBm",
    )
    async_fire_mqtt_message(
        hass, f"homeassistant/sensor/canary_{DEVICE}/rssi/config", discovery_config(row), retain=True
    )
    await hass.async_block_till_done()
    entity_id = er.async_get(hass).async_get_entity_id("sensor", "mqtt", f"canary_{DEVICE}_rssi")
    assert entity_id is not None, "the discovery config created no Signal Strength sensor"
    return entity_id


async def publish(hass: HomeAssistant, body: str, *, retained: bool = False) -> None:
    """A status, as the core's subscription gets it: live (a broker forwards a
    publish to an existing subscription with the retain flag cleared), or with
    `retained`, the broker's retained copy a fresh subscription is sent (the
    core takes one per topic and subscription, as a broker sends one)."""
    async_fire_mqtt_message(hass, STATUS, body, qos=0, retain=retained)
    await hass.async_block_till_done()


def warnings_from_the_core(caplog: pytest.LogCaptureFixture) -> list[str]:
    """Every warning any Home Assistant logger wrote. Not only the MQTT
    integration's: a template that reads a key the body lacks is warned
    about by homeassistant.helpers.template."""
    return [
        f"{r.name}: {r.getMessage()}"
        for r in caplog.records
        if r.levelno >= logging.WARNING and r.name.startswith("homeassistant")
    ]


async def test_signal_strength_keeps_its_reading_across_a_reconnect(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, caplog: pytest.LogCaptureFixture
) -> None:
    """The loop's reading survives the will and the connect burst's status:
    the entity goes unavailable on the will, available again on the burst,
    and shows the last reading until the loop measures a new one."""
    caplog.set_level(logging.DEBUG, logger="homeassistant.components.mqtt.sensor")
    entity = await announce(hass)
    burst = connect_burst_status()
    assert "rssi" not in json.loads(burst) and "rssi" not in json.loads(last_will())

    await publish(hass, loop_status(-58))
    state = hass.states.get(entity)
    assert (state.state, state.attributes["unit_of_measurement"]) == ("-58", "dBm")
    assert (state.attributes["device_class"], state.attributes["state_class"]) == (
        "signal_strength",
        "measurement",
    )

    await publish(hass, last_will())
    assert hass.states.get(entity).state == "unavailable"

    caplog.clear()
    await publish(hass, burst)
    assert hass.states.get(entity).state == "-58", "the reading the burst's status does not carry is kept"
    assert any(f"Ignore empty state from '{STATUS}'" in r.getMessage() for r in caplog.records), (
        "the core ignored the empty rendering by its numeric-sensor rule"
    )
    assert warnings_from_the_core(caplog) == []

    await publish(hass, loop_status(-61))
    assert hass.states.get(entity).state == "-61"


async def test_a_core_that_starts_inside_the_window_reads_unknown_not_zero(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, caplog: pytest.LogCaptureFixture
) -> None:
    """With only the burst's status retained, there is no reading to keep:
    Signal Strength is available and unknown, not 0 dBm, until the loop's status."""
    entity = await announce(hass)
    await publish(hass, connect_burst_status(), retained=True)
    assert hass.states.get(entity).state == "unknown"
    assert warnings_from_the_core(caplog) == []

    await publish(hass, loop_status(-70))
    assert hass.states.get(entity).state == "-70"
