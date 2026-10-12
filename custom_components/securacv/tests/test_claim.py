"""The retained claim (contract C3): topic, payload, publish, withdraw, refresh.

The payload carries a random nonce and a bucket start, never the entry id;
it is retained at QoS 1, republished on every load and hourly, and
withdrawn with an empty retained payload only from async_remove_entry.
"""

from __future__ import annotations

import json
import sys

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, published, run

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import claim  # noqa: E402
from ..const import (  # noqa: E402
    CONF_CLAIM_NONCE,
    CONF_KERNEL_DEVICE_ID,
    CONF_MQTT_TOPIC_PREFIX,
)

DEVICE_ID = "pwk_1a2b3c4d"


def test_nonce_is_sixteen_hex_and_fresh_each_time():
    a, b = claim.new_claim_nonce(), claim.new_claim_nonce()
    assert len(a) == 16 and int(a, 16) >= 0
    assert a != b


def test_topic_and_payload_shape():
    assert claim.claim_topic("witness", DEVICE_ID) == "witness/claim/pwk_1a2b3c4d"
    text = claim.claim_payload("0123456789abcdef", now=1760140875.5)
    assert text == json.dumps(
        {"v": 1, "claimed": True, "nonce": "0123456789abcdef", "claimed_bucket_start": 1760140800},
        separators=(",", ":"),
    )
    doc = json.loads(text)
    assert doc["claimed_bucket_start"] % 600 == 0, "coarse time: a bucket start, never the instant"
    assert "entry_id" not in text


def test_publish_is_retained_qos1_and_needs_the_kernel_identity():
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(data={CONF_KERNEL_DEVICE_ID: DEVICE_ID, CONF_CLAIM_NONCE: "feedfacefeedface"})
    assert run(claim.async_publish_claim(hass, entry)) is True
    (msg,) = published(hass)
    assert msg["topic"] == "witness/claim/pwk_1a2b3c4d"
    assert msg["qos"] == 1 and msg["retain"] is True
    body = json.loads(msg["payload"])
    assert body["nonce"] == "feedfacefeedface" and body["claimed"] is True

    # A custom bridge prefix moves the topic.
    entry.data[CONF_MQTT_TOPIC_PREFIX] = "pwk"
    run(claim.async_publish_claim(hass, entry))
    assert published(hass)[-1]["topic"] == "pwk/claim/pwk_1a2b3c4d"

    # No device id (identity never read) or no nonce: nothing is claimed.
    for data in ({CONF_CLAIM_NONCE: "feedfacefeedface"}, {CONF_KERNEL_DEVICE_ID: DEVICE_ID}, {}):
        before = len(published(hass))
        assert run(claim.async_publish_claim(hass, make_entry(data=data))) is False
        assert len(published(hass)) == before


def test_withdraw_publishes_an_empty_retained_payload():
    hass = HomeAssistant()
    hass.data = {}
    run(claim.async_withdraw_claim(hass, "witness", DEVICE_ID))
    (msg,) = published(hass)
    assert msg == {"topic": "witness/claim/pwk_1a2b3c4d", "payload": b"", "qos": 1, "retain": True}


def test_hourly_republish_is_scheduled_and_cancellable(monkeypatch):
    scheduled: list = []
    event_mod = sys.modules["homeassistant.helpers.event"]

    def _track(hass, action, interval, **kw):
        scheduled.append((action, interval))
        return lambda: scheduled.append("canceled")

    monkeypatch.setattr(event_mod, "async_track_time_interval", _track)
    monkeypatch.setattr(claim, "async_track_time_interval", _track)
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(data={CONF_KERNEL_DEVICE_ID: DEVICE_ID, CONF_CLAIM_NONCE: "feedfacefeedface"})
    cancel = claim.async_schedule_claim_republish(hass, entry)
    action, interval = scheduled[0]
    assert interval == claim.CLAIM_REPUBLISH_INTERVAL
    assert interval.total_seconds() == 3600
    run(action(None))
    assert published(hass)[-1]["topic"] == "witness/claim/pwk_1a2b3c4d"
    cancel()
    assert scheduled[-1] == "canceled"


def test_claim_document_for_diagnostics():
    entry = make_entry(data={CONF_KERNEL_DEVICE_ID: DEVICE_ID, CONF_CLAIM_NONCE: "feedfacefeedface"})
    assert claim.claim_document(entry) == {
        "topic": "witness/claim/pwk_1a2b3c4d",
        "nonce": "feedfacefeedface",
    }
    assert claim.claim_document(make_entry(data={})) is None
