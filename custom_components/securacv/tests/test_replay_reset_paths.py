"""Replay high-water marks reset on every path that changes a device's key.

The replay gate compares the signed counters (event_id, seq, chain length,
totals) against the last value seen for a device_id. Those counters belong
to the KEY: a factory-reset or replaced Canary starts them over. So every
path that changes which key a device_id answers to must forget the marks —
TOFU first sight (the health hook) and the operator's pin / rotate / unpin
(the options flow) — or the replacement device's first publishes all read
as replays of the old one until its counters climb past the old marks (or
Home Assistant restarts). A review of the first replay-gate change found
that none of these paths did; this pins that they all do now.
"""

from __future__ import annotations

import json
from types import SimpleNamespace

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run
from .test_options_flow import _flow
from .test_tofu_health_hook import _Hass

from ..config_flow import CONF_PIN_DEVICE_ID, CONF_PIN_PUBKEY_HEX
from ..const import DOMAIN
from .. import _async_health_for_tofu
from ..device_trust import TrustStore

ENTRY = SimpleNamespace(entry_id="e1")
DEVICE = "canary-a1b2"
OTHER = "canary-zz99"
KEY_A = "11" * 32
KEY_B = "22" * 32
STALE_MARKS = {"verify_event": 40, "verify_chain": 12}


def _health_msg(device_id: str, pubkey_hex: str):
    return SimpleNamespace(
        topic=f"securacv/{device_id}/health",
        payload=json.dumps({"public_key": pubkey_hex}).encode(),
    )


def test_tofu_first_sight_forgets_the_old_marks_for_that_device_only():
    hass = _Hass()
    store = TrustStore(hass, entry_id="e1")
    run(store.async_load())
    hass.data = {
        DOMAIN: {
            "e1": {
                "trust_store": store,
                "verify": {},
                "replay": {DEVICE: dict(STALE_MARKS), OTHER: dict(STALE_MARKS)},
                "mismatch_notified": set(),
            }
        }
    }
    hook = _async_health_for_tofu(hass, ENTRY)

    hook(_health_msg(DEVICE, KEY_A))
    assert store.is_pinned(DEVICE)
    replay = hass.data[DOMAIN]["e1"]["replay"]
    assert DEVICE not in replay, "a first-sight pin is a new identity; its marks start over"
    assert replay[OTHER] == STALE_MARKS, "another device's marks are untouched"


def test_a_health_publish_for_an_already_pinned_device_keeps_the_marks():
    hass = _Hass()
    store = TrustStore(hass, entry_id="e1")
    run(store.async_load())
    run(store.async_tofu_pin_if_unknown(DEVICE, KEY_A))
    hass.data = {
        DOMAIN: {
            "e1": {
                "trust_store": store,
                "verify": {},
                "replay": {DEVICE: dict(STALE_MARKS)},
                "mismatch_notified": set(),
            }
        }
    }
    hook = _async_health_for_tofu(hass, ENTRY)

    # Same key, and even a different key: no pin happens (TOFU keeps the
    # first identity), so the marks — which guard against exactly the replay
    # a second publisher would attempt — stay put.
    hook(_health_msg(DEVICE, KEY_A))
    hook(_health_msg(DEVICE, KEY_B))
    assert hass.data[DOMAIN]["e1"]["replay"][DEVICE] == STALE_MARKS


def test_manual_pin_rotate_and_unpin_each_forget_the_marks():
    flow, store, entry_data = _flow()
    entry_data["replay"] = {DEVICE: dict(STALE_MARKS), OTHER: dict(STALE_MARKS)}

    result = run(flow.async_step_pin({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_A}))
    assert result["type"] == "create_entry"
    assert DEVICE not in entry_data["replay"]
    assert entry_data["replay"][OTHER] == STALE_MARKS

    entry_data["replay"][DEVICE] = dict(STALE_MARKS)
    result = run(flow.async_step_rotate({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_B}))
    assert result["type"] == "create_entry"
    assert DEVICE not in entry_data["replay"]

    entry_data["replay"][DEVICE] = dict(STALE_MARKS)
    result = run(flow.async_step_unpin({CONF_PIN_DEVICE_ID: DEVICE}))
    assert result["type"] == "create_entry"
    assert DEVICE not in entry_data["replay"]
    assert entry_data["replay"][OTHER] == STALE_MARKS


def test_a_refused_options_step_leaves_the_marks_alone():
    flow, store, entry_data = _flow()
    entry_data["replay"] = {DEVICE: dict(STALE_MARKS)}

    # Rotating a device that was never pinned is refused; nothing changed.
    result = run(flow.async_step_rotate({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_B}))
    assert result["type"] == "form"
    assert result["errors"] == {"device_id": "device_not_pinned"}
    assert entry_data["replay"][DEVICE] == STALE_MARKS
