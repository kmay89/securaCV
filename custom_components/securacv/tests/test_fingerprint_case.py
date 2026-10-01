"""A canary-wap's signed publishes verify: fingerprints compare ignoring case.

A canary-wap on firmware 2.4.15 or older spells hex in capitals: `hex_to_str`
in canary_wap.ino writes "0123456789ABCDEF", and it filled the `fp` of every
signed chain / counts / events envelope (via device_signature::init) and the
`public_key` of the health publish (via csi_mqtt::init). HA derives the pinned
fingerprint in lowercase (`fingerprint_from_pubkey_hex`), and the verifier
compared the two strings exactly, so after TOFU every signed publish from a
canary-wap read `mismatch` ("Fingerprint changed without rotation") and
raised a key-mismatch notification, even though its signature was good.

The WAP_* bodies below are what such a canary-wap publishes, byte for byte,
for the repo's Ed25519 test key (seed 0x42 * 32, test_signature.py
`_make_keypair`). They were produced on the host by compiling the firmware's
own code: `sha256_domain`, `compute_fingerprint`, `hex_to_str` and
`generate_device_id` lifted verbatim from canary_wap.ino (S3 prefix), with
firmware/common/identity/device_signature.cpp for init, the canonicals and
b64url, firmware/common/csi/src/csi_event_wire.h for the events body, and
csi_mqtt.cpp's health / chain / counts formats; signed with OpenSSL's
Ed25519, which is deterministic, so the Python signer below reproduces
every `sig`.

A later canary-wap spells those two strings in lowercase (sweep HA20:
canary_wap.ino's `mqtt_identity.h`), and so does every other build. Its
bodies are the same bytes with `fp` and `public_key` lowercased, which is
the `lowercase` spelling below; the WAP's own host test
(tests_host/test_mqtt_identity.cpp) builds the lowercase events body through
the firmware's code and compares it with this one. Capital-spelling units
stay deployed, so both spellings keep running.

On the exact-compare code every WAP-spelling test here fails (the end-to-end
one with `mismatch` on the first signed topic). The lowercase runs, the
fixture check and the firmware cross-check pass on both, and are the
regression guard for every build that spells hex in lowercase.
"""

from __future__ import annotations

import base64
import json
import logging
import re
from pathlib import Path
from types import SimpleNamespace

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from .test_mqtt_payload_hardening import HomeAssistant  # noqa: F401  (platform stubs)
from .conftest import run

from .. import _async_health_for_tofu, async_record_verify
from .. import sensor as sensor_platform
from ..const import DOMAIN
from ..device_trust import (
    PIN_SOURCE_MANUAL,
    PIN_SOURCE_TOFU,
    TrustStore,
    TrustVerdict,
    fingerprint_from_pubkey_hex,
)
from ..signature import build_chain_canonical, verify_chain, verify_counts, verify_event

ENTRY = SimpleNamespace(entry_id="e1", data={}, options={})

# generate_device_id() for this key on an S3 board: "canary-s3-" + the
# unambiguous-alphabet rendering of pubkey_fp[0..1].
DEVICE_ID = "canary-s3-4dC2"
TEST_PUB = "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12"
WAP_FP = "7916CA487912FA1B"

WAP_HEALTH = (
    '{"battery":100,"battery_present":false,"memory_free":168224,"uptime":3600,'
    '"firmware_version":"2.4.15",'
    '"public_key":"2152F8D19B791D24453242E15F2EAB6CB7CFFA7B6A5ED30097960E069881DB12"}'
)
WAP_CHAIN = (
    '{"v":1,"length":42,'
    '"latest_hash":"a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf",'
    '"algorithm":"ed25519","alg":"ed25519","fp":"7916CA487912FA1B",'
    '"sig":"dFIRuNfzt0Xiu8dZp5CQx63SsM3l2YG_KDUFFKlBf7hDWwXyxm9DYTukZwBhAYNpi5ZDiylrE9mxPb9AKc89CA"}'
)
WAP_COUNTS = (
    '{"v":1,"total":1234,"alg":"ed25519","fp":"7916CA487912FA1B",'
    '"sig":"4176LMyKrwuNd5ZyVw5SGUx0Ps3MnJJg9zjT6gbmVgAHMtrI6Lwyn8h7RgR-hA175qMOzRNkjv96fxxnUtEZDg"}'
)
WAP_EVENT = (
    '{"event_id":77,"event_type":"present","timestamp":98,"zone":"","confidence":"likely",'
    '"signed":true,"module":"core.presence","type":"presence","category":"event",'
    '"privacy":"p1","state":"present","motion":42,"breathing":17,"bpm":14,'
    '"duration_sec":120,"bundled":1,"replay":false,"v":1,"alg":"ed25519",'
    '"fp":"7916CA487912FA1B",'
    '"sig":"MKYFGqnCMgTbqZH83Z9bvH-_WktERv_RR7ubjarog7i28YlWpsDo_Z9zjLuovY9_h7foHkUY1pjIYaBA89m6Bg"}'
)

UPPER_HEX = "0123456789ABCDEF"
LOWER_HEX = "0123456789abcdef"

# Present in the monorepo; absent in the HACS mirror, where the cross-check skips.
_WAP_SKETCH = (
    Path(__file__).resolve().parents[3]
    / "firmware" / "projects" / "canary-wap" / "arduino" / "canary_wap" / "canary_wap.ino"
)
# The encoder that spells the two MQTT strings since HA20, next to the sketch.
_WAP_MQTT_IDENTITY = _WAP_SKETCH.with_name("mqtt_identity.h")


# ─── harness ───────────────────────────────────────────────────────────


class _Hass(HomeAssistant):
    """Runs what the callbacks schedule (the TOFU pin, the notification)."""

    def __init__(self) -> None:
        super().__init__()
        self.notifications: list[dict] = []

    def async_create_task(self, coro):
        return run(coro)

    @property
    def services(self):
        outer = self

        class _Services:
            async def async_call(self, domain, service, data=None, **kwargs):
                outer.notifications.append(dict(data or {}))

        return _Services()


def _setup() -> tuple[_Hass, TrustStore]:
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
    return hass, store


def _msg(kind: str, body: str) -> SimpleNamespace:
    return SimpleNamespace(topic=f"securacv/{DEVICE_ID}/{kind}", payload=body)


def _entity(cls, hass):
    inst = cls("securacv", DEVICE_ID, ENTRY)
    inst.hass = hass
    inst.writes = []
    inst.async_write_ha_state = lambda: inst.writes.append(True)
    inst.async_on_remove = lambda unsub: None
    return inst


def _spelled(body: str, spelling: str) -> str:
    """The WAP body as sent, or with every hex identity lowercased (how the
    other builds spell it). Only `fp` / `public_key` change; the signature
    covers neither, so it still verifies."""
    if spelling == "wap-capitals":
        return body
    data = json.loads(body)
    for key in ("fp", "public_key"):
        if key in data:
            data[key] = data[key].lower()
    return json.dumps(data, separators=(",", ":"))


SIGNED = (
    ("chain", WAP_CHAIN, sensor_platform.SecuraCVCanaryChainLengthSensor),
    ("counts", WAP_COUNTS, sensor_platform.SecuraCVCanaryWitnessCountSensor),
    ("events", WAP_EVENT, sensor_platform.SecuraCVCanaryLastEventSensor),
)


# ─── the fixture is the WAP's spelling ─────────────────────────────────


def test_fixture_is_the_wap_spelling_of_the_test_key():
    priv = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32)
    assert priv.public_key().public_bytes_raw().hex() == TEST_PUB
    # Same bytes as HA's derivation, spelled by hex_to_str.
    assert fingerprint_from_pubkey_hex(TEST_PUB) == WAP_FP.lower()
    assert json.loads(WAP_HEALTH)["public_key"] == TEST_PUB.upper()
    for body in (WAP_CHAIN, WAP_COUNTS, WAP_EVENT):
        assert json.loads(body)["fp"] == WAP_FP
    # Deterministic Ed25519: the Python signer reproduces the firmware-built sig.
    head = bytes(range(0xA0, 0xC0))
    sig = priv.sign(build_chain_canonical(DEVICE_ID, 42, head.hex()))
    assert base64.urlsafe_b64encode(sig).rstrip(b"=").decode() == json.loads(WAP_CHAIN)["sig"]


def test_wap_firmware_spelling_is_one_this_file_covers():
    """The encoder canary_wap.ino spells its MQTT fp and health key with, and
    the two strings it feeds.

    Through 2.4.15 both came from hex_to_str (capitals); since HA20 both come
    from mqtt_identity.h's lowercase encoder. Both spellings run below, so a
    lowercase encoder passes unchanged. This fails if the encoder writes
    anything else, or if the envelope `fp` (device_signature::init's
    fingerprint), or the health key at any csi_mqtt::init call, stops coming
    from it: re-read how the WAP spells them, point these checks at the new
    source, and keep both spellings running below while units that send
    capitals are deployed."""
    if not _WAP_SKETCH.exists():
        pytest.skip("canary_wap.ino not present (HACS mirror checkout)")
    text = _WAP_SKETCH.read_text(encoding="utf-8")
    assert _WAP_MQTT_IDENTITY.exists(), (
        "mqtt_identity.h moved; re-read how the WAP spells its MQTT fp and key"
    )
    header = _WAP_MQTT_IDENTITY.read_text(encoding="utf-8")
    m = re.search(
        r"inline void hex_lower\([^)]*\)\s*\{\s*static const char kLowerHex\[\] = \"([^\"]*)\";",
        header,
    )
    assert m, "mqtt_identity.h's alphabet moved; re-read how the WAP spells its fp"
    assert m.group(1) in (UPPER_HEX, LOWER_HEX)
    assert re.search(
        r"mqtt_identity::fingerprint_hex\((\w+),\s*g_device\.pubkey_fp\);\s*"
        r"device_signature::init\(g_device\.privkey,\s*g_device\.pubkey,\s*"
        r"g_device\.device_id,\s*\1\);",
        text,
    ), "the envelope fp no longer comes from mqtt_identity::fingerprint_hex"
    # Every call, not only the boot one: the QR hub-provisioning path
    # re-inits csi_mqtt with a key of its own, split over two lines.
    calls = re.findall(r"csi_mqtt::init\(", text)
    fed = re.findall(
        r"mqtt_identity::public_key_hex\(pubkey_hex, g_device\.pubkey\);\s*"
        r"csi_mqtt::init\(g_device\.device_id,\s*FIRMWARE_VERSION,\s*pubkey_hex\);",
        text,
    )
    assert calls, "csi_mqtt::init moved; re-read where the health key comes from"
    assert len(fed) == len(calls), (
        f"{len(calls) - len(fed)} of {len(calls)} csi_mqtt::init calls no "
        "longer take the health public_key from mqtt_identity::public_key_hex"
    )
    # The capital fixture is the older WAP's spelling, and the lowercase run
    # is the newer one's, byte for byte.
    assert WAP_FP == WAP_FP.upper()
    if m.group(1) == LOWER_HEX:
        for body in (WAP_HEALTH, WAP_CHAIN, WAP_COUNTS, WAP_EVENT):
            data = json.loads(body)
            old = data.get("fp") or data.get("public_key")
            assert _spelled(body, "lowercase") == body.replace(old, old.lower())


# ─── end to end: TOFU from the health publish, then the signed topics ──


@pytest.mark.parametrize("spelling", ["wap-capitals", "lowercase"])
def test_signed_publishes_verify_after_tofu_from_health(spelling, caplog):
    hass, store = _setup()
    with caplog.at_level(logging.INFO):
        _async_health_for_tofu(hass, ENTRY)(_msg("health", _spelled(WAP_HEALTH, spelling)))
    # docs/device_trust.md "How to verify" step 2 sends owners to this line,
    # and says every key HA shows is lowercase.
    tofu_lines = [r.getMessage() for r in caplog.records if "TOFU-pinning" in r.getMessage()]
    assert tofu_lines == [f"TOFU-pinning Canary {DEVICE_ID} with pubkey {TEST_PUB[:16]}…"]

    pin = store.get(DEVICE_ID)
    assert pin is not None and pin.pin_source == PIN_SOURCE_TOFU

    for kind, body, cls in SIGNED:
        ent = _entity(cls, hass)
        ent._handle_message(_msg(kind, _spelled(body, spelling)))
        attrs = ent._attr_extra_state_attributes
        assert attrs["trust_reason"] == "ok", (kind, attrs)
        assert attrs["verified"] is True, (kind, attrs)
        assert attrs["pinned_fingerprint"] == WAP_FP.lower()
        assert attrs["received_fingerprint"] == WAP_FP.lower()
    assert hass.notifications == [], "a good signature raised a key-mismatch notice"

    # One lowercase spelling in the store, whatever the device sent.
    assert pin.pubkey_hex == TEST_PUB
    assert pin.fingerprint_hex == WAP_FP.lower()


@pytest.mark.parametrize("key_spelling", [TEST_PUB.upper(), TEST_PUB])
def test_manual_pin_then_wap_publishes_verify(key_spelling):
    """The pin form lowercases what it is given; the store does too, for any
    other caller. Either way the WAP's capital fp must match the pin."""
    hass, store = _setup()
    entry = run(store.async_pin(DEVICE_ID, key_spelling, source=PIN_SOURCE_MANUAL))
    assert entry.pubkey_hex == TEST_PUB
    for verifier, body in (
        (verify_chain, WAP_CHAIN),
        (verify_counts, WAP_COUNTS),
        (verify_event, WAP_EVENT),
    ):
        verdict = verifier(store, DEVICE_ID, json.loads(body))
        assert verdict.trusted and verdict.reason == "ok", (verifier.__name__, verdict)


def test_health_sensor_shows_the_key_in_lowercase():
    hass, _store = _setup()
    health = _entity(sensor_platform.SecuraCVCanaryHealthSensor, hass)
    health._handle_message(_msg("health", WAP_HEALTH))
    assert health._attr_extra_state_attributes["public_key"] == TEST_PUB


# ─── stored pins stay lowercase ────────────────────────────────────────


def test_a_capital_pin_in_an_existing_store_heals_to_lowercase():
    """Before the fix a TOFU pin kept the WAP's key as sent. Loading such a
    store lowercases it (same bytes) and writes the store back."""
    hass = HomeAssistant()
    old = TrustStore(hass, entry_id="e1")
    old._store._payload = {
        "version": 1,
        "devices": {
            DEVICE_ID: {
                "pubkey_hex": TEST_PUB.upper(),
                "fingerprint_hex": WAP_FP.lower(),
                "pinned_at": 1.0,
                "pin_source": PIN_SOURCE_TOFU,
                "previous": [
                    {"pubkey_hex": "AB" * 32, "fp": "ABCDEF0123456789", "retired_at": 0.5}
                ],
                "counters": {"length": 42},
            }
        },
    }
    run(old.async_load())
    pin = old.get(DEVICE_ID)
    assert pin.pubkey_hex == TEST_PUB
    assert pin.fingerprint_hex == WAP_FP.lower()
    assert pin.previous[0]["pubkey_hex"] == "ab" * 32
    assert pin.previous[0]["fp"] == "abcdef0123456789"
    assert pin.counters == {"length": 42}, "healing the spelling keeps the replay floor"
    saved = old._store._payload["devices"][DEVICE_ID]
    assert saved["pubkey_hex"] == TEST_PUB, "the healed spelling was written back"
    # And the healed pin verifies the WAP as sent.
    assert verify_chain(old, DEVICE_ID, json.loads(WAP_CHAIN)).reason == "ok"


# ─── ignoring case does not widen trust ────────────────────────────────


def test_a_different_key_in_capitals_is_still_a_mismatch():
    hass, store = _setup()
    run(store.async_pin(DEVICE_ID, TEST_PUB, source=PIN_SOURCE_MANUAL))
    other_fp = fingerprint_from_pubkey_hex("11" * 32).upper()
    payload = dict(json.loads(WAP_CHAIN), fp=other_fp)
    verdict = verify_chain(store, DEVICE_ID, payload)
    assert not verdict.trusted and verdict.reason == "mismatch"
    assert verdict.detail == "Fingerprint changed without rotation"
    assert verdict.received_fingerprint == other_fp.lower()


def test_a_tampered_publish_with_the_right_fp_in_capitals_still_fails():
    """The fp only picks the key; the signature is what is checked."""
    hass, store = _setup()
    run(store.async_pin(DEVICE_ID, TEST_PUB, source=PIN_SOURCE_MANUAL))
    payload = dict(json.loads(WAP_CHAIN), length=43)
    verdict = verify_chain(store, DEVICE_ID, payload)
    assert not verdict.trusted and verdict.reason == "mismatch"
    assert verdict.detail == "Signature failed to verify against pinned pubkey"


def test_one_mismatch_notice_per_fingerprint_whatever_its_case():
    hass, _store = _setup()
    for received in (WAP_FP, WAP_FP.lower()):
        async_record_verify(
            hass,
            ENTRY,
            DEVICE_ID,
            TrustVerdict(
                trusted=False,
                reason="mismatch",
                pinned_fingerprint="00" * 8,
                received_fingerprint=received,
            ),
        )
    assert len(hass.notifications) == 1
    assert hass.data[DOMAIN]["e1"]["mismatch_notified"] == {(DEVICE_ID, WAP_FP.lower())}


# ─── HA22: a 64-character key that is not 64 hex digits never reaches the pin task ──


@pytest.mark.parametrize(
    "bad_key",
    [
        TEST_PUB[:60] + "    ",            # bytes.fromhex skips whitespace: 30 bytes
        TEST_PUB[:30] + "  " + TEST_PUB[32:],  # whitespace in the middle
        "0x" + TEST_PUB[:62],              # a prefix is not hex
        TEST_PUB[:63] + "g",               # one non-hex digit
    ],
)
def test_a_64_char_key_that_is_not_64_hex_digits_is_not_pinned(bad_key, caplog):
    hass, store = _setup()
    body = json.loads(WAP_HEALTH)
    body["public_key"] = bad_key
    assert len(bad_key) == 64
    with caplog.at_level(logging.INFO):
        # Before HA22's fix the whitespace forms passed bytes.fromhex, the
        # pin task raised ValueError (a 30-byte key has no fingerprint), and
        # this call raised with it.
        _async_health_for_tofu(hass, ENTRY)(_msg("health", json.dumps(body)))
    assert store.get(DEVICE_ID) is None
    assert not [r for r in caplog.records if "TOFU-pinning" in r.getMessage()]
