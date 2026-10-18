"""MQTT subscription lifecycle — nothing may leak across unload/reload.

The regression: every entity dropped the unsubscribe callable returned by
``mqtt.async_subscribe`` on the floor, and so did the wildcard discovery
subscriptions — ``async_unload_entry`` only unwound the two ``__init__.py``
subscriptions. After a reload, stale discovery closures kept feeding the
previous ``async_add_entities`` and removed entities kept writing state;
subscriptions accumulated per reload.

Now every entity hands its unsubscribe to ``async_on_remove`` (released by
HA when the entity is removed), and the discovery handles land in
``entry_data["unsub_mqtt"]`` (released by ``async_unload_entry``). These
tests drive both against a recording ``async_subscribe`` stub.
"""

from __future__ import annotations

import sys
import types

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import make_entry, make_runtime, run

# Reuse the platform stubs the hardening tests install (idempotent).
from .test_mqtt_payload_hardening import _install_platform_stubs

_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import async_unload_entry  # noqa: E402
from .. import binary_sensor as bs_platform  # noqa: E402
from .. import event as event_platform  # noqa: E402
from .. import sensor as sensor_platform  # noqa: E402
from ..const import DOMAIN  # noqa: E402

ENTRY = types.SimpleNamespace(entry_id="e1")
MQTT_STUB = sys.modules["homeassistant.components.mqtt"]
DISPATCHER_STUB = sys.modules["homeassistant.helpers.dispatcher"]


class _Recorder:
    """Records subscribe calls and which of them were later released."""

    def __init__(self) -> None:
        self.subscribed: list[str] = []
        self.released: list[str] = []

    async def async_subscribe(self, hass, topic, callback):
        self.subscribed.append(topic)
        return lambda: self.released.append(topic)


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {DOMAIN: {}}
    ENTRY.runtime_data = make_runtime(hass)
    return hass


# Every MQTT-subscribing entity class, with any extra constructor args.
ENTITY_CLASSES = [
    (sensor_platform.SecuraCVCanaryWitnessCountSensor, ()),
    (sensor_platform.SecuraCVCanaryChainLengthSensor, ()),
    (sensor_platform.SecuraCVCanaryLastEventSensor, ()),
    (sensor_platform.SecuraCVCanaryHealthSensor, ()),        # 2 subscriptions
    (sensor_platform.SecuraCVCanarySDWearSensor, ()),
    (sensor_platform.SecuraCVCanaryGPSSensor, ()),
    (sensor_platform.SecuraCVCanaryRadarLinkSensor, ()),
    (bs_platform.SecuraCVCanaryOnlineSensor, ()),
    (bs_platform.SecuraCVCanaryChainValidSensor, ()),
    (bs_platform.SecuraCVCanaryTamperSensor, ()),        # 2 subscriptions
    (bs_platform.SecuraCVCanaryTamperTypeSensor,
     ("power_loss", "mdi:power-plug-off")),  # 2 subscriptions
    (bs_platform.SecuraCVCanarySDReplaceSensor, ()),
    (bs_platform.SecuraCVCanaryEventIdSpaceLowSensor, ()),
    (bs_platform.SecuraCVCanaryTransportSensor,
     ("wifi_sta", "mdi:wifi")),
    (bs_platform.SecuraCVCanaryMotionSensor, ()),
    (bs_platform.SecuraCVCanaryOccupancySensor, ()),     # 2 subscriptions
    (bs_platform.SecuraCVCanaryMeshConnectedSensor, ()),
    (bs_platform.SecuraCVCanaryChirpActiveSensor, ()),
    (event_platform.SecuraCVCanaryTamperEventEntity, ()),
]


def test_every_entity_releases_its_subscriptions_on_remove(monkeypatch) -> None:
    for cls, extra in ENTITY_CLASSES:
        recorder = _Recorder()
        monkeypatch.setattr(MQTT_STUB, "async_subscribe", recorder.async_subscribe)

        inst = cls("securacv", "canary01", ENTRY, *extra)
        inst.hass = _hass()
        on_remove: list = []
        inst.async_on_remove = on_remove.append

        run(inst.async_added_to_hass())

        assert len(recorder.subscribed) >= 1, cls.__name__
        # Every MQTT subscription plus the one availability signal the base
        # connects (the device's online/offline dispatch).
        assert len(on_remove) == len(recorder.subscribed) + 1, (
            f"{cls.__name__} dropped an unsubscribe callable"
        )
        listeners = inst.hass.data["_securacv_test_dispatcher"]
        assert sum(len(v) for v in listeners.values()) == 1, cls.__name__
        # Releasing what was registered releases every subscription.
        for unsub in on_remove:
            unsub()
        assert sorted(recorder.released) == sorted(recorder.subscribed), cls.__name__
        assert sum(len(v) for v in listeners.values()) == 0, cls.__name__


def test_discovery_subscriptions_land_in_unsub_mqtt(monkeypatch) -> None:
    for setup, expected_topics in (
        (sensor_platform._setup_mqtt_sensors, 5),
        (bs_platform._setup_mqtt_binary_sensors, 8),
        (event_platform._async_setup_tamper_events, 2),
    ):
        recorder = _Recorder()
        monkeypatch.setattr(MQTT_STUB, "async_subscribe", recorder.async_subscribe)
        hass = _hass()
        runtime = ENTRY.runtime_data

        run(setup(hass, ENTRY, "securacv", lambda entities: None))

        assert len(recorder.subscribed) == expected_topics, setup.__name__
        assert len(runtime.unsub_mqtt) == expected_topics, (
            f"{setup.__name__} dropped a discovery unsubscribe"
        )
        for unsub in runtime.unsub_mqtt:
            unsub()
        assert sorted(recorder.released) == sorted(recorder.subscribed)


def test_unload_entry_releases_recorded_subscriptions() -> None:
    """The reload half: async_unload_entry unwinds unsub_mqtt and clears it,
    so a reload starts from zero subscriptions and a retried unload (Home
    Assistant retries when a platform refused) never unsubscribes twice."""
    released: list[str] = []
    hass = HomeAssistant()
    hass.data = {DOMAIN: {}}
    entry = make_entry(
        hass,
        runtime=make_runtime(
            hass, unsub_mqtt=[lambda: released.append("a"), lambda: released.append("b")]
        ),
    )

    assert run(async_unload_entry(hass, entry)) is True
    assert sorted(released) == ["a", "b"]
    assert entry.runtime_data.unsub_mqtt == []
    # Nothing was published: the claim is withdrawn only on removal.
    assert hass.data.get("_securacv_test_mqtt_published", []) == []
    assert run(async_unload_entry(hass, entry)) is True
    assert sorted(released) == ["a", "b"], "a retried unload must not release twice"
