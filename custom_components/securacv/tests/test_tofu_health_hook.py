"""The TOFU health hook in __init__.py (roadmap #54).

``_async_health_for_tofu`` is the one place a device's identity enters the
trust store without an operator: the first health publish that carries a
well-formed ``public_key`` pins it. These tests fix the contract —
first sight pins; a later publish with a DIFFERENT key does not re-pin
(the store keeps the first identity and the verifier flags the new key as
a mismatch, once, via the notification dedup); malformed keys, topics and
device ids never reach the store.
"""

from __future__ import annotations

import json
from types import SimpleNamespace

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

from homeassistant.core import HomeAssistant

from .. import _async_health_for_tofu, async_record_verify
from ..const import DOMAIN
from ..device_trust import PIN_SOURCE_TOFU, TrustStore, fingerprint_from_pubkey_hex
from ..signature import verify_chain

ENTRY = SimpleNamespace(entry_id="e1")
KEY_A = "11" * 32
KEY_B = "22" * 32


class _Hass(HomeAssistant):
    """Runs what the callback schedules, instead of dropping it, so the
    pin (and the notification) the hook fires off are observable."""

    def __init__(self) -> None:
        super().__init__()
        self.scheduled = 0
        self.notifications: list[dict] = []

    def async_create_task(self, coro):
        self.scheduled += 1
        return run(coro)

    @property
    def services(self):
        outer = self

        class _Services:
            async def async_call(self, domain, service, data=None, **kwargs):
                outer.notifications.append({"domain": domain, "service": service, **(data or {})})

        return _Services()


def _setup():
    hass = _Hass()
    store = TrustStore(hass, entry_id="e1")
    run(store.async_load())
    hass.data = {
        DOMAIN: {
            "e1": {
                "trust_store": store,
                "verify": {},
                "replay": {},
                "mismatch_notified": set(),
            }
        }
    }
    return hass, store, _async_health_for_tofu(hass, ENTRY)


def _health(payload, device_id: str = "canary01", topic: str | None = None):
    body = payload if isinstance(payload, (str, bytes)) else json.dumps(payload)
    return SimpleNamespace(topic=topic or f"securacv/{device_id}/health", payload=body)


def test_first_health_publish_pins_the_key():
    hass, store, cb = _setup()
    assert cb(_health({"public_key": KEY_A, "free_heap": 168224})) is None

    assert hass.scheduled == 1
    pin = store.get("canary01")
    assert pin is not None
    assert pin.pubkey_hex == KEY_A
    assert pin.pin_source == PIN_SOURCE_TOFU
    assert pin.fingerprint_hex == fingerprint_from_pubkey_hex(KEY_A)

    # Re-publishing the SAME key is idempotent: nothing more is scheduled.
    cb(_health({"public_key": KEY_A}))
    assert hass.scheduled == 1


def test_a_changed_key_is_not_re_pinned_and_verifies_as_a_mismatch():
    """TOFU is sticky. A second identity for a pinned device_id is never
    adopted by the hook; only the options flow (rotate) can replace a pin."""
    hass, store, cb = _setup()
    cb(_health({"public_key": KEY_A}))
    cb(_health({"public_key": KEY_B}))

    assert hass.scheduled == 1, "the changed key must not schedule a pin"
    assert store.get("canary01").pubkey_hex == KEY_A
    assert store.get("canary01").previous == []

    # The verifier then rejects a publish under the new identity...
    fp_b = fingerprint_from_pubkey_hex(KEY_B)
    verdict = verify_chain(
        store,
        "canary01",
        {"v": 1, "length": 1, "latest_hash": "aa" * 32, "sig": "AA", "fp": fp_b, "alg": "ed25519"},
    )
    assert not verdict.trusted
    assert verdict.reason == "mismatch"
    assert verdict.pinned_fingerprint == fingerprint_from_pubkey_hex(KEY_A)
    assert verdict.received_fingerprint == fp_b

    # ...and recording it warns loudly exactly once per (device, fp).
    async_record_verify(hass, ENTRY, "canary01", verdict)
    async_record_verify(hass, ENTRY, "canary01", verdict)
    entry_data = hass.data[DOMAIN]["e1"]
    assert entry_data["verify"]["canary01"]["reason"] == "mismatch"
    assert entry_data["mismatch_notified"] == {("canary01", fp_b)}
    assert len(hass.notifications) == 1
    note = hass.notifications[0]
    assert (note["domain"], note["service"]) == ("persistent_notification", "create")
    assert note["notification_id"] == "securacv_mismatch_canary01"
    assert fp_b in note["message"]


def test_malformed_keys_never_reach_the_store():
    hass, store, cb = _setup()
    for payload in (
        {},                                  # no key at all
        {"public_key": None},
        {"public_key": 123},                 # not a string
        {"public_key": KEY_A[:-2]},          # 63 chars
        {"public_key": KEY_A + "00"},        # 66 chars
        {"public_key": "zz" * 32},           # 64 chars, not hex
        "not json",
        '["' + KEY_A + '"]',                 # JSON, not an object
        b"\xff\xfe{",                        # undecodable bytes
    ):
        cb(_health(payload))
    assert hass.scheduled == 0
    assert store.all_devices() == {}


def test_bad_topics_device_ids_and_unknown_entries_are_ignored():
    hass, store, cb = _setup()
    good = {"public_key": KEY_A}

    cb(_health(good, topic="securacv/health"))            # no device segment
    cb(_health(good, topic="securacv/../health"))          # hostile device id
    cb(_health(good, topic="securacv/canary one/health"))  # not a Canary segment
    assert hass.scheduled == 0
    assert store.all_devices() == {}

    # An entry whose slice is gone (unloaded mid-flight) is a no-op, not a crash.
    orphan = _async_health_for_tofu(hass, SimpleNamespace(entry_id="gone"))
    assert orphan(_health(good)) is None
    assert store.all_devices() == {}

    # The same payload on a well-formed topic pins — the gate, not the key, was the problem.
    cb(_health(good))
    assert store.get("canary01").pubkey_hex == KEY_A
