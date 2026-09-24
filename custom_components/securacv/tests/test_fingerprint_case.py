"""A canary-wap's signed publishes verify: fingerprints compare ignoring case.

The canary-wap spells hex in capitals: `hex_to_str` in canary_wap.ino writes
"0123456789ABCDEF", and it is what fills `g_device.fingerprint_hex` (the `fp`
of every signed chain / counts / events envelope, via device_signature::init)
and the `public_key` of the health publish (via csi_mqtt::init). HA derives
the pinned fingerprint in lowercase (`fingerprint_from_pubkey_hex`), and the
verifier compared the two strings exactly, so after TOFU every signed publish
from a canary-wap read `mismatch` ("Fingerprint changed without rotation")
and raised a key-mismatch notification, even though its signature was good.

The WAP_* bodies below are what a canary-wap publishes, byte for byte, for the
repo's Ed25519 test key (seed 0x42 * 32, test_signature.py `_make_keypair`).
They were produced on the host by compiling the firmware's own code:
`sha256_domain`, `compute_fingerprint`, `hex_to_str` and `generate_device_id`
lifted verbatim from canary_wap.ino (S3 prefix), with
firmware/common/identity/device_signature.cpp for init, the canonicals and
b64url, firmware/common/csi/src/csi_event_wire.h for the events body, and
csi_mqtt.cpp's health / chain / counts formats; signed with OpenSSL's
Ed25519, which is deterministic, so the Python signer below reproduces
every `sig`.

On the exact-compare code every WAP-spelling test here fails (the end-to-end
one with `mismatch` on the first signed topic). The lowercase runs, the
fixture check and the firmware cross-check pass on both, and are the
regression guard for every build that already spells hex in lowercase.
"""

from __future__ import annotations

import base64
import json
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
    """canary_wap.ino's hex_to_str alphabet, and the two strings it feeds.

    Both spellings run below, so this does not need updating if the WAP
    ever emits lowercase; it fails if the envelope `fp` or the health key
    stop coming from hex_to_str, or if hex_to_str emits something else."""
    if not _WAP_SKETCH.exists():
        pytest.skip("canary_wap.ino not present (HACS mirror checkout)")
    text = _WAP_SKETCH.read_text(encoding="utf-8")
    m = re.search(
        r"static void hex_to_str\([^)]*\)\s*\{\s*static const char hex\[\] = \"([^\"]*)\";",
        text,
    )
    assert m, "hex_to_str's alphabet moved; re-read how the WAP spells its fp"
    assert m.group(1) in (UPPER_HEX, LOWER_HEX)
    assert "hex_to_str(g_device.fingerprint_hex, g_device.pubkey_fp, 8);" in text
    assert re.search(
        r"device_signature::init\([^;]*g_device\.fingerprint_hex\);", text
    ), "the envelope fp no longer comes from g_device.fingerprint_hex"
    assert re.search(
        r"hex_to_str\(pubkey_hex, g_device\.pubkey, 32\);\s*"
        r"csi_mqtt::init\(g_device\.device_id, FIRMWARE_VERSION, pubkey_hex\);",
        text,
    ), "the health public_key no longer comes from hex_to_str"
    if m.group(1) == UPPER_HEX:
        assert WAP_FP == WAP_FP.upper()


# ─── end to end: TOFU from the health publish, then the signed topics ──


@pytest.mark.parametrize("spelling", ["wap-capitals", "lowercase"])
def test_signed_publishes_verify_after_tofu_from_health(spelling):
    hass, store = _setup()
    _async_health_for_tofu(hass, ENTRY)(_msg("health", _spelled(WAP_HEALTH, spelling)))

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


@pytest.mark.parametrize("key_spelling", [TEST_PUB.upper(), TEST_PUB])
def test_manual_pin_then_wap_publishes_verify(key_spelling):
    """Whatever case the key was pinned in, the WAP's capital fp must match
    the pin, whose fingerprint is derived in lowercase."""
    hass, store = _setup()
    run(store.async_pin(DEVICE_ID, key_spelling, source=PIN_SOURCE_MANUAL))
    for verifier, body in (
        (verify_chain, WAP_CHAIN),
        (verify_counts, WAP_COUNTS),
        (verify_event, WAP_EVENT),
    ):
        verdict = verifier(store, DEVICE_ID, json.loads(body))
        assert verdict.trusted and verdict.reason == "ok", (verifier.__name__, verdict)


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
