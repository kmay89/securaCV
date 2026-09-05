"""Options flow — pin / rotate / unpin (roadmap #54).

The "Configure" button's flow shipped with no tests. These drive
SecuraCVOptionsFlow against a real TrustStore (on the in-memory Store
stub) and prove each action by its effect on the store — and, for
rotation, on a real Ed25519 signature: the form result is only half the
contract, the other half is whether the next publish verifies.

Runs against the stub OptionsFlow base from conftest — no HA core needed.
"""

from __future__ import annotations

import base64
from types import SimpleNamespace

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from . import conftest  # noqa: F401  (installs/augments ha stubs at import time)
from .conftest import run

from homeassistant.core import HomeAssistant

from ..config_flow import (
    CONF_PIN_DEVICE_ID,
    CONF_PIN_PUBKEY_HEX,
    PIN_ACTION_PIN,
    PIN_ACTION_ROTATE,
    PIN_ACTION_UNPIN,
    SecuraCVConfigFlow,
    SecuraCVOptionsFlow,
)
from ..const import DOMAIN
from ..device_trust import (
    PIN_SOURCE_MANUAL,
    PIN_SOURCE_ROTATION,
    PIN_SOURCE_TOFU,
    TrustStore,
    fingerprint_from_pubkey_hex,
)
from ..signature import build_chain_canonical, verify_chain

DEVICE = "canary-a1b2"
KEY_A = "11" * 32
KEY_B = "22" * 32


def _flow(*, with_store: bool = True, notified=None):
    """An options flow over a fresh entry slice. Returns (flow, store, entry_data)."""
    hass = HomeAssistant()
    entry = SimpleNamespace(entry_id="e1", data={}, options={}, unique_id=None)
    entry_data: dict = {"mismatch_notified": set(notified or ())}
    store = None
    if with_store:
        store = TrustStore(hass, entry_id="e1")
        run(store.async_load())
        entry_data["trust_store"] = store
    hass.data = {DOMAIN: {"e1": entry_data}}
    flow = SecuraCVOptionsFlow(entry)
    flow.hass = hass
    return flow, store, entry_data


def _keypair(seed: bytes):
    priv = Ed25519PrivateKey.from_private_bytes(seed)
    return priv, priv.public_key().public_bytes_raw().hex()


def _signed_chain(priv, pubkey_hex: str, length: int) -> dict:
    """A chain publish signed by `priv`, exactly as the firmware envelopes it."""
    latest_hash = "ab" * 32
    sig = priv.sign(build_chain_canonical(DEVICE, length, latest_hash))
    return {
        "v": 1,
        "length": length,
        "latest_hash": latest_hash,
        "sig": base64.urlsafe_b64encode(sig).rstrip(b"=").decode("ascii"),
        "fp": fingerprint_from_pubkey_hex(pubkey_hex),
        "alg": "ed25519",
    }


# ---------------------------------------------------------------------------
# Entry point + menu
# ---------------------------------------------------------------------------


def test_configure_button_opens_the_trust_menu():
    entry = SimpleNamespace(entry_id="e1", data={}, options={})
    flow = SecuraCVConfigFlow.async_get_options_flow(entry)
    assert isinstance(flow, SecuraCVOptionsFlow)

    result = run(flow.async_step_init())
    assert result["type"] == "menu"
    assert result["step_id"] == "init"
    assert result["menu_options"] == [PIN_ACTION_PIN, PIN_ACTION_ROTATE, PIN_ACTION_UNPIN]


# ---------------------------------------------------------------------------
# pin
# ---------------------------------------------------------------------------


def test_pin_shows_the_form_then_pins_manually():
    flow, store, _ = _flow()

    result = run(flow.async_step_pin())
    assert result["type"] == "form"
    assert result["step_id"] == "pin"
    assert result["errors"] == {}
    assert {m.key for m in result["data_schema"].schema} == {
        CONF_PIN_DEVICE_ID,
        CONF_PIN_PUBKEY_HEX,
    }

    # Pasted from a case-preserving source with stray whitespace: accepted,
    # normalized to lowercase, recorded as a MANUAL pin.
    result = run(
        flow.async_step_pin(
            {CONF_PIN_DEVICE_ID: f" {DEVICE} ", CONF_PIN_PUBKEY_HEX: f"  {KEY_A.upper()} "}
        )
    )
    assert result["type"] == "create_entry"
    pin = store.get(DEVICE)
    assert pin is not None
    assert pin.pubkey_hex == KEY_A
    assert pin.pin_source == PIN_SOURCE_MANUAL
    assert pin.fingerprint_hex == fingerprint_from_pubkey_hex(KEY_A)
    assert pin.previous == []


def test_manual_pin_overrides_tofu_and_keeps_the_audit_trail():
    flow, store, _ = _flow()
    run(store.async_tofu_pin_if_unknown(DEVICE, KEY_A))

    result = run(flow.async_step_pin({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_B}))
    assert result["type"] == "create_entry"
    pin = store.get(DEVICE)
    assert pin.pubkey_hex == KEY_B
    assert pin.pin_source == PIN_SOURCE_MANUAL
    # The TOFU identity is retired into the audit trail, not erased.
    assert [p["pubkey_hex"] for p in pin.previous] == [KEY_A]
    assert pin.previous[0]["fp"] == fingerprint_from_pubkey_hex(KEY_A)


def test_pin_rejects_bad_input_and_pins_nothing():
    flow, store, _ = _flow()

    result = run(flow.async_step_pin({CONF_PIN_DEVICE_ID: "   ", CONF_PIN_PUBKEY_HEX: KEY_A}))
    assert result["type"] == "form"
    assert result["errors"] == {"device_id": "invalid_device_id"}

    for bad in ("", KEY_A[:-2], "zz" * 32, KEY_A + "00"):
        result = run(flow.async_step_pin({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: bad}))
        assert result["type"] == "form", bad
        assert result["errors"] == {"pubkey_hex": "invalid_pubkey_hex"}, bad

    assert store.all_devices() == {}


def test_pin_clears_the_notification_dedup_like_rotate_and_unpin():
    """Pin is a key-CHANGING path too — its own docstring says it replaces a
    TOFU pin with a manual one — and it is the action an alarmed operator
    reaches for after a mismatch notification. It alone left the dedup key
    behind, so the next genuine mismatch carrying that same fp was dropped
    in silence until the entry reloaded."""
    fp = fingerprint_from_pubkey_hex(KEY_B)
    flow, store, entry_data = _flow(notified={(DEVICE, fp), ("other-dev", "ff" * 8)})
    run(store.async_tofu_pin_if_unknown(DEVICE, KEY_A))

    result = run(flow.async_step_pin({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_B}))
    assert result["type"] == "create_entry"
    assert entry_data["mismatch_notified"] == {("other-dev", "ff" * 8)}

    # First-time pins clear too — same path, no pre-existing pin.
    flow, store, entry_data = _flow(notified={("fresh-dev", "aa" * 8)})
    run(flow.async_step_pin({CONF_PIN_DEVICE_ID: "fresh-dev", CONF_PIN_PUBKEY_HEX: KEY_A}))
    assert entry_data["mismatch_notified"] == set()


def test_pin_without_a_trust_store_reports_it_instead_of_crashing():
    flow, _, _ = _flow(with_store=False)
    result = run(flow.async_step_pin({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_A}))
    assert result["type"] == "form"
    assert result["errors"] == {"base": "trust_store_unavailable"}


# ---------------------------------------------------------------------------
# rotate
# ---------------------------------------------------------------------------


def test_rotate_needs_an_existing_pin():
    flow, store, _ = _flow()
    result = run(flow.async_step_rotate({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: KEY_B}))
    assert result["type"] == "form"
    assert result["step_id"] == "rotate"
    assert result["errors"] == {"device_id": "device_not_pinned"}
    assert not store.is_pinned(DEVICE)

    # Input validation runs before the store is consulted.
    result = run(flow.async_step_rotate({CONF_PIN_DEVICE_ID: "", CONF_PIN_PUBKEY_HEX: KEY_B}))
    assert result["errors"] == {"device_id": "invalid_device_id"}
    result = run(flow.async_step_rotate({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: "nope"}))
    assert result["errors"] == {"pubkey_hex": "invalid_pubkey_hex"}


def test_rotate_replaces_the_key_and_a_real_signature_follows():
    """The whole point of rotation: before it, a publish signed by the new
    key is a mismatch against the old pin; after it, the same publish
    verifies. The stuck mismatch notification for THIS device is cleared;
    another device's is not."""
    priv_a, pub_a = _keypair(b"\x41" * 32)
    priv_b, pub_b = _keypair(b"\x42" * 32)
    fp_b = fingerprint_from_pubkey_hex(pub_b)
    flow, store, entry_data = _flow(notified={(DEVICE, fp_b), ("other-dev", "ff" * 8)})
    run(store.async_tofu_pin_if_unknown(DEVICE, pub_a))

    publish = _signed_chain(priv_b, pub_b, length=5)
    before = verify_chain(store, DEVICE, publish)
    assert not before.trusted
    assert before.reason == "mismatch"
    assert before.pinned_fingerprint == fingerprint_from_pubkey_hex(pub_a)
    assert before.received_fingerprint == fp_b

    result = run(flow.async_step_rotate({CONF_PIN_DEVICE_ID: DEVICE, CONF_PIN_PUBKEY_HEX: pub_b}))
    assert result["type"] == "create_entry"

    pin = store.get(DEVICE)
    assert pin.pubkey_hex == pub_b
    assert pin.pin_source == PIN_SOURCE_ROTATION
    assert [p["pubkey_hex"] for p in pin.previous] == [pub_a]

    after = verify_chain(store, DEVICE, publish)
    assert after.trusted and after.reason == "ok"
    assert after.pinned_fingerprint == after.received_fingerprint == fp_b
    # ...and the OLD key no longer verifies: rotation is a replacement.
    assert verify_chain(store, DEVICE, _signed_chain(priv_a, pub_a, 6)).reason == "mismatch"

    assert entry_data["mismatch_notified"] == {("other-dev", "ff" * 8)}


# ---------------------------------------------------------------------------
# unpin
# ---------------------------------------------------------------------------


def test_unpin_drops_the_pin_and_clears_the_notification_dedup():
    flow, store, entry_data = _flow(notified={(DEVICE, "aa" * 8), ("other-dev", "bb" * 8)})
    run(store.async_pin(DEVICE, KEY_A))
    run(store.async_pin("other-dev", KEY_B))

    result = run(flow.async_step_unpin({CONF_PIN_DEVICE_ID: f" {DEVICE} "}))
    assert result["type"] == "create_entry"
    assert not store.is_pinned(DEVICE)
    assert store.is_pinned("other-dev"), "unpin is per device"
    # Without this a spoofed fp seen before the unpin would be suppressed
    # forever after a re-pin — the dedup key must go with the pin.
    assert entry_data["mismatch_notified"] == {("other-dev", "bb" * 8)}

    # A second unpin of the same device is an error, not a silent no-op.
    result = run(flow.async_step_unpin({CONF_PIN_DEVICE_ID: DEVICE}))
    assert result["type"] == "form"
    assert result["step_id"] == "unpin"
    assert result["errors"] == {"device_id": "device_not_pinned"}

    result = run(flow.async_step_unpin({CONF_PIN_DEVICE_ID: ""}))
    assert result["errors"] == {"device_id": "invalid_device_id"}


def test_unpin_then_the_next_publish_tofu_pins_fresh():
    """The documented reason to unpin: the operator cannot recover the old
    keypair and wants HA to start over with whatever key shows up next."""
    flow, store, _ = _flow()
    run(store.async_pin(DEVICE, KEY_A))
    assert run(flow.async_step_unpin({CONF_PIN_DEVICE_ID: DEVICE}))["type"] == "create_entry"

    fresh = run(store.async_tofu_pin_if_unknown(DEVICE, KEY_B))
    assert fresh is not None
    assert fresh.pubkey_hex == KEY_B
    assert fresh.pin_source == PIN_SOURCE_TOFU
    assert fresh.previous == [], "an unpinned device has no audit trail to inherit"


def test_unpin_without_a_trust_store_reports_it():
    flow, _, _ = _flow(with_store=False)
    result = run(flow.async_step_unpin({CONF_PIN_DEVICE_ID: DEVICE}))
    assert result["errors"] == {"base": "trust_store_unavailable"}
