"""A canary-wap's MQTT publishes, read the way Home Assistant reads them (F34).

The device-to-broker contract, from the real ``csi_mqtt.cpp``: the module
builds the canary-wap's host harness
(``firmware/projects/canary-wap/tests_host/test_wap_mqtt_bodies.cpp``,
``make -C <tests_host> test_wap_mqtt_bodies``), which runs it over a fake
esp_mqtt client through a unit's life (the connect burst, a healthy loop, a
tamper commit, a backfill replay, a unit in trouble, unsigned early-boot
bodies, discovery removed) and prints every publish it recorded, with its QoS
and retain flag, the last will and the command subscriptions (``--json``).
The signed bodies are signed for real, on the repo's test key (seed
0x42 * 32). Two bodies are the sketch's, not csi_mqtt.cpp's (the update
entity's state and the acoustic snapshot): the harness types them in the
sketch's shape and this module holds their keys to canary_wap.ino. That
record is then held to what reads it:

  * The delivery bound. Every publish is QoS 0 (at most once); the last will
    is QoS 1 and retained; the command topics are subscribed at QoS 1. State
    topics and discovery configs are retained, the events and tamper topics
    are not. So what Home Assistant shows from a retained topic survives a
    broker or Home Assistant restart, and an event is delivered once or
    never: no reader may count on a redelivery.
  * The integration's parsers, end to end. The integration is set up in MQTT
    mode against an emulated broker (a subscribe gets the retained message of
    every topic its filter matches; a publish reaches every matching
    subscription; an empty retained publish clears the topic), the platforms
    it forwards to are set up for real, and the record is replayed into it.
    Its entities are then read: the key TOFU-pinned from health, the signed
    chain / counts / events verifying, the Last Event's attributes and
    modality, the health and egress attributes, the tamper and event-id
    levels with the tamper body's type and severity, Chirp, unsigned bodies
    read as unsigned, the last will taking the device offline.
  * Home Assistant's MQTT discovery, at key and word level. The integration
    does not parse the discovery configs; Home Assistant's MQTT integration
    does, and it is not installed in this lane. So the configs are held to
    the record: every topic a config reads is one the WAP publishes (its
    availability topic retained), every ``value_json.<key>`` a template reads
    is in the bodies on its topic (in every body, unless the template gives a
    default and the gap is named below), each binary sensor's template names
    ON and OFF, the switches' state words are the ones the WAP publishes,
    the commands are the ones it subscribes to, and the removal clears every
    config. The templates are rendered, the way Home Assistant renders them,
    by ``scripts/tests/test_ha_discovery_wap_render.py`` (lint.yml, where
    jinja2 is installed at Home Assistant's pin); the harness itself delivers
    each command word back and checks the WAP acts on it.
  * The sketch's two relayed bodies. The update entity's state and the
    acoustic snapshot are built by canary_wap.ino and only relayed by
    csi_mqtt.cpp; the harness hands over bodies in the sketch's shape, and
    their keys are held to the keys the sketch writes.

What the record shows that is wrong today is written down as strict xfails,
each naming its sweep item or handed up (#1836): a test that starts passing
fails until its marker is removed.

Host-tested: the harness is the firmware's code compiled with g++ over stubs,
not a unit on a bench, and what Home Assistant's MQTT integration does with a
config was read from its documentation, not run. Reconnect behavior on a real
broker is bench work (U1).

The HACS mirror carries no firmware source, so the module skips there. A
missing ``make`` or C++ compiler, or a harness that does not build here (it
needs GCC, as CI builds it, and OpenSSL's headers: clang refuses one of its
GCC warning flags), skips it too, naming why, except under
``SECURACV_WAP_HARNESS=require`` (python.yml sets it), where it fails. A
harness that builds and whose own checks fail is a failure everywhere.
"""

from __future__ import annotations

import importlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path
from typing import Any

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from . import conftest  # noqa: F401  (installs the HA stubs at import time)
from .conftest import device_registry, make_entry, run
from .test_mqtt_payload_hardening import _install_platform_stubs

_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import async_setup_entry  # noqa: E402
from ..const import (  # noqa: E402
    CONF_ENABLE_MQTT,
    CRITICAL_MEMORY_THRESHOLD_BYTES,
    CONF_MQTT_PREFIX,
    CONF_SETUP_MODE,
    HOMEKIT_MOTION_HOLD_SECONDS,
    SETUP_MODE_MQTT,
)
from ..device_trust import PIN_SOURCE_TOFU, fingerprint_from_pubkey_hex  # noqa: E402
from ..health_metrics import egress_topic  # noqa: E402
from ..signature import verify_event  # noqa: E402

_REPO = Path(__file__).resolve().parents[3]
_WAP = _REPO / "firmware" / "projects" / "canary-wap"
_HOST = _WAP / "tests_host"
_HARNESS_SRC = _HOST / "test_wap_mqtt_bodies.cpp"
_HARNESS = _HOST / "test_wap_mqtt_bodies"
_SKETCH = _WAP / "arduino" / "canary_wap" / "canary_wap.ino"
_WORKFLOW = _REPO / ".github" / "workflows" / "python.yml"
_REQUIRE = "SECURACV_WAP_HARNESS"

pytestmark = pytest.mark.skipif(
    not _HARNESS_SRC.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)

MQTT_STUB = sys.modules["homeassistant.components.mqtt"]

# The four attributes every Canary entity carries for its trust verdict.
TRUST_KEYS = {"verified", "trust_reason", "pinned_fingerprint", "received_fingerprint"}

# The JSON keys Home Assistant's MQTT `update` platform reads off a state
# topic that has no value template (its documentation's "state_topic" JSON
# schema; read, not run in this lane).
HA_UPDATE_STATE_KEYS = {
    "installed_version",
    "latest_version",
    "title",
    "release_summary",
    "release_url",
    "entity_picture",
    "in_progress",
    "update_percentage",
}


# ─── the record ────────────────────────────────────────────────────────


def _toolchain_missing() -> list[str]:
    cxx = shlex.split(os.environ.get("CXX") or "g++")[:1] or ["g++"]
    return [tool for tool in ("make", *cxx) if shutil.which(tool) is None]


def _cannot_build(why: str) -> None:
    """Skip, or fail under SECURACV_WAP_HARNESS=require."""
    if os.environ.get(_REQUIRE) == "require":
        pytest.fail(f"{why} ({_REQUIRE}=require)")
    pytest.skip(f"{why}; {_REQUIRE}=require makes this a failure")


def _first_error(output: str) -> str:
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    errors = [ln for ln in lines if "error" in ln.lower()]
    return (errors or lines or ["no output"])[0]


@pytest.fixture(scope="module")
def record() -> dict[str, Any]:
    """The harness's record, built and run from the firmware sources.

    Every failure here is pytest.fail or a skip, never an assert, so a broken
    harness cannot pass as one of the strict xfails below (raises=AssertionError).
    """
    missing = _toolchain_missing()
    if missing:
        _cannot_build(f"cannot build the WAP MQTT harness: {', '.join(missing)} not on PATH")
    build = subprocess.run(
        ["make", "-C", str(_HOST), _HARNESS.name],
        capture_output=True,
        text=True,
        timeout=900,
        check=False,
    )
    if build.returncode != 0:
        if os.environ.get(_REQUIRE) == "require":
            pytest.fail(f"the harness did not build:\n{build.stdout}\n{build.stderr}")
        _cannot_build(
            f"the WAP MQTT harness does not build here ({_first_error(build.stderr)})"
        )
    out = subprocess.run(
        [str(_HARNESS), "--json"], capture_output=True, text=True, timeout=120, check=False
    )
    if out.returncode != 0:
        pytest.fail(f"the harness's own checks failed:\n{out.stderr}")
    data = json.loads(out.stdout)
    if data.get("harness") != _HARNESS.name:
        pytest.fail(f"not the harness's record: {data.get('harness')!r}")
    return data


def _require(condition: bool, why: str) -> None:
    """A strict xfail's precondition: pytest.fail, so a broken one fails
    instead of passing as the expected failure (raises=AssertionError)."""
    if not condition:
        pytest.fail(f"precondition: {why}")


def _publishes(record: dict[str, Any], *phases: str) -> list[dict[str, Any]]:
    """Every publish of the named phases (all of them with none), in order."""
    names = phases or tuple(p["name"] for p in record["phases"])
    known = {p["name"] for p in record["phases"]}
    assert set(names) <= known, f"no such phase: {set(names) - known}"
    return [pub for p in record["phases"] if p["name"] in names for pub in p["publishes"]]


def _topic(record: dict[str, Any], suffix: str) -> str:
    return f"{record['prefix']}/{record['device_id']}/{suffix}"


def _suffix(record: dict[str, Any], topic: str) -> str | None:
    head = f"{record['prefix']}/{record['device_id']}/"
    return topic[len(head):] if topic.startswith(head) else None


def _configs(record: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """The discovery configs of the connect burst, parsed."""
    return [
        (pub["topic"], json.loads(pub["payload"]))
        for pub in _publishes(record, "connect")
        if pub["topic"].endswith("/config")
    ]


def _bodies(record: dict[str, Any], topic: str) -> list[str]:
    """Every non-empty body the WAP sends on `topic`, the last will included."""
    bodies = [pub["payload"] for pub in _publishes(record) if pub["topic"] == topic]
    if record["last_will"]["topic"] == topic:
        bodies.append(record["last_will"]["payload"])
    return [b for b in bodies if b]


def _one(record: dict[str, Any], phase: str, suffix: str) -> dict[str, Any]:
    (body,) = [
        json.loads(pub["payload"])
        for pub in _publishes(record, phase)
        if pub["topic"] == _topic(record, suffix)
    ]
    return body


# ─── an MQTT broker, as Home Assistant's client sees one ─────────────


def _matches(topic_filter: str, topic: str) -> bool:
    """MQTT 3.1.1 topic-filter matching (`+` one level, `#` the rest)."""
    want, have = topic_filter.split("/"), topic.split("/")
    for i, level in enumerate(want):
        if level == "#":
            return True
        if i >= len(have) or (level != "+" and level != have[i]):
            return False
    return len(want) == len(have)


class _Hass(HomeAssistant):
    """Queues what the callbacks schedule (the TOFU pin) for the broker to run."""

    def __init__(self) -> None:
        super().__init__()
        self.scheduled: deque[Any] = deque()

    def async_create_task(self, coro, *args, **kwargs):
        self.scheduled.append(coro)


class _Broker:
    """A broker and Home Assistant's MQTT client, as the integration sees them.

    A subscribe gets the retained message of every topic its filter matches
    (flagged retained), a publish reaches every matching subscription (the
    flag cleared, as a broker forwards it), a retained publish replaces the
    topic's retained message and an empty one clears it. Deliveries, the
    entities a callback adds and the tasks it schedules are queued and run
    one at a time, from no event loop, as Home Assistant's client runs
    subscription callbacks: so a message reaches only the subscriptions that
    exist when it is delivered.

    With `hold_retained` set, a subscription is live at once but its
    retained messages wait for `release_retained()`: Home Assistant batches
    a new SUBSCRIBE (mqtt/client.py SUBSCRIBE_COOLDOWN), so a publish that
    arrives in that window reaches the new subscription before the broker's
    retained copy does, and the copy is what the broker holds when the
    SUBSCRIBE is sent.
    """

    def __init__(self, hass: _Hass) -> None:
        self.hass = hass
        self.retained: dict[str, str] = {}
        self.subscriptions: list[dict[str, Any]] = []
        self.deliveries: deque[tuple[dict[str, Any], str, str, bool]] = deque()
        self.adding: list[Any] = []
        self.entities: list[Any] = []
        self.hold_retained = False
        self.held: list[dict[str, Any]] = []

    async def async_subscribe(self, hass, topic, msg_callback, qos=0, encoding="utf-8"):
        sub = {"filter": topic, "callback": msg_callback, "active": True}
        self.subscriptions.append(sub)
        if self.hold_retained:
            self.held.append(sub)
        else:
            self._queue_retained(sub)

        def _unsubscribe() -> None:
            sub["active"] = False

        return _unsubscribe

    def _queue_retained(self, sub: dict[str, Any]) -> None:
        for retained_topic, payload in self.retained.items():
            if _matches(sub["filter"], retained_topic):
                self.deliveries.append((sub, retained_topic, payload, True))

    def release_retained(self) -> None:
        """Send the held subscriptions the retained messages held now."""
        held, self.held = self.held, []
        for sub in held:
            self._queue_retained(sub)
        self.pump()

    def add_entities(self, entities, update_before_add: bool = False) -> None:
        self.adding.extend(entities)

    def publish(self, topic: str, payload: str, retain: bool) -> None:
        if retain:
            if payload:
                self.retained[topic] = payload
            else:
                self.retained.pop(topic, None)
        for sub in self.subscriptions:
            if sub["active"] and _matches(sub["filter"], topic):
                self.deliveries.append((sub, topic, payload, False))
        self.pump()

    def pump(self) -> None:
        while self.adding or self.hass.scheduled or self.deliveries:
            if self.adding:
                entity = self.adding.pop(0)
                entity.hass = self.hass
                entity.writes = 0

                def _write(entity=entity) -> None:
                    entity.writes += 1

                entity.async_write_ha_state = _write
                self.entities.append(entity)
                run(entity.async_added_to_hass())
            elif self.hass.scheduled:
                run(self.hass.scheduled.popleft())
            else:
                sub, topic, payload, retained = self.deliveries.popleft()
                if sub["active"]:
                    sub["callback"](
                        MQTT_STUB.ReceiveMessage(
                            topic=topic,
                            payload=payload,
                            qos=0,
                            retain=retained,
                            subscribed_topic=sub["filter"],
                        )
                    )


class _Wap:
    """The integration, set up in MQTT mode, with a canary-wap's record replayed.

    `retained` seeds the broker before the integration subscribes: what a
    broker holds when Home Assistant starts after the device published.
    """

    def __init__(
        self, record: dict[str, Any], monkeypatch, retained: dict[str, str] | None = None
    ) -> None:
        self.record = record
        self.hass = _Hass()
        self.hass.data = {}
        self.broker = _Broker(self.hass)
        self.broker.retained.update(retained or {})
        monkeypatch.setattr(MQTT_STUB, "async_subscribe", self.broker.async_subscribe)
        self.entry = make_entry(
            data={
                CONF_ENABLE_MQTT: True,
                CONF_MQTT_PREFIX: record["prefix"],
                CONF_SETUP_MODE: SETUP_MODE_MQTT,
            }
        )
        self.hass.config_entries.add(self.entry)
        assert run(async_setup_entry(self.hass, self.entry)) is True
        ((_, platforms),) = self.hass.config_entries.forwarded
        for platform in platforms:
            module = importlib.import_module(f"..{platform}", __package__)
            run(module.async_setup_entry(self.hass, self.entry, self.broker.add_entities))
        self.broker.pump()

    def replay(self, *phases: str) -> _Wap:
        for pub in _publishes(self.record, *phases):
            self.broker.publish(pub["topic"], pub["payload"], pub["retain"])
        return self

    def last_will(self) -> _Wap:
        will = self.record["last_will"]
        self.broker.publish(will["topic"], will["payload"], will["retain"])
        return self

    def entity(self, key: str, device_id: str | None = None) -> Any:
        uid = f"securacv_canary_{device_id or self.record['device_id']}_{key}"
        found = [e for e in self.broker.entities if e.unique_id == uid]
        assert len(found) <= 1, f"two entities with unique id {uid}"
        return found[0] if found else None

    def keys(self) -> set[str]:
        head = f"securacv_canary_{self.record['device_id']}_"
        return {e.unique_id[len(head):] for e in self.broker.entities}

    @property
    def store(self):
        return self.entry.runtime_data.trust_store


def _value(entity: Any, name: str) -> Any:
    """What Home Assistant would read: a property the class defines, else `_attr_<name>`."""
    for klass in type(entity).__mro__:
        if isinstance(vars(klass).get(name), property):
            return getattr(entity, name)
    return getattr(entity, f"_attr_{name}", None)


def _attrs(entity: Any) -> dict[str, Any]:
    return dict(_value(entity, "extra_state_attributes") or {})


def _snapshot(entity: Any) -> tuple[Any, Any, str]:
    """An entity's state and attributes, less the trust verdict and timestamps."""
    attrs = {
        k: v
        for k, v in _attrs(entity).items()
        if k not in TRUST_KEYS and k != "last_triggered"
    }
    return (
        _value(entity, "native_value"),
        _value(entity, "is_on"),
        json.dumps(attrs, sort_keys=True, default=str),
    )


# ─── the delivery bound ────────────────────────────────────────────────


def test_every_publish_is_at_most_once_and_the_last_will_is_at_least_once(record):
    """QoS 0 is the delivery bound: the integration must not count on a redelivery."""
    pubs = _publishes(record)
    assert pubs, "the harness recorded nothing"
    assert {p["qos"] for p in pubs} == {0}, [p["topic"] for p in pubs if p["qos"] != 0]
    will = record["last_will"]
    assert will == {
        "topic": _topic(record, "status"),
        "payload": '{"online":false}',
        "qos": 1,
        "retain": True,
    }
    assert {s["qos"] for s in record["subscriptions"]} == {1}
    assert sorted(s["topic"] for s in record["subscriptions"]) == sorted(
        _topic(record, t) for t in ("update/cmd", "update/auto/cmd", "mic/cmd")
    )


def test_state_is_retained_and_events_are_not(record):
    for pub in _publishes(record):
        event = _suffix(record, pub["topic"]) in ("events", "tamper")
        assert pub["retain"] is not event, pub["topic"]


# ─── the integration's parsers, end to end ─────────────────────────────


def test_the_record_is_signed_with_the_repos_test_key(record):
    """The harness's key is the repo's test key, derived here independently."""
    pub = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32).public_key()
    assert record["public_key"] == pub.public_bytes_raw().hex()
    assert record["fingerprint"] == fingerprint_from_pubkey_hex(record["public_key"])


def test_a_wap_is_registered_from_its_status_and_goes_offline_on_its_last_will(
    record, monkeypatch
):
    wap = _Wap(record, monkeypatch).replay("connect")
    device_id = record["device_id"]
    assert wap.entry.runtime_data.devices[device_id]["online"] is True
    assert _value(wap.entity("online"), "is_on") is True
    (device,) = device_registry(wap.hass).devices
    assert device.identifiers == {("securacv", f"canary_{device_id}")}

    wap.replay("running").last_will()
    assert wap.entry.runtime_data.devices[device_id]["online"] is False
    online = wap.entity("online")
    assert _value(online, "is_on") is False
    assert online.available is True, "Online stays available to report offline"
    others = [e for e in wap.broker.entities if e is not online]
    assert others and not [e.unique_id for e in others if e.available], (
        "every other entity follows the last will to unavailable"
    )


def test_the_device_shows_the_firmware_the_wap_reports(record, monkeypatch):
    """The WAP's status carries no firmware_version and its health does; the
    device page reads it from the health (ledger HA45, device-details PR 2)."""
    statuses = [json.loads(body) for body in _bodies(record, _topic(record, "status"))]
    assert statuses and not [s for s in statuses if "firmware_version" in s]
    assert _one(record, "running", "health")["firmware_version"] == record["firmware_version"]
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    (device,) = device_registry(wap.hass).devices
    assert device.sw_version == record["firmware_version"]


def test_health_pins_the_key_and_the_signed_topics_verify(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    pin = wap.store.get(record["device_id"])
    assert pin is not None and pin.pin_source == PIN_SOURCE_TOFU
    assert pin.pubkey_hex == record["public_key"]
    assert pin.fingerprint_hex == record["fingerprint"]

    chain = _one(record, "running", "chain")
    length = wap.entity("chain_length")
    assert _value(length, "native_value") == chain["length"]
    assert _attrs(length)["latest_hash"] == chain["latest_hash"]
    counts = _one(record, "running", "counts")
    for key, value in (("chain_length", chain["length"]), ("witness_count", counts["total"])):
        ent = wap.entity(key)
        assert _value(ent, "native_value") == value, key
        attrs = _attrs(ent)
        assert (attrs["verified"], attrs["trust_reason"]) == (True, "ok"), (key, attrs)
        assert attrs["received_fingerprint"] == record["fingerprint"], key
    assert _value(wap.entity("chain_valid"), "is_on") is True
    assert wap.hass.data.get("_securacv_test_notifications", []) == []

    # The signed event, through the verifier the Last Event sensor runs (the
    # sensor shows it too, HA44: test_the_first_event_is_shown_by_the_entity_it_creates).
    event = _one(record, "running", "events")
    verdict = verify_event(wap.store, record["device_id"], event)
    assert verdict.trusted and verdict.reason == "ok", verdict


def test_last_event_reads_the_wap_event_body(record, monkeypatch):
    """Each field of the events body Last Event reads, and the modality it
    derives from the status's device_type, on a second publish of the same
    body once the entity exists (the creating publish's own showing is
    test_the_first_event_is_shown_by_the_entity_it_creates, HA44)."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    sent = _one(record, "running", "events")
    status = _one(record, "connect", "status")
    assert status["device_type"] == "canary-wap"
    last = wap.entity("last_event")
    assert last is not None, "the first events publish creates Last Event"
    wap.broker.publish(_topic(record, "events"), json.dumps(sent), False)
    attrs = _attrs(last)
    assert _value(last, "native_value") == sent["event_type"] == sent["state"] == "active"
    for key in ("timestamp", "zone", "confidence", "signed"):
        assert attrs[key] == sent[key], key
    assert attrs["signed"] is True
    assert (attrs["verified"], attrs["trust_reason"]) == (True, "ok")
    assert attrs["received_fingerprint"] == sent["fp"] == record["fingerprint"]
    assert attrs["modality"] == "wifi-csi", "a canary-wap's status names it a CSI sensor"


def test_health_and_egress_read_as_the_wap_sent_them(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    health = wap.entity("health_status")
    sent = _one(record, "running", "health")
    attrs = _attrs(health)
    assert _value(health, "native_value") == "healthy"
    assert attrs["battery_percent"] == sent["battery"]
    assert attrs["battery_present"] is sent["battery_present"] is False
    assert attrs["memory_free_bytes"] == sent["memory_free"]
    assert attrs["uptime_seconds"] == sent["uptime"]
    assert attrs["firmware_version"] == sent["firmware_version"] == record["firmware_version"]
    assert attrs["public_key"] == sent["public_key"]
    assert (attrs["verified"], attrs["trust_reason"]) == (False, "unsigned")
    egress = _one(record, "running", "egress")
    parsed = egress_topic(egress)
    assert parsed is not None, "the integration does not read the WAP's egress body"
    assert attrs["csi_event_egress"] == egress["csi_event_egress"] == parsed[1]

    wap.replay("trouble")
    sent = _one(record, "trouble", "health")
    attrs = _attrs(health)
    assert _value(health, "native_value") == "critical", "8% battery, 9 kB free"
    assert attrs["battery_percent"] == sent["battery"]
    assert attrs["battery_present"] is True
    for key in ("charge_state", "battery_health_pct", "battery_mv"):
        assert attrs[key] == sent[key], key


def test_tamper_and_event_id_levels_follow_the_wap(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running", "tamper")
    events = wap.entity("tamper_events")
    tamper = _one(record, "tamper", "tamper")
    assert events.state == tamper["type"] == "enclosure"
    assert [kind for kind, _ in events.fired] == ["enclosure"]
    enclosure = wap.entity("tamper_enclosure")
    assert _value(enclosure, "is_on") is True
    assert _attrs(enclosure)["severity"] == tamper["severity"] == "tamper"
    aggregate = wap.entity("tamper")
    assert _value(aggregate, "is_on") is True
    assert _attrs(aggregate)["tamper_type"] == tamper["type"]
    # The row itself rides events too, as the anomaly the device trigger fires on.
    row = _one(record, "tamper", "events")
    assert (row["category"], row["module"], row["state"]) == (
        "anomaly",
        "system.integrity",
        tamper["type"],
    )
    assert _value(wap.entity("tamper_sd_remove"), "is_on") is False
    assert _value(wap.entity("event_id_space_low"), "is_on") is False

    wap.replay("trouble")
    sent = _one(record, "trouble", "health")
    assert (sent["enclosure_open"], sent["sd_mounted"], sent["event_id_space_low"]) == (
        True,
        False,
        True,
    )
    assert _value(wap.entity("tamper_enclosure"), "is_on") is True
    assert _value(wap.entity("tamper_sd_remove"), "is_on") is True
    assert _value(wap.entity("event_id_space_low"), "is_on") is True
    assert [kind for kind, _ in events.fired] == ["enclosure"], "health fires no event"


def test_chirp_active_follows_the_wap_word(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    chirp = wap.entity("chirp_active")
    assert _one(record, "running", "chirp") == {"state": "active"}
    assert _value(chirp, "is_on") is True
    assert _attrs(chirp)["channel_state"] == "active"
    wap.replay("trouble")
    assert _value(chirp, "is_on") is False
    assert _attrs(chirp)["channel_state"] == "disabled"


def test_unsigned_bodies_read_as_unsigned(record, monkeypatch):
    """What an early boot sends, before device_signature is ready: shown, not trusted."""
    wap = _Wap(record, monkeypatch).replay("connect", "running", "unsigned")
    for key, value in (
        ("witness_count", _one(record, "unsigned", "counts")["total"]),
        ("chain_length", _one(record, "unsigned", "chain")["length"]),
    ):
        ent = wap.entity(key)
        assert _value(ent, "native_value") == value, key
        attrs = _attrs(ent)
        assert (attrs["verified"], attrs["trust_reason"]) == (False, "unsigned"), key
    assert _value(wap.entity("chain_valid"), "is_on") is False
    last = wap.entity("last_event")
    sent = _one(record, "unsigned", "events")
    assert _value(last, "native_value") == sent["state"] == "empty"
    attrs = _attrs(last)
    assert (attrs["signed"], attrs["verified"], attrs["trust_reason"]) == (False, False, "unsigned")
    assert attrs["confidence"] == sent["confidence"]


def test_every_wap_topic_has_a_reader(record, monkeypatch):
    """Each topic the WAP publishes is read by the integration or by a discovery
    config, and each per-device topic the integration reads that a WAP never
    publishes belongs to another product."""
    wap = _Wap(record, monkeypatch).replay()
    filters = {s["filter"] for s in wap.broker.subscriptions}
    by_config = {
        cfg.get("stat_t") or cfg.get("topic") for _, cfg in _configs(record)
    }
    published = {
        pub["topic"] for pub in _publishes(record) if not pub["topic"].endswith("/config")
    }
    unread = {
        t for t in published if not any(_matches(f, t) for f in filters) and t not in by_config
    }
    assert unread == set(), f"nothing reads {sorted(unread)}"
    # The canary base's availability and transport topics, canary-sense's state.
    device = {_suffix(record, f) for f in filters} - {None}
    wildcard = {f.rsplit("/", 1)[1] for f in filters if f.split("/")[1] == "+"}
    sent = {_suffix(record, t) for t in published}
    assert (device | wildcard) - sent == {"availability", "transport", "state"}


# ─── Home Assistant's MQTT discovery, held to the record ───────────────

_VALUE_JSON = re.compile(r"value_json\.(\w+)(\s*\|\s*default\()?")


def _template_reads(template: str) -> list[tuple[str, bool]]:
    """(key, has a default) for every value_json key a template reads."""
    assert "value_json[" not in template, f"a template this test cannot read: {template}"
    return [(m.group(1), m.group(2) is not None) for m in _VALUE_JSON.finditer(template)]


def test_discovery_configs_read_topics_the_wap_publishes(record):
    retained = {pub["topic"] for pub in _publishes(record) if pub["retain"] and pub["payload"]}
    sent = {pub["topic"] for pub in _publishes(record)}
    subscribed = {s["topic"] for s in record["subscriptions"]}
    configs = _configs(record)
    assert len(configs) == 33, "27 entity configs and 6 device triggers"
    for config_topic, cfg in configs:
        component = config_topic.split("/")[1]
        assert cfg["dev"]["ids"] == [f"canary_{record['device_id']}"], config_topic
        assert cfg["dev"]["sw"] == record["firmware_version"], config_topic
        if component == "device_automation":
            assert cfg["automation_type"] == "trigger", config_topic
            assert cfg["topic"] in sent, config_topic
            continue
        assert cfg["stat_t"] in sent, (config_topic, cfg["stat_t"])
        # Availability must survive a restart: the status topic is retained.
        assert cfg["avty_t"] == _topic(record, "status") and cfg["avty_t"] in retained, config_topic
        if "cmd_t" in cfg:
            assert cfg["cmd_t"] in subscribed, (config_topic, cfg["cmd_t"])


# A key a template reads with a default and that some body on its topic does
# not carry, and why the default is what the entity shows then. Held both
# ways: a new gap needs its reason here, and a closed one must leave.
DEFAULTED_GAPS = {
    ("status", "rssi"): (
        "the connect burst's status and the last will carry no rssi: the "
        "template renders nothing; HA keeps the last reading (sweep F368); "
        "after the will the entity is unavailable anyway"
    ),
}


def test_discovery_templates_read_keys_the_bodies_carry(record):
    """A key read without a default is in every body on its topic; one read with
    a default is in at least one (else the default is all the entity shows),
    and is missing from one only where DEFAULTED_GAPS says why."""
    read = 0
    gaps: set[tuple[str | None, str]] = set()
    for config_topic, cfg in _configs(record):
        pairs = [(cfg.get("stat_t") or cfg.get("topic"), cfg.get("val_tpl"))]
        pairs.append((cfg.get("avty_t"), cfg.get("avty_tpl")))
        for topic, template in pairs:
            if not template:
                continue
            bodies = [json.loads(b) for b in _bodies(record, topic)]
            assert bodies, (config_topic, topic)
            for key, defaulted in _template_reads(template):
                read += 1
                have = [key in b for b in bodies]
                if defaulted:
                    assert any(have), (config_topic, topic, key)
                    if not all(have):
                        gaps.add((_suffix(record, topic), key))
                else:
                    assert all(have), (config_topic, topic, key, have)
    assert read > 30, "the templates read fewer keys than the configs carry"
    assert gaps == set(DEFAULTED_GAPS), "name each defaulted gap's reason, or drop a closed one"


def test_discovery_payload_words_match_the_bodies(record):
    """At word level: each binary sensor's template names ON and OFF (what
    each renders, body by body, is test_ha_discovery_wap_render.py's), the
    switches' state words are the ones the WAP publishes, the update state is
    keys Home Assistant reads, and the presence triggers' words are states the
    WAP's presence rows carry."""
    configs = dict(_configs(record))
    by_object = {t.split("/")[-2]: cfg for t, cfg in configs.items()}
    for topic, cfg in configs.items():
        if topic.split("/")[1] == "binary_sensor":
            # Home Assistant's defaults: payload_on "ON", payload_off "OFF".
            assert "pl_on" not in cfg and "pl_off" not in cfg, topic
            assert "ON" in cfg["val_tpl"] and "OFF" in cfg["val_tpl"], topic
    mic = by_object["mic_mute"]
    assert set(_bodies(record, mic["stat_t"])) <= {mic["stat_on"], mic["stat_off"]}
    auto = by_object["auto_update"]
    # No stat_on / stat_off: the switch's state words default to "ON" / "OFF".
    assert "stat_on" not in auto and "stat_off" not in auto
    assert set(_bodies(record, auto["stat_t"])) <= {"ON", "OFF"}
    update = by_object["firmware"]
    assert "val_tpl" not in update
    for body in _bodies(record, update["stat_t"]):
        assert set(json.loads(body)) <= HA_UPDATE_STATE_KEYS, body
    # The presence triggers fire on the states the WAP's presence events carry.
    trigger_states = {
        cfg["pl"] for cfg in configs.values() if cfg.get("type") == "presence_changed"
    }
    events = [json.loads(b) for b in _bodies(record, _topic(record, "events"))]
    presence = [e for e in events if e["module"] == "core.presence"]
    assert presence and {e["state"] for e in presence} <= trigger_states


def test_discovery_removal_clears_every_config(record):
    removed = _publishes(record, "discovery_removed")
    assert sorted(p["topic"] for p in removed) == sorted(t for t, _ in _configs(record))
    assert all(p["payload"] == "" and p["retain"] for p in removed)
    retained: dict[str, str] = {}
    for pub in _publishes(record):
        if pub["retain"]:
            if pub["payload"]:
                retained[pub["topic"]] = pub["payload"]
            else:
                retained.pop(pub["topic"], None)
    assert not [t for t in retained if t.endswith("/config")], "a config survives its removal"


# ─── the sketch's two relayed bodies ───────────────────────────────────


def _sketch_function(text: str, signature: str) -> str:
    m = re.search(re.escape(signature) + r"\s*\{(.*?)\n\}\n", text, re.S)
    assert m, f"canary_wap.ino's {signature} moved; re-read what it publishes"
    return m.group(1)


def test_the_relayed_bodies_carry_the_keys_the_sketch_writes(record):
    text = _SKETCH.read_text(encoding="utf-8")
    m = re.search(r"snprintf\(sensing_json,\s*sizeof\(sensing_json\),(.*?)\);", text, re.S)
    assert m, "canary_wap.ino's acoustic snapshot moved; re-read what it publishes"
    sketch_sensing = set(re.findall(r'\\"(\w+)\\":', m.group(1)))
    (sensing,) = _bodies(record, _topic(record, "sensing"))
    assert set(json.loads(sensing)) == sketch_sensing, "kSensing in the harness is stale"

    body = _sketch_function(text, "static void ota_publish_update_state()")
    sketch_update = set(re.findall(r'doc\["(\w+)"\]', body))
    assert sketch_update <= HA_UPDATE_STATE_KEYS, sketch_update - HA_UPDATE_STATE_KEYS
    # The two keys the sketch sets only when a release is on offer; the
    # harness's body is the no-update shape, every other key the sketch sets.
    offer_only = {"release_url", "release_summary"}
    assert offer_only <= sketch_update, "the sketch's offer-only keys moved"
    (update,) = _bodies(record, _topic(record, "update/state"))
    assert set(json.loads(update)) == sketch_update - offer_only, (
        "kUpdateState in the harness is stale"
    )


# ─── the CI wiring ─────────────────────────────────────────────────────


def _harness_inputs() -> set[str]:
    """Every file the harness's compile read, from the dependency file make
    wrote beside it: the union of every rule whose target is the harness.
    with_deps runs -MM over each source the recipe compiles, so there is one
    such rule per source (the harness's own TU, then csi_mqtt.cpp's); -MP's
    empty rules, one per header, name the header as their target and add
    nothing."""
    deps = Path(f"{_HARNESS}.d").read_text(encoding="utf-8").replace("\\\n", " ")
    rules = [line.partition(":") for line in deps.splitlines() if line.strip()]
    words = [w for target, _, dep in rules if target.strip() == _HARNESS.name for w in dep.split()]
    inputs = {(_HOST / w).resolve().relative_to(_REPO).as_posix() for w in words}
    # So a change to with_deps or to the recipe cannot quietly shrink the set
    # this test holds python.yml to: both compiled sources must be in it.
    for source in (_HARNESS_SRC, _WAP / "arduino" / "canary_wap" / "csi_mqtt.cpp"):
        assert source.relative_to(_REPO).as_posix() in inputs, (
            f"{_HARNESS.name}.d names no rule for {source.name}: the recipe or with_deps moved"
        )
    return inputs


def _paths_listed(paths: list[str], rel: str) -> bool:
    """`rel` is one of a workflow trigger's `paths`, or under one of its `/**` globs."""
    return rel in paths or any(p.endswith("/**") and rel.startswith(p[:-2]) for p in paths)


def test_python_yml_runs_this_suite_on_each_file_the_harness_compiles(record):
    """A firmware-only edit to anything the harness compiles still runs this
    module: python.yml's push and pull_request paths list each file the
    compile read (every rule of its dependency file, written by make), the
    Makefile, and this module's other inputs, and the step that runs this
    suite requires the harness."""
    if not _WORKFLOW.is_file():
        pytest.skip("no python.yml beside this tree")
    import yaml

    inputs = _harness_inputs() | {
        (_HOST / "Makefile").relative_to(_REPO).as_posix(),
        _SKETCH.relative_to(_REPO).as_posix(),
    }
    workflow = yaml.safe_load(_WORKFLOW.read_text(encoding="utf-8"))
    on = workflow.get("on", workflow.get(True))   # YAML 1.1 reads a bare `on` as true
    for trigger in ("push", "pull_request"):
        paths = on[trigger]["paths"]
        missing = sorted(rel for rel in inputs if not _paths_listed(paths, rel))
        assert missing == [], f"python.yml's {trigger} paths must list {missing}"
    steps = [
        step
        for job in workflow["jobs"].values()
        for step in job.get("steps", [])
        if str(step.get("run", "")).split() == ["pytest", "custom_components/securacv/tests/", "-q"]
    ]
    assert len(steps) == 1, "python.yml runs this suite in exactly one step"
    assert (steps[0].get("env") or {}).get(_REQUIRE) == "require", (
        f"the step that runs this suite must set {_REQUIRE}: require"
    )


# ─── the publish that creates an entity (HA44) ─────────────────────────


class _Deliveries:
    """What reaches the voice brief and the hold-window timers: every
    _record_last_event call Last Event makes, and every hold window the
    projected sensors arm (async_call_later)."""

    def __init__(self, monkeypatch) -> None:
        sensor = importlib.import_module("..sensor", __package__)
        binary_sensor = importlib.import_module("..binary_sensor", __package__)
        self.recorded: list[tuple[str, Any]] = []
        self.armed: list[float] = []
        record_last_event = sensor._record_last_event

        def _record(hass, entry, device_id, event_type) -> None:
            self.recorded.append((device_id, event_type))
            record_last_event(hass, entry, device_id, event_type)

        def _call_later(hass, delay, action):
            self.armed.append(delay)
            return lambda: None

        monkeypatch.setattr(sensor, "_record_last_event", _record)
        monkeypatch.setattr(binary_sensor, "async_call_later", _call_later)


def test_the_first_event_is_shown_by_the_entity_it_creates(record, monkeypatch):
    """Sweep HA44: the canary-wap's events are not retained, and the first
    one creates Last Event, which subscribes after that delivery. It is
    handed the body and shows it through the path every publish takes: the
    signature verifies, the voice brief records it once, and the replay
    gate's mark is the creating event's, so the backfilled row (an older
    event_id) is refused as a replay rather than shown as the latest."""
    seen = _Deliveries(monkeypatch)
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    sent = _one(record, "running", "events")
    device = record["device_id"]
    last = wap.entity("last_event")
    assert last is not None, "the first events publish creates Last Event"
    assert _value(last, "native_value") == sent["state"]
    assert _attrs(last)["verified"] is True
    assert seen.recorded == [(device, sent["state"])]

    backfill = _one(record, "backfill", "events")
    assert backfill["event_id"] < sent["event_id"], "the backfilled row is older"
    wap.replay("backfill")
    assert _value(last, "native_value") == sent["state"]
    assert _attrs(last)["trust_reason"] == "replay"
    assert seen.recorded == [(device, sent["state"])]

    # Added again (the core re-adds an entity on a registry change): the
    # creating body was dropped after use, so nothing is replayed.
    writes = last.writes
    run(last.async_added_to_hass())
    wap.broker.pump()
    assert seen.recorded == [(device, sent["state"])]
    assert last.writes == writes


# A Canary whose events carry the dictionary's vocabulary: a canary-wap's
# presence words map to no Apple Home signal (handed up, HA47), so the
# creating publish that asserts one is this one's.
_PORCH = "porch01"
_SIGNALS = {"motion": "BoundaryCrossingObjectLarge", "occupancy": "PresenceInRestrictedZone"}


def _porch_event(event_type: str, event_id: int = 41) -> str:
    return json.dumps(
        {
            "event_id": event_id,
            "event_type": event_type,
            "timestamp": 3600,
            "zone": "porch",
            "confidence": "likely",
            "signed": False,
        }
    )


@pytest.mark.parametrize("key", sorted(_SIGNALS))
@pytest.mark.parametrize("delivery", ["live", "live_and_retained", "retained_at_startup"])
def test_the_creating_event_is_applied_once(record, monkeypatch, key, delivery):
    """The event that creates Last Event, Motion and Occupancy moves each of
    them once, however the broker delivers it:

      * live, not retained (the canary-wap's events): the discovery callback
        hands the body over and each entity applies it after its subscribe;
      * live, and retained by the broker too: handed over the same way, and
        the broker's retained copy to the entities' fresh subscriptions is
        that same publish, recognized and not applied again (the replay gate
        passes an equal counter by design, and this body is unsigned, so
        nothing else would stop it);
      * retained when Home Assistant starts: the discovery callback sees a
        retained copy and hands nothing over; the entities' own
        subscriptions get the broker's copy, once each.
    """
    seen = _Deliveries(monkeypatch)
    topic = f"{record['prefix']}/{_PORCH}/events"
    body = _porch_event(_SIGNALS[key])
    if delivery == "retained_at_startup":
        wap = _Wap(record, monkeypatch, retained={topic: body})
    else:
        wap = _Wap(record, monkeypatch)
        wap.broker.publish(topic, body, delivery == "live_and_retained")
    other = ({"motion", "occupancy"} - {key}).pop()
    assert _value(wap.entity(key, _PORCH), "is_on") is True
    assert _value(wap.entity(other, _PORCH), "is_on") is False
    assert _attrs(wap.entity(key, _PORCH))["verified"] is False, "an unsigned event says so"
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS], "one event arms one hold window"
    assert _value(wap.entity("last_event", _PORCH), "native_value") == _SIGNALS[key]
    assert seen.recorded == [(_PORCH, _SIGNALS[key])]

    # The next event takes the ordinary path, whatever came first.
    wap.broker.publish(topic, _porch_event(_SIGNALS[key], 42), False)
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 2
    assert seen.recorded == [(_PORCH, _SIGNALS[key])] * 2


def test_a_live_event_before_the_retained_copy_does_not_reopen_the_creating_event(
    record, monkeypatch
):
    """The broker retained the creating publish, and a second event arrives
    live before the entities' batched SUBSCRIBE gets that retained copy back
    (the broker kept the first: the second was not retained). The second is
    applied as it comes, and the late copy is still the creating publish's
    echo, so nothing records or arms twice and Last Event keeps the newer
    event rather than going back to the first."""
    seen = _Deliveries(monkeypatch)
    wap = _Wap(record, monkeypatch)
    topic = f"{record['prefix']}/{_PORCH}/events"
    wap.broker.hold_retained = True
    wap.broker.publish(topic, _porch_event(_SIGNALS["motion"], 41), True)
    assert sum(sub["filter"] == topic for sub in wap.broker.held) == 3, (
        "Last Event, Motion and Occupancy wait for their retained copies"
    )
    wap.broker.publish(topic, _porch_event(_SIGNALS["occupancy"], 42), False)
    assert seen.recorded == [(_PORCH, _SIGNALS["motion"]), (_PORCH, _SIGNALS["occupancy"])]
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 2

    wap.broker.release_retained()
    assert seen.recorded == [(_PORCH, _SIGNALS["motion"]), (_PORCH, _SIGNALS["occupancy"])]
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 2, "each event arms one hold window"
    assert _value(wap.entity("last_event", _PORCH), "native_value") == _SIGNALS["occupancy"]

    # The watch closed on that copy: a later live event takes the ordinary path.
    wap.broker.publish(topic, _porch_event(_SIGNALS["motion"], 43), False)
    assert seen.recorded[-1] == (_PORCH, _SIGNALS["motion"])
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 3


@pytest.mark.parametrize("key", sorted(_SIGNALS))
def test_a_creating_event_is_not_applied_again_when_the_entity_is_added_again(
    record, monkeypatch, key
):
    """The creating body is dropped once applied: a second async_added_to_hass
    (the core re-adds an entity on a registry change) subscribes again and
    re-arms nothing, and records nothing."""
    seen = _Deliveries(monkeypatch)
    wap = _Wap(record, monkeypatch)
    wap.broker.publish(f"{record['prefix']}/{_PORCH}/events", _porch_event(_SIGNALS[key]), False)
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS]
    assert len(seen.recorded) == 1
    for entity_key in ("last_event", "motion", "occupancy"):
        run(wap.entity(entity_key, _PORCH).async_added_to_hass())
    wap.broker.pump()
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS]
    assert len(seen.recorded) == 1


# ─── Memory Critical (HA48) ────────────────────────────────────────────


def test_memory_critical_turns_on_when_the_wap_is_low_on_memory(record, monkeypatch):
    """Sweep HA48: Memory Critical reads the health's free memory the way the
    Health sensor does (memory_free, the WAP's spelling, else free_heap)."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    healthy = _one(record, "running", "health")
    sent = _one(record, "trouble", "health")
    assert healthy["memory_free"] >= CRITICAL_MEMORY_THRESHOLD_BYTES, "the healthy unit is not low"
    assert sent["memory_free"] < CRITICAL_MEMORY_THRESHOLD_BYTES, "the unit in trouble is low"
    assert "free_heap" not in healthy and "free_heap" not in sent, "the WAP spells it memory_free"
    memory = wap.entity("tamper_memory_critical")
    assert _value(memory, "is_on") is False
    wap.replay("trouble")
    assert _value(wap.entity("health_status"), "native_value") == "critical"
    assert _value(memory, "is_on") is True
    # The unit recovers: the next healthy health turns it off again.
    wap.replay("running")
    assert _value(memory, "is_on") is False


def test_memory_critical_stays_off_on_a_health_without_a_memory_reading(record, monkeypatch):
    """A health that carries neither memory_free nor free_heap as a number has
    no reading, and no reading is not a critical one: Memory Critical reads
    off, as the old default did (memory_free_bytes reads 0 there, which a
    plain swap would have shown as critical). A number in either spelling is
    still read, memory_free first."""
    wap = _Wap(record, monkeypatch).replay("connect", "running", "trouble")
    memory = wap.entity("tamper_memory_critical")
    assert _value(memory, "is_on") is True, "the unit in trouble is low"
    health = _topic(record, "health")
    low = _one(record, "trouble", "health")
    without = {k: v for k, v in low.items() if k not in ("memory_free", "free_heap")}
    for body, on in (
        (without, False),
        (low, True),
        ({**without, "memory_free": None}, False),
        ({**without, "memory_free": "junk"}, False),
        ({**without, "free_heap": "junk"}, False),
        (low, True),
        ({**without, "memory_free": "junk", "free_heap": 4096}, True),
        ({**without, "memory_free": 182000, "free_heap": 4096}, False),
        ({**without, "free_heap": 4096}, True),
        ({**without, "free_heap": 182000}, False),
    ):
        wap.broker.publish(health, json.dumps(body), True)
        assert _value(memory, "is_on") is on, body

    # A non-finite number, which json.loads takes from an untrusted broker
    # (Infinity, NaN, 1e400 for an infinity), is no reading either, and
    # raises out of no health callback (this sensor's or the Health
    # sensor's: the broker delivers to both, and the stub runs callbacks
    # unguarded, so a raise fails here).
    rest = json.dumps(without)[1:-1]
    wap.broker.publish(health, json.dumps({**without, "free_heap": 4096}), True)
    assert _value(memory, "is_on") is True
    for spelled in ('"memory_free": Infinity', '"free_heap": Infinity',
                    '"memory_free": 1e400', '"free_heap": -Infinity', '"memory_free": NaN'):
        wap.broker.publish(health, "{" + rest + ", " + spelled + "}", True)
        assert _value(memory, "is_on") is False, spelled
    # And it does not hide the other spelling's reading.
    wap.broker.publish(health, "{" + rest + ', "memory_free": Infinity, "free_heap": 4096}', True)
    assert _value(memory, "is_on") is True


# What a health's memory keys can say, as the broker hands the text over
# (json.loads takes NaN, Infinity and 1e400), and what that reads as: the
# Health sensor's state on a unit whose battery is fine, its
# memory_free_bytes attribute, and Memory Critical. The Health sensor reads
# critical below WARNING_MEMORY_THRESHOLD_BYTES, Memory Critical turns on
# below CRITICAL_MEMORY_THRESHOLD_BYTES; both read the same reading.
_MEMORY_READINGS = [
    # No reading: neither key holds a number. Not low memory, and no 0.
    ("neither key", "", "healthy", None, False),
    ("null", '"memory_free": null', "healthy", None, False),
    ("NaN", '"memory_free": NaN', "healthy", None, False),
    ("Infinity", '"memory_free": Infinity', "healthy", None, False),
    ("-Infinity", '"free_heap": -Infinity', "healthy", None, False),
    ("1e400", '"memory_free": 1e400', "healthy", None, False),
    ("empty string", '"memory_free": ""', "healthy", None, False),
    ("junk string", '"memory_free": "junk"', "healthy", None, False),
    ("true", '"memory_free": true', "healthy", None, False),
    ("false", '"free_heap": false', "healthy", None, False),
    ("list", '"memory_free": [512]', "healthy", None, False),
    ("object", '"memory_free": {"bytes": 512}', "healthy", None, False),
    # A negative count: free heap is unsigned in both firmware, so no unit
    # sends one, and it is no reading rather than low memory.
    ("-1", '"memory_free": -1', "healthy", None, False),
    ("-0.5", '"memory_free": -0.5', "healthy", None, False),
    ("string -1", '"free_heap": "-1"', "healthy", None, False),
    # A string int() reads only through its digit separator (50 bytes, which
    # would read critical): a unit publishes a number, never a spelling.
    ("string 5_0", '"memory_free": "5_0"', "healthy", None, False),
    # A finite number past 64 bits: Home Assistant's JSON writer refuses the
    # integer in a state attribute (TypeError from State.as_dict_json, which
    # fails the recorder and every websocket subscription on the entity), so
    # it is no reading rather than one the core cannot carry (sweep HA57; a
    # row here once pinned 2**64 as a reading).
    ("2**64", f'"memory_free": {2**64}', "healthy", None, False),
    ("1e300", '"memory_free": 1e300', "healthy", None, False),
    # A reading: a number in either key, memory_free first. A string of
    # digits reads as its number, as the battery's does, and the largest
    # integer the core carries reads as sent.
    ("1024", '"memory_free": 1024', "healthy", 1024, True),
    ("1023", '"memory_free": 1023', "critical", 1023, True),
    ("0", '"memory_free": 0', "critical", 0, True),
    ("2**64-1", f'"memory_free": {2**64 - 1}', "healthy", 2**64 - 1, False),
    ("string 1024", '"memory_free": "1024"', "healthy", 1024, True),
    ("string 1023", '"memory_free": "1023"', "critical", 1023, True),
    ("free_heap only, low", '"free_heap": 512', "critical", 512, True),
    ("free_heap only, plenty", '"free_heap": 182000', "healthy", 182000, False),
    ("true beside free_heap", '"memory_free": true, "free_heap": 512', "critical", 512, True),
    ("NaN beside free_heap", '"memory_free": NaN, "free_heap": 512', "critical", 512, True),
    ("-1 beside free_heap", '"memory_free": -1, "free_heap": 512', "critical", 512, True),
]


@pytest.mark.parametrize(
    ("spelled", "state", "attribute", "memory_on"),
    [case[1:] for case in _MEMORY_READINGS],
    ids=[case[0] for case in _MEMORY_READINGS],
)
def test_the_health_sensor_reads_only_a_memory_number_as_low_memory(
    record, monkeypatch, spelled, state, attribute, memory_on
):
    """Sweep HA53: the Health sensor read a health with no free-memory number
    as 0 bytes free (memory_free_bytes), so a health with neither key, or
    with null, NaN or an infinity there, read critical on a unit whose
    battery is fine, and its memory_free_bytes attribute claimed 0. It now
    reads memory_free_reading, as Memory Critical does: no reading is not
    low memory, and the attribute is None. A bool is no reading either (int()
    read `true` as 1 byte free, critical on both), nor is a negative count,
    which neither firmware can send. A real number below the threshold
    still reads critical. Each body follows one that reads the other way,
    so the state shown is the one this body set."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    health = wap.entity("health_status")
    memory = wap.entity("tamper_memory_critical")
    topic = _topic(record, "health")
    fine = _one(record, "running", "health")
    assert (fine["battery"], fine["battery_present"]) == (100, False), "the unit's battery is fine"
    rest = json.dumps({k: v for k, v in fine.items() if k not in ("memory_free", "free_heap")})[1:-1]
    before = '"memory_free": 182000' if state == "critical" else '"memory_free": 512'
    wap.broker.publish(topic, "{" + rest + ", " + before + "}", True)
    assert _value(health, "native_value") != state, before
    body = "{" + rest + (", " + spelled if spelled else "") + "}"
    wap.broker.publish(topic, body, True)
    assert _value(health, "native_value") == state, body
    attrs = _attrs(health)
    assert "memory_free_bytes" in attrs
    assert attrs["memory_free_bytes"] == attribute, body
    assert type(attrs["memory_free_bytes"]) is type(attribute), body
    assert attrs["battery_percent"] == 100
    assert _value(memory, "is_on") is memory_on, body


# What a battery key can say on a unit whose battery is present and
# discharging (the record's unit in trouble, with plenty of memory), as the
# broker hands the text over, and what the Health sensor reads from it: its
# state and its battery_percent attribute. The Health sensor reads critical
# below CRITICAL_BATTERY_THRESHOLD_PERCENT (10) and warning below
# WARNING_BATTERY_THRESHOLD_PERCENT (25). No reading is not a low battery, so
# it reads healthy, and since this unit says it has a battery its
# battery_percent is None, no charge, rather than the 100 nobody sent (sweep
# HA60; a mains unit still reads 100, test_a_present_battery_with_no_number_shows_no_charge).
_BATTERY_READINGS = [
    # No reading: not a state of charge a unit's uint8_t can hold, a string
    # int() reads only through a sign or its digit separator, or a finite
    # number past 64 bits, which Home Assistant's JSON writer refuses in an
    # attribute.
    ("null", "healthy", None),
    ("true", "healthy", None),
    ("false", "healthy", None),
    ("NaN", "healthy", None),
    ("Infinity", "healthy", None),
    ("-Infinity", "healthy", None),
    ("1e400", "healthy", None),
    ("-1", "healthy", None),
    ("-0.5", "healthy", None),
    ('"-1"', "healthy", None),
    ('"junk"', "healthy", None),
    ('"+5"', "healthy", None),
    ('"5_0"', "healthy", None),
    ("1e300", "healthy", None),
    ("18446744073709551616", "healthy", None),
    # A reading, truncated toward zero; minus zero is zero. Above 100 reads
    # as sent, up to the largest integer Home Assistant carries.
    ("-0.0", "critical", 0),
    ("0", "critical", 0),
    ("8", "critical", 8),
    ("9.9", "critical", 9),
    ("10", "warning", 10),
    ("24", "warning", 24),
    ("25", "healthy", 25),
    ("50.9", "healthy", 50),
    ('"50"', "healthy", 50),
    ("100", "healthy", 100),
    ("101", "healthy", 101),
    ("18446744073709551615", "healthy", 18446744073709551615),
]


@pytest.mark.parametrize("key", ["battery", "battery_soc"])
@pytest.mark.parametrize(
    ("spelled", "state", "attribute"),
    _BATTERY_READINGS,
    ids=[case[0] for case in _BATTERY_READINGS],
)
def test_the_health_sensor_reads_only_a_battery_number_as_a_charge(
    record, monkeypatch, key, spelled, state, attribute
):
    """Sweep HA57: battery_percent did `int(value)` under TypeError and
    ValueError only, so a planted `Infinity` or `1e400` raised OverflowError
    out of the Health callback (the stub broker runs callbacks unguarded, so
    the raise fails here), a `true` read as 1 % and critical, and `false` or
    a negative charge read as critical too. A battery key's value is now read
    as the memory keys' are, under the canary-wap's `battery` and under
    firmware/canary's `battery_soc` alike (read with battery_present true).
    A value that is no reading shows battery_percent None (HA60; it showed
    100 before). Each body follows one that reads the other way, so the
    state shown is the one this body set."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    health = wap.entity("health_status")
    memory = wap.entity("tamper_memory_critical")
    topic = _topic(record, "health")
    low = _one(record, "trouble", "health")
    assert (low["battery"], low["battery_present"], low["charge_state"]) == (8, True, "discharging")
    gone = ("battery", "battery_soc", "battery_present", "memory_free", "free_heap")
    rest = json.dumps({k: v for k, v in low.items() if k not in gone})[1:-1]
    rest += ', "battery_present": true, "memory_free": 182000'
    before = f'"{key}": 50' if state != "healthy" else f'"{key}": 8'
    wap.broker.publish(topic, "{" + rest + ", " + before + "}", True)
    assert _value(health, "native_value") != state, before
    body = "{" + rest + f', "{key}": {spelled}' + "}"
    wap.broker.publish(topic, body, True)
    assert _value(health, "native_value") == state, body
    attrs = _attrs(health)
    assert "battery_percent" in attrs
    assert attrs["battery_percent"] == attribute, body
    assert type(attrs["battery_percent"]) is type(attribute), body
    assert attrs["battery_present"] is True
    assert attrs["memory_free_bytes"] == 182000
    assert _value(memory, "is_on") is False, "the battery is not the memory"


def test_the_health_sensor_reads_battery_soc_past_a_battery_with_no_number(record, monkeypatch):
    """A `battery` that holds no number is read past like an absent one, so
    battery_soc is the reading beside it when battery_present is true, and
    is not trusted without it (an older firmware/canary sends 0 on USB)."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    health = wap.entity("health_status")
    topic = _topic(record, "health")
    low = _one(record, "trouble", "health")
    gone = ("battery", "battery_soc", "battery_present", "memory_free", "free_heap")
    rest = json.dumps({k: v for k, v in low.items() if k not in gone})[1:-1]
    rest += ', "memory_free": 182000'
    for spelled, state, attribute in (
        ('"battery_present": true, "battery": true, "battery_soc": 8', "critical", 8),
        ('"battery_present": true, "battery": Infinity, "battery_soc": 50', "healthy", 50),
        ('"battery_present": true, "battery": -1, "battery_soc": 0', "critical", 0),
        ('"battery": true, "battery_soc": 0', "healthy", 100),
        ('"battery_present": true, "battery": 50, "battery_soc": 8', "healthy", 50),
        ('"battery_present": false, "battery": 8, "battery_soc": 8', "healthy", 100),
        ('"battery_present": true, "battery": 8', "critical", 8),
        # A present battery with no number in either key: no charge (HA60).
        ('"battery_present": true, "battery": 1e400, "battery_soc": 1e400', "healthy", None),
    ):
        # Each body reads otherwise than the one before it, state or attribute.
        body = "{" + rest + ", " + spelled + "}"
        wap.broker.publish(topic, body, True)
        assert _value(health, "native_value") == state, body
        assert _attrs(health)["battery_percent"] == attribute, body


# One value per kind of charge that is no reading (sweep HA57), sent by a unit
# that says it has a battery, and that unit sending no battery key at all.
_NO_CHARGE = [
    ("bool", '"battery": true'),
    ("non-finite", '"battery": Infinity'),
    ("negative", '"battery": -1'),
    ("past 64 bits", '"battery": 18446744073709551616'),
    ("sign-spelled", '"battery": "+5"'),
    ("separator-spelled", '"battery": "5_0"'),
    ("no battery key", ""),
]


@pytest.mark.parametrize("spelled", [c[1] for c in _NO_CHARGE], ids=[c[0] for c in _NO_CHARGE])
def test_a_present_battery_with_no_number_shows_no_charge(record, monkeypatch, spelled):
    """Sweep HA60: the Health sensor's battery_percent read 100 whenever
    battery_percent() had no reading, so a unit that says it has a battery
    (battery_present true) and sends no number for its charge showed a full
    battery nobody sent. It is now None there, as memory_free_bytes is None
    for no reading (HA53). A real charge still reads as sent, the canary-wap
    on mains (100 with battery_present false, the contract
    test_health_and_egress_read_as_the_wap_sent_them pins) still reads 100,
    and so does a health that does not say whether it has a battery. The
    Health state does not move with the attribute: no reading is not a low
    battery, so every body here reads healthy, as each did before."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    health = wap.entity("health_status")
    topic = _topic(record, "health")
    mains = _one(record, "running", "health")
    assert (mains["battery"], mains["battery_present"]) == (100, False), "the running unit is on mains"
    gone = ("battery", "battery_soc", "battery_present", "memory_free", "free_heap")
    rest = json.dumps({k: v for k, v in mains.items() if k not in gone})[1:-1]
    rest += ', "memory_free": 182000'
    no_charge = "{" + rest + ', "battery_present": true' + (", " + spelled if spelled else "") + "}"
    # Each body reads otherwise than the one before it.
    for body, attribute in (
        ("{" + rest + ', "battery_present": true, "battery": 37}', 37),
        (no_charge, None),
        ("{" + rest + ', "battery_present": false, "battery": 100}', 100),
        (no_charge, None),
        ("{" + rest + "}", 100),
    ):
        wap.broker.publish(topic, body, True)
        assert _value(health, "native_value") == "healthy", body
        attrs = _attrs(health)
        assert "battery_percent" in attrs, body
        assert attrs["battery_percent"] == attribute, body
        assert type(attrs["battery_percent"]) is type(attribute), body


# A Canary whose health carries the `sd` object, as the canary base's does once
# its SD diagnostics read (firmware/canary/src/main.cpp, `doc["sd"]`), beside
# the WAP on the same broker.
_SD_DEVICE = "canary-base-01"
_SD_OBJECT = {
    "mounted": True,
    "usage_pct": 12.5,
    "writes": 4096,
    "errors": 0,
    "lifetime_kb": 524288,
    "wear_pct": 2.8,
    "replace_recommended": False,
}


def test_sd_wear_is_never_created_for_a_wap(record, monkeypatch):
    """HA46: SD Wear reads the health's `sd` object, and the WAP's health
    carries none (its card is `sd_mounted`), so a WAP gets no SD Wear: not
    through its whole record, and not when Home Assistant starts after it
    published and the broker's retained copies reach discovery first."""
    bodies = [json.loads(b) for b in _bodies(record, _topic(record, "health"))]
    assert bodies and not [b for b in bodies if "sd" in b], "the WAP's health carries no sd object"
    assert any("sd_mounted" in b for b in bodies)
    wap = _Wap(record, monkeypatch).replay()
    assert wap.entity("health_status") is not None
    assert wap.entity("sd_wear") is None
    assert "sd_wear" not in wap.keys()
    retained: dict[str, str] = {}
    for pub in _publishes(record):
        if pub["retain"] and pub["payload"]:
            retained[pub["topic"]] = pub["payload"]
        elif pub["retain"]:
            retained.pop(pub["topic"], None)
    assert _topic(record, "health") in retained
    restarted = _Wap(record, monkeypatch, retained=retained)
    assert restarted.entity("health_status") is not None
    assert restarted.entity("sd_wear") is None


def test_sd_wear_is_created_by_the_first_health_that_carries_the_sd_object(record, monkeypatch):
    """HA46, as Event ID Space Low is created (HA24): a health without the
    `sd` object, or with one that is not an object, creates no SD Wear; the
    first that carries it does, a later one included, once, and the sensor
    shows that health's wear from the broker's retained copy. Later healths
    move it, and one without the object leaves it as it was."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    topic = f"{record['prefix']}/{_SD_DEVICE}/health"
    base = {"uptime": 5, "free_heap": 182000, "firmware_version": record["firmware_version"]}
    wap.broker.publish(topic, json.dumps(base), True)
    assert wap.entity("health_status", _SD_DEVICE) is not None, "its first health creates Health"
    assert wap.entity("sd_wear", _SD_DEVICE) is None, "a health without the sd object creates none"
    for not_an_object in (None, True, 2.8, "sd", [_SD_OBJECT]):
        wap.broker.publish(topic, json.dumps({**base, "sd": not_an_object}), True)
        assert wap.entity("sd_wear", _SD_DEVICE) is None, not_an_object
    wap.broker.publish(topic, json.dumps({**base, "uptime": 65, "sd": _SD_OBJECT}), True)
    wear = wap.entity("sd_wear", _SD_DEVICE)
    assert wear is not None, "the first health that carries the sd object creates SD Wear"
    assert _value(wear, "native_value") == 2.8
    attrs = _attrs(wear)
    assert (attrs["mounted"], attrs["replace_recommended"]) == (True, False)
    assert (attrs["verified"], attrs["trust_reason"]) == (False, "unsigned")
    for uptime, body, shown in (
        (125, {"sd": {**_SD_OBJECT, "wear_pct": 3.1}}, 3.1),
        (185, {}, 3.1),
        (245, {"sd": {**_SD_OBJECT, "wear_pct": 3.4}}, 3.4),
    ):
        wap.broker.publish(topic, json.dumps({**base, "uptime": uptime, **body}), True)
        assert _value(wear, "native_value") == shown, body
    uid = f"securacv_canary_{_SD_DEVICE}_sd_wear"
    assert [e.unique_id for e in wap.broker.entities].count(uid) == 1, "created once"
    assert wap.entity("sd_wear") is None, "the WAP beside it still has none"


# ─── what the record shows is wrong today ──────────────────────────────


@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): six discovery entities (Presence, State, Confidence, Motion "
    "Score, Breathing Score, Breathing Rate) read the events topic, which is not retained",
)
def test_every_discovery_entity_finds_its_state_after_a_restart(record):
    """Home Assistant reads a state topic's last value at startup only from the
    broker's retained copy, so an entity on a topic that is not retained reads
    unknown after a Home Assistant or broker restart until its next publish."""
    retained = {pub["topic"] for pub in _publishes(record) if pub["retain"] and pub["payload"]}
    lost = sorted(
        t.split("/")[-2]
        for t, cfg in _configs(record)
        if "stat_t" in cfg and cfg["stat_t"] not in retained
    )
    assert lost == []


@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="F41's open maintainer call: the WAP's health carries no tamper_detected, so "
    "the aggregate Tamper sensor clears on the next health while the lid is open",
)
def test_tamper_stays_on_while_the_wap_says_the_lid_is_open(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running", "tamper", "trouble")
    _require(_one(record, "trouble", "health")["enclosure_open"] is True, "the lid is open")
    _require(_value(wap.entity("tamper_enclosure"), "is_on") is True, "Tamper Enclosure is on")
    assert _value(wap.entity("tamper"), "is_on") is True


_FEEDS_NOTHING = pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): created for a canary-wap, reads nothing it sends",
)

_WAP_ENTITIES = [
    # Reads peer_count / peers / sent / received / relayed; the WAP's mesh
    # body is its airtime governor's (and no firmware sends those keys).
    pytest.param("mesh_connected", "mesh", marks=_FEEDS_NOTHING, id="mesh_connected"),
    # Reads the health's `gps` object; the WAP's health has none.
    pytest.param("gps_fix", "health", marks=_FEEDS_NOTHING, id="gps_fix"),
    # Read the health's `sd` object (wear_pct, replace_recommended); the WAP's
    # health carries only sd_mounted. SD Wear is created only by a health that
    # carries the object (HA46), so a WAP gets none and its case passes.
    pytest.param("sd_wear", "health", id="sd_wear"),
    pytest.param("sd_replace", "health", marks=_FEEDS_NOTHING, id="sd_replace"),
    # Read tamper.gpio / tamper_gpio, gps_fix_lost / gps_jamming and
    # tamper.motion / unexpected_motion; the WAP's lid switch is
    # enclosure_open, and it has no GPS and no motion sensor.
    pytest.param("tamper_gpio", "health", marks=_FEEDS_NOTHING, id="tamper_gpio"),
    pytest.param("tamper_gps_jamming", "health", marks=_FEEDS_NOTHING, id="tamper_gps_jamming"),
    pytest.param("tamper_motion", "health", marks=_FEEDS_NOTHING, id="tamper_motion"),
]


def test_the_entities_a_wap_still_gets_stay_strict_xfails():
    """Each case still open is a strict xfail on AssertionError, so the day
    one starts passing (its entity no longer created for a WAP, or fed) the
    run fails until its marker is removed; only SD Wear's is resolved."""
    open_cases = {
        param.values[0]
        for param in _WAP_ENTITIES
        if [m for m in param.marks if m.name == "xfail"]
    }
    assert open_cases == {
        "mesh_connected", "gps_fix", "sd_replace", "tamper_gpio", "tamper_gps_jamming", "tamper_motion",
    }
    for param in _WAP_ENTITIES:
        for mark in param.marks:
            assert mark.name == "xfail", param.id
            assert mark.kwargs["strict"] is True, param.id
            assert mark.kwargs["raises"] is AssertionError, param.id


@pytest.mark.parametrize(("key", "topic"), _WAP_ENTITIES)
def test_an_entity_a_wap_gets_moves_with_what_it_sends(record, monkeypatch, key, topic):
    """The entity is not created for a WAP, or its state moves when the WAP's
    publish on its topic changes (the healthy unit against the one in trouble)."""
    _require(_one(record, "running", topic) != _one(record, "trouble", topic), "the bodies differ")
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    ent = wap.entity(key)
    if ent is None:
        return
    before = _snapshot(ent)
    wap.replay("trouble")
    assert _snapshot(ent) != before


@pytest.mark.parametrize("key", ["motion", "occupancy"])
@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): the HomeKit projection maps no event type a canary-wap "
    "sends (its presence vocabulary: active, subtle, quiet, together, empty)",
)
def test_a_wap_presence_event_asserts_motion_and_occupancy(record, monkeypatch, key):
    """The entity is not created for a WAP, or a presence event asserts it."""
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    ent = wap.entity(key)
    if ent is None:
        return
    (sent,) = [
        pub["payload"]
        for pub in _publishes(record, "running")
        if pub["topic"] == _topic(record, "events")
    ]
    _require(json.loads(sent)["state"] == "active", "the running phase's event is active")
    # Delivered again: the first one created the entity and was applied by it
    # too (HA44), and a WAP word asserts nothing on either delivery.
    wap.broker.publish(_topic(record, "events"), sent, False)
    assert _value(ent, "is_on") is True
