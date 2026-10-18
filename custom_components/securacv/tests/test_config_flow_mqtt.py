"""MQTT auto-discovery config flow — the MqttServiceInfo contract.

Since HA 2022.6, ``async_step_mqtt`` receives an ``MqttServiceInfo``: a
slots dataclass with attribute access only. The flow once read the topic
with ``discovery_info.get("topic", "")`` — dict API from before 2022.6 —
which raised AttributeError on the very first ``securacv/#`` publish and
killed the advertised discovery prompt. These tests drive the step with the
real-shaped slots dataclass, so dict access can never regress silently
again. The step also keeps a dict fallback for older cores and stubs,
dedupes by ``mqtt_<prefix>`` unique_id, and routes on the topic shape: a
``witness/<id>/announce`` is a kernel offering itself (announce_confirm),
``<prefix>/claim/<id>`` is the integration's own retained claim (not a
device), anything else under ``securacv/#`` is a Canary (confirm).
"""

from __future__ import annotations

import json

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

from homeassistant.helpers.service_info.mqtt import MqttServiceInfo  # noqa: E402

from .. import config_flow  # noqa: E402
from ..const import DEFAULT_MQTT_PREFIX  # noqa: E402

ANNOUNCE = {
    "v": 1,
    "device_id": "pwk_1a2b3c4d",
    "kernel_version": "2.4.16",
    "verifying_key": "ab" * 32,
    "api": {"scheme": "http", "bind": "all", "port": 8799},
    "pairing": "required",
    "mqtt_topic_prefix": "witness",
    "discovery_prefix": "homeassistant",
    "availability_topic": "witness/status",
    "announced_bucket_start": 1760140800,
    "announced_bucket_size": 600,
}


def _info(topic: str, payload: str = "online") -> MqttServiceInfo:
    return MqttServiceInfo(
        topic=topic,
        payload=payload,
        qos=0,
        retain=False,
        subscribed_topic="securacv/#",
        timestamp=0.0,
    )


def _flow_with_captured_next_step():
    flow = config_flow.SecuraCVConfigFlow()
    flow.context = {}
    called: dict[str, object] = {}

    async def _fake_confirm(user_input=None):
        called["confirm"] = True
        return {"type": "form", "step_id": "confirm"}

    async def _fake_announce_confirm(user_input=None):
        called["announce_confirm"] = True
        return {"type": "form", "step_id": "announce_confirm"}

    async def _fake_set_unique_id(unique_id):
        called["unique_id"] = unique_id

    flow.async_step_confirm = _fake_confirm
    flow.async_step_announce_confirm = _fake_announce_confirm
    flow.async_set_unique_id = _fake_set_unique_id
    flow._abort_if_unique_id_configured = lambda *a, **kw: None
    flow._async_current_entries = lambda: []
    return flow, called


def test_service_info_has_no_dict_api() -> None:
    """The premise of the regression: the real object rejects dict access."""
    info = _info("securacv/canary01/status")
    assert not hasattr(info, "get")
    assert not hasattr(info, "__getitem__")


def test_mqtt_discovery_reads_topic_as_attribute() -> None:
    """The step must survive a slots-dataclass service info and route on."""
    flow, called = _flow_with_captured_next_step()
    result = run(flow.async_step_mqtt(_info("securacv/canary01/status")))
    assert result == {"type": "form", "step_id": "confirm"}
    assert called.get("confirm") is True
    # The prefix seen on the wire pre-fills the confirm step and keys dedupe;
    # the flow keeps it on itself (ConfigFlowContext has no key for it).
    assert flow._discovered_prefix == "securacv"
    assert flow.context["title_placeholders"] == {"name": "securacv"}
    assert called.get("unique_id") == "mqtt_securacv"


def test_mqtt_discovery_with_short_topic_still_routes() -> None:
    """A degenerate topic falls back to the default prefix, no crash."""
    flow, called = _flow_with_captured_next_step()
    run(flow.async_step_mqtt(_info("securacv")))
    assert called.get("confirm") is True
    assert flow._discovered_prefix == DEFAULT_MQTT_PREFIX


def test_mqtt_discovery_accepts_dict_payload_from_older_cores() -> None:
    """The dict fallback keeps pre-2022.6 cores and stubs working."""
    flow, called = _flow_with_captured_next_step()
    run(flow.async_step_mqtt({"topic": "barn/canary02/status"}))
    assert called.get("confirm") is True
    assert flow._discovered_prefix == "barn"
    assert called.get("unique_id") == "mqtt_barn"


def test_a_kernel_announce_routes_to_the_announce_confirm() -> None:
    """``witness/<id>/announce`` is a kernel, keyed on its device id, and the
    payload (bytes on the real wire) is parsed off the service info."""
    flow, called = _flow_with_captured_next_step()
    info = _info("witness/pwk_1a2b3c4d/announce", json.dumps(ANNOUNCE).encode())
    result = run(flow.async_step_mqtt(info))
    assert result == {"type": "form", "step_id": "announce_confirm"}
    assert called.get("announce_confirm") is True
    assert called.get("unique_id") == "kernel_pwk_1a2b3c4d"
    assert flow._announce is not None and flow._announce.device_id == "pwk_1a2b3c4d"
    assert flow.context["title_placeholders"] == {"name": "sidecar pwk_1a2b3c4d"}


def test_an_empty_announce_is_the_retained_clear_not_a_device() -> None:
    flow, called = _flow_with_captured_next_step()
    result = run(flow.async_step_mqtt(_info("witness/pwk_1a2b3c4d/announce", "")))
    assert result["type"] == "abort" and result["reason"] == "invalid_discovery_info"
    assert called == {}


def test_the_integrations_own_claim_is_not_a_device() -> None:
    flow, called = _flow_with_captured_next_step()
    result = run(flow.async_step_mqtt(_info("witness/claim/pwk_1a2b3c4d", "{}")))
    assert result["type"] == "abort" and result["reason"] == "not_securacv_device"
    assert called == {}
