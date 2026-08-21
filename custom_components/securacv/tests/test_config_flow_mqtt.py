"""MQTT auto-discovery config flow — the MqttServiceInfo contract.

Since HA 2022.6, ``async_step_mqtt`` receives an ``MqttServiceInfo``: a
slots dataclass with attribute access only. The flow once read the topic
with ``discovery_info.get("topic", "")`` — dict API from before 2022.6 —
which raised AttributeError on the very first ``securacv/#`` publish and
killed the advertised discovery prompt. These tests drive the step with a
slots-dataclass stub shaped exactly like the real service info, so dict
access can never regress silently again.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

# config_flow.py subclasses ConfigFlow with the `domain=` class kwarg, which
# the base stubs' bare `object` cannot accept — upgrade the stub to a class
# that swallows it (the same lightweight-stub convention as the sensor
# platform stubs in test_modality_and_radar.py).
_ce_mod = sys.modules["homeassistant.config_entries"]


class _StubConfigFlow:
    def __init_subclass__(cls, domain: str | None = None, **kwargs) -> None:
        super().__init_subclass__(**kwargs)
        cls._stub_domain = domain


_ce_mod.ConfigFlow = _StubConfigFlow

from .. import config_flow  # noqa: E402


@dataclass(slots=True)
class _MqttServiceInfo:
    """Mirror of homeassistant.helpers.service_info.mqtt.MqttServiceInfo.

    A ``slots=True`` dataclass, exactly like the real one at the declared
    minimum HA version (2024.4.1) — no ``.get``, no ``__getitem__``.
    """

    topic: str
    payload: str
    qos: int
    retain: bool
    subscribed_topic: str
    timestamp: float


def _info(topic: str) -> _MqttServiceInfo:
    return _MqttServiceInfo(
        topic=topic,
        payload="online",
        qos=0,
        retain=False,
        subscribed_topic="securacv/#",
        timestamp=0.0,
    )


def _flow_with_captured_next_step():
    flow = config_flow.SecuraCVConfigFlow()
    flow.context = {}
    called: dict[str, bool] = {}

    async def _fake_mqtt_config(user_input=None):
        called["mqtt_config"] = True
        return {"type": "form", "step_id": "mqtt_config"}

    flow.async_step_mqtt_config = _fake_mqtt_config
    return flow, called


def test_service_info_stub_has_no_dict_api() -> None:
    """The premise of the regression: the real object rejects dict access."""
    info = _info("securacv/canary01/status")
    assert not hasattr(info, "get")
    assert not hasattr(info, "__getitem__")


def test_mqtt_discovery_reads_topic_as_attribute() -> None:
    """The step must survive a slots-dataclass service info and route on."""
    flow, called = _flow_with_captured_next_step()
    result = run(flow.async_step_mqtt(_info("securacv/canary01/status")))
    assert result == {"type": "form", "step_id": "mqtt_config"}
    assert called.get("mqtt_config") is True
    # The prefix seen on the wire pre-fills the manual form.
    assert flow.context["mqtt_prefix"] == "securacv"
    assert flow.context["title_placeholders"] == {"name": "SecuraCV (securacv)"}


def test_mqtt_discovery_with_short_topic_still_routes() -> None:
    """A degenerate topic sets no prefix but must not crash the flow."""
    flow, called = _flow_with_captured_next_step()
    run(flow.async_step_mqtt(_info("securacv")))
    assert called.get("mqtt_config") is True
    assert "mqtt_prefix" not in flow.context
