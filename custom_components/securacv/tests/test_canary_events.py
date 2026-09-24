"""The canary base's committed-event wire, verified end to end (backlog F29).

The canary PIO tree now publishes every committed csi_event on
``securacv/<id>/events`` with the canary-wap's exact body — both hosts build
it with firmware/common/csi/src/csi_event_wire.h — signed with the device's
witness Ed25519 key, and bridges ``system.integrity`` tamper rows onto
``securacv/<id>/tamper`` as ``{"type": <kind>}``. These tests pin what Home
Assistant does with those bytes:

  - a canary-shaped signed body (the host test's golden, with a real
    signature in place of its fake one) verifies against the pinned key
    through the same ``verify_event`` the canary-wap uses;
  - an unsigned body says ``"signed": false`` and is reported unsigned;
  - the tamper bridge and the touch drain's new ``type`` key light the
    per-type sensors they name.

In the monorepo the goldens are also read out of
firmware/tests_host/test_csi_event_wire.cpp, so the Python copy cannot drift
from the bytes the firmware test pins; the HACS mirror carries no firmware
and skips that one test.
"""

from __future__ import annotations

import base64
import json
import re
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from .conftest import run
from .test_mqtt_payload_hardening import _entity, _msg

from .. import binary_sensor as bs_platform
from ..const import TAMPER_ENCLOSURE, TAMPER_SD_REMOVE
from ..device_trust import TrustStore
from ..signature import build_event_canonical, verify_event
from homeassistant.core import HomeAssistant

_WIRE_TEST = (
    Path(__file__).resolve().parents[3] / "firmware" / "tests_host" / "test_csi_event_wire.cpp"
)

# The firmware host test's goldens, with its fake fingerprint / signature
# replaced by placeholders (they are swapped for real ones below).
_FAKE_SIG = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    "ABCDEFGHIJKLMNOPQRSTUV"
)
TAMPER_ROW_GOLDEN = (
    '{"event_id":7,"event_type":"sd_remove","timestamp":5,"zone":"",'
    '"confidence":"tentative","signed":true,"module":"system.integrity",'
    '"type":"tamper","category":"anomaly","privacy":"p0",'
    '"state":"sd_remove","motion":0,"breathing":0,"bpm":0,'
    '"duration_sec":0,"bundled":1,"replay":false,"v":1,'
    '"alg":"ed25519","fp":"@FP@","sig":"@SIG@"}'
)
PRESENCE_GOLDEN = (
    '{"event_id":1234,"event_type":"present","timestamp":98,"zone":"",'
    '"confidence":"likely","signed":true,"module":"core.presence",'
    '"type":"presence","category":"event","privacy":"p1",'
    '"state":"present","motion":42,"breathing":17,"bpm":14,'
    '"duration_sec":120,"bundled":3,"replay":false,"v":1,'
    '"alg":"ed25519","fp":"@FP@","sig":"@SIG@"}'
)
UNSIGNED_GOLDEN = (
    '{"event_id":1234,"event_type":"present","timestamp":98,"zone":"",'
    '"confidence":"likely","signed":false,"module":"core.presence",'
    '"type":"presence","category":"event","privacy":"p1",'
    '"state":"present","motion":42,"breathing":17,"bpm":14,'
    '"duration_sec":120,"bundled":3,"replay":false,"v":1}'
)

DEVICE = "canary_a1b2c3"


def _b64url_nopad(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")


def _pinned_store():
    priv = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32)
    pub = priv.public_key().public_bytes_raw()
    ts = TrustStore(HomeAssistant(), entry_id="abc")
    run(ts.async_load())
    run(ts.async_pin(DEVICE, pub.hex(), source="manual"))
    return priv, ts


def _sign_golden(golden: str, priv, ts) -> dict:
    """Fill a golden's placeholders the way the canary does: its pinned
    fingerprint, and an Ed25519 signature over the event canonical HA
    rebuilds from the body's own fields."""
    fields = json.loads(golden.replace("@FP@", "x").replace("@SIG@", "x"))
    canonical = build_event_canonical(
        DEVICE, fields["event_id"], fields["state"], fields["category"],
        fields["privacy"], fields["motion"], fields["breathing"], fields["bpm"],
    )
    body = golden.replace("@FP@", ts.get(DEVICE).fingerprint_hex).replace(
        "@SIG@", _b64url_nopad(priv.sign(canonical))
    )
    return json.loads(body)


def test_canary_tamper_row_verifies_against_the_pinned_key() -> None:
    priv, ts = _pinned_store()
    payload = _sign_golden(TAMPER_ROW_GOLDEN, priv, ts)
    verdict = verify_event(ts, DEVICE, payload)
    assert verdict.trusted is True, verdict
    # A tampered-with body (the kind rewritten in transit) does not.
    forged = dict(payload, state="watchdog")
    assert verify_event(ts, DEVICE, forged).trusted is False


def test_canary_presence_row_verifies_against_the_pinned_key() -> None:
    priv, ts = _pinned_store()
    payload = _sign_golden(PRESENCE_GOLDEN, priv, ts)
    assert verify_event(ts, DEVICE, payload).trusted is True


def test_unsigned_canary_body_is_reported_unsigned_not_trusted() -> None:
    _priv, ts = _pinned_store()
    payload = json.loads(UNSIGNED_GOLDEN)
    assert payload["signed"] is False  # the body no longer claims otherwise
    verdict = verify_event(ts, DEVICE, payload)
    assert verdict.trusted is False


def _tamper_sensor(kind: str):
    # Names come from translations/ (translation_key), not the constructor.
    return _entity(bs_platform.SecuraCVCanaryTamperTypeSensor, kind, "mdi:alert")


def test_tamper_bridge_body_lights_the_named_sensor_only() -> None:
    removed = _tamper_sensor(TAMPER_SD_REMOVE)
    enclosure = _tamper_sensor(TAMPER_ENCLOSURE)
    bridge = '{"type":"sd_remove","severity":"tamper"}'
    removed._handle_tamper_message(_msg(bridge))
    enclosure._handle_tamper_message(_msg(bridge))
    assert removed._attr_is_on is True
    assert enclosure._attr_is_on is False
    enclosure._handle_tamper_message(_msg('{"type":"enclosure","severity":"tamper"}'))
    assert enclosure._attr_is_on is True


def test_touch_drain_payload_lights_enclosure_open_and_keeps_kind() -> None:
    """The canary's touch-pad tamper now carries HA's `type` beside the
    adapter's `kind`; before, no per-type sensor could match it."""
    payload = '{"state":"on","confidence":0.87,"kind":"enclosure_tamper","type":"enclosure"}'
    assert json.loads(payload)["kind"] == "enclosure_tamper"  # adapter contract kept
    enclosure = _tamper_sensor(TAMPER_ENCLOSURE)
    enclosure._handle_tamper_message(_msg(payload))
    assert enclosure._attr_is_on is True
    # A kind-only payload (temp drift) still lights no per-type sensor.
    other = _tamper_sensor(TAMPER_ENCLOSURE)
    other._handle_tamper_message(_msg('{"state":"on","confidence":0.5,"kind":"temp_drift"}'))
    assert other._attr_is_on is False


def _c_golden(text: str, func: str) -> str:
    """The first CHECK_STR(body, ...) golden inside a C++ test function, as
    the string the adjacent literals concatenate to."""
    m = re.search(r"static int " + func + r"\(\)\s*\{(.*?)\n\}", text, re.S)
    assert m, f"{func} moved; update this test"
    call = re.search(r"CHECK_STR\(body,(.*?)\);", m.group(1), re.S)
    assert call, f"{func}: no CHECK_STR(body, ...) golden"
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', call.group(1))
    return "".join(parts).encode().decode("unicode_escape")


@pytest.mark.skipif(
    not _WIRE_TEST.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)
def test_python_goldens_are_the_firmware_host_test_bytes() -> None:
    text = _WIRE_TEST.read_text(encoding="utf-8")
    tamper = _c_golden(text, "test_system_integrity_row_golden")
    assert tamper.replace("fedcba9876543210", "@FP@").replace(_FAKE_SIG, "@SIG@") == TAMPER_ROW_GOLDEN
    presence = _c_golden(text, "test_signed_golden_and_canonical_tuple")
    assert presence.replace("0123456789abcdef", "@FP@").replace(_FAKE_SIG, "@SIG@") == PRESENCE_GOLDEN
    assert _c_golden(text, "test_unsigned_golden") == UNSIGNED_GOLDEN
