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
    """

    def __init__(self, hass: _Hass) -> None:
        self.hass = hass
        self.retained: dict[str, str] = {}
        self.subscriptions: list[dict[str, Any]] = []
        self.deliveries: deque[tuple[dict[str, Any], str, str, bool]] = deque()
        self.adding: list[Any] = []
        self.entities: list[Any] = []

    async def async_subscribe(self, hass, topic, msg_callback, qos=0, encoding="utf-8"):
        sub = {"filter": topic, "callback": msg_callback, "active": True}
        self.subscriptions.append(sub)
        for retained_topic, payload in self.retained.items():
            if _matches(topic, retained_topic):
                self.deliveries.append((sub, retained_topic, payload, True))

        def _unsubscribe() -> None:
            sub["active"] = False

        return _unsubscribe

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
    """The integration, set up in MQTT mode, with a canary-wap's record replayed."""

    def __init__(self, record: dict[str, Any], monkeypatch) -> None:
        self.record = record
        self.hass = _Hass()
        self.hass.data = {}
        self.broker = _Broker(self.hass)
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

    def entity(self, key: str) -> Any:
        uid = f"securacv_canary_{self.record['device_id']}_{key}"
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
    # sensor itself misses the first event: see the xfail below).
    event = _one(record, "running", "events")
    verdict = verify_event(wap.store, record["device_id"], event)
    assert verdict.trusted and verdict.reason == "ok", verdict


def test_last_event_reads_the_wap_event_body(record, monkeypatch):
    """Each field of the events body Last Event reads, and the modality it
    derives from the status's device_type. The event is delivered again once
    the entity exists: the first one created it (see the xfail below)."""
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
        "the connect burst's status and the last will carry no rssi: after the "
        "will the entity is unavailable, so the default is not shown; after a "
        "connect Signal Strength reads 0 dBm until the loop's next status "
        "(handed up (#1836))"
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


# ─── what the record shows is wrong today ──────────────────────────────


@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): the first events publish creates Last Event and is not "
    "shown by it; events are not retained, and the entity subscribes after the delivery",
)
def test_the_first_event_is_shown_by_the_entity_it_creates(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    sent = _one(record, "running", "events")
    last = wap.entity("last_event")
    _require(last is not None, "the first events publish creates Last Event")
    assert _value(last, "native_value") == sent["state"]
    assert _attrs(last)["verified"] is True


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


@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): the device's sw_version comes from the status topic, "
    "and a canary-wap's status carries no firmware_version (its health does)",
)
def test_the_device_shows_the_firmware_the_wap_reports(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running")
    (device,) = device_registry(wap.hass).devices
    assert device.sw_version == record["firmware_version"]


@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): Memory Critical reads the health's free_heap (default "
    "100000); a canary-wap's health carries memory_free",
)
def test_memory_critical_turns_on_when_the_wap_is_low_on_memory(record, monkeypatch):
    wap = _Wap(record, monkeypatch).replay("connect", "running", "trouble")
    sent = _one(record, "trouble", "health")
    _require(sent["memory_free"] < CRITICAL_MEMORY_THRESHOLD_BYTES, "the unit in trouble is low")
    _require("free_heap" not in sent, "the WAP's health spells it memory_free")
    _require(
        _value(wap.entity("health_status"), "native_value") == "critical",
        "the Health sensor reads it critical",
    )
    assert _value(wap.entity("tamper_memory_critical"), "is_on") is True


@pytest.mark.parametrize(
    ("key", "topic"),
    [
        # Reads peer_count / peers / sent / received / relayed; the WAP's mesh
        # body is its airtime governor's (and no firmware sends those keys).
        ("mesh_connected", "mesh"),
        # Reads the health's `gps` object; the WAP's health has none.
        ("gps_fix", "health"),
        # Read the health's `sd` object (wear_pct, replace_recommended); the
        # WAP's health carries only sd_mounted.
        ("sd_wear", "health"),
        ("sd_replace", "health"),
        # Read tamper.gpio / tamper_gpio, gps_fix_lost / gps_jamming and
        # tamper.motion / unexpected_motion; the WAP's lid switch is
        # enclosure_open, and it has no GPS and no motion sensor.
        ("tamper_gpio", "health"),
        ("tamper_gps_jamming", "health"),
        ("tamper_motion", "health"),
    ],
)
@pytest.mark.xfail(
    strict=True,
    raises=AssertionError,
    reason="handed up (#1836): created for a canary-wap, reads nothing it sends",
)
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
    # Delivered again now that the entity exists (the first one created it).
    wap.broker.publish(_topic(record, "events"), sent, False)
    assert _value(ent, "is_on") is True
