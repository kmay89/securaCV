"""The Home Assistant seams, held to the SAME bytes the other side pins.

Every contract between the kernel/bridges (Rust), the add-on and sidecar
shells and this integration crosses a wire as bytes. The vectors under
``tests/fixtures/ha_contract/`` (written by ``gen_ha_contract_fixtures.py``)
are read by BOTH ends — the Rust unit/integration tests build the struct and
assert its serialization equals the vector; these tests parse the vector and
assert the integration derives what the contract says. A field renamed or
reordered on either side fails one file's readers on both sides.

  announce.json      C1  kernel_trust: parse, canonical bytes, signature, fingerprint
  claim.json         C3  claim: topic, payload, withdrawal; coordinator: claim_state
  discovery.json     C6  config_flow hassio step
  pair.json          C2  api.async_pair_kernel request/response/refusal; revoke 404/403
  event_state.json   C5  coordinator bucket/dedupe; kernel_verify.witness_event_attributes
  frigate_link.json  C5  coordinator push; the frigate_link binary sensor
  identity.json      C4  coordinator backstop: pin, chain head, kernel self-report

The fixtures live at the repository root; a checkout that carries only the
integration (the HACS mirror) skips this module.
"""

from __future__ import annotations

import json
import types
from datetime import datetime, timezone
from pathlib import Path

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, published, run
from .test_config_flow import CONF_URL, PROBE, drive, fake_probe, make_flow
from .test_coordinator import FakeApi, _coordinator, _msg

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import binary_sensor as binary_sensor_mod  # noqa: E402
from .. import claim  # noqa: E402
from .. import coordinator as coord_mod  # noqa: E402
from ..api import (  # noqa: E402
    PAIRED_TOKEN_LABEL,
    SecuraCVApi,
    SecuraCVApiAuthError,
    SecuraCVApiResponseError,
    async_pair_kernel,
)
from ..binary_sensor import SecuraCVKernelFrigateLinkSensor  # noqa: E402
from ..const import (  # noqa: E402
    AUTH_MODE_TOKEN_FILE,
    CLAIM_STATE_CLAIMED,
    CLAIM_STATE_UNCLAIMED,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_TOKEN_FILE,
    FRIGATE_LINK_STALE_AFTER_SECONDS,
    KERNEL_TOPIC_ANNOUNCE,
    KERNEL_TOPIC_CLAIM_STATE,
    KERNEL_TOPIC_FRIGATE_LINK,
    TIME_BUCKET_SECONDS,
    normalize_event_type,
)
from ..kernel_trust import (  # noqa: E402
    announce_canonical_bytes,
    kernel_key_fingerprint,
    parse_announce,
    verify_announce_signature,
)
from ..kernel_verify import witness_event_attributes  # noqa: E402

FIXTURES = Path(__file__).resolve().parents[3] / "tests" / "fixtures" / "ha_contract"
MANIFEST = Path(__file__).resolve().parents[1] / "manifest.json"

pytestmark = pytest.mark.skipif(
    not FIXTURES.is_dir(), reason="the shared seam vectors live at the repository root"
)

DEVICE_ID = "pwk_1a2b3c4d"
OTHER_KEY = "cd" * 32


def load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _mqtt_filter_matches(pattern: str, topic: str) -> bool:
    """MQTT wildcard match, enough for the manifest's filters."""
    pat, top = pattern.split("/"), topic.split("/")
    for i, part in enumerate(pat):
        if part == "#":
            return True
        if i >= len(top) or (part != "+" and part != top[i]):
            return False
    return len(pat) == len(top)


# ── C1 announce ─────────────────────────────────────────────────────────


def test_announce_vectors_verify_under_the_vector_s_key_and_no_other():
    fixture = load("announce.json")
    key = fixture["verifying_key"]
    assert fixture["topic"] == f"witness/{DEVICE_ID}/{KERNEL_TOPIC_ANNOUNCE}"
    assert kernel_key_fingerprint(key) == fixture["fingerprint"]
    filters = json.loads(MANIFEST.read_text(encoding="utf-8"))["mqtt"]
    assert any(_mqtt_filter_matches(f, fixture["topic"]) for f in filters), filters

    assert len(fixture["vectors"]) == 2
    for vector in fixture["vectors"]:
        announce = parse_announce(vector["published"])
        assert announce is not None, vector["name"]
        # The canonical bytes are rebuilt from the payload as received, keys
        # in the bridge's struct order — exactly what the kernel signed.
        assert announce_canonical_bytes(announce.raw) == vector["canonical_message"].encode("utf-8")
        assert announce.sig == vector["sig"]
        assert verify_announce_signature(announce, key) is True, vector["name"]
        assert verify_announce_signature(announce, OTHER_KEY) is False
        assert announce.fingerprint == fixture["fingerprint"]
        assert announce.device_id == DEVICE_ID
        assert announce.verifying_key == key
        assert announce.mqtt_topic_prefix == "witness"
        assert announce.availability_topic == "witness/status"
        # A reordered or edited payload is a different message: the pin says no.
        tampered = dict(announce.raw)
        tampered["kernel_version"] = "9.9.9"
        bad = parse_announce(json.dumps(tampered, separators=(",", ":")))
        assert bad is not None and verify_announce_signature(bad, key) is False

    no_host, hosted = fixture["vectors"]
    sidecar = parse_announce(no_host["published"])
    addon = parse_announce(hosted["published"])
    assert sidecar is not None and addon is not None
    assert sidecar.pairing == "required" and sidecar.api_host is None
    assert sidecar.api_url("192.168.1.20") == "http://192.168.1.20:8799"
    assert sidecar.api_url(None) is None
    assert addon.pairing == "token_file"
    assert addon.api_host == "d0491a67-privacy-witness-kernel"
    assert addon.api_url("ignored") == "http://d0491a67-privacy-witness-kernel:8799"


# ── C3 claim ────────────────────────────────────────────────────────────


def test_claim_vector_is_what_the_integration_publishes_and_withdraws(monkeypatch):
    fixture = load("claim.json")
    bucket = fixture["claimed_bucket_start"]
    assert claim.claim_topic("witness", DEVICE_ID) == fixture["topic"]
    assert claim.claim_payload(fixture["nonce"], now=bucket + 59) == fixture["payload"]
    assert claim.CLAIM_REPUBLISH_INTERVAL.total_seconds() == fixture["republish_interval_secs"]
    assert TIME_BUCKET_SECONDS == fixture["future_skew_secs"]
    assert fixture["republish_interval_secs"] < fixture["fresh_for_secs"]
    assert fixture["claim_state_topic"] == f"witness/{KERNEL_TOPIC_CLAIM_STATE}"
    assert fixture["claim_state_values"] == [CLAIM_STATE_CLAIMED, CLAIM_STATE_UNCLAIMED]

    hass = HomeAssistant()
    hass.data = {}
    monkeypatch.setattr(claim.time, "time", lambda: bucket + 59)
    entry = make_entry(
        data={
            CONF_KERNEL_DEVICE_ID: DEVICE_ID,
            CONF_CLAIM_NONCE: fixture["nonce"],
            CONF_MQTT_TOPIC_PREFIX: "witness",
        }
    )
    assert run(claim.async_publish_claim(hass, entry)) is True
    assert published(hass)[-1] == {
        "topic": fixture["topic"],
        "payload": fixture["payload"],
        "qos": 1,
        "retain": True,
    }
    run(claim.async_withdraw_claim(hass, "witness", DEVICE_ID))
    assert published(hass)[-1] == {
        "topic": fixture["topic"],
        "payload": fixture["withdrawal"].encode(),
        "qos": 1,
        "retain": True,
    }

    _hass, _entry, _api, coordinator = _coordinator()
    for word in fixture["claim_state_values"]:
        coordinator._handle_claim_state(_msg(fixture["claim_state_topic"], word))
        assert coordinator.data["claim_state"] == word


# ── C6 Supervisor discovery ─────────────────────────────────────────────


def test_discovery_vector_drives_the_hassio_step(monkeypatch):
    fixture = load("discovery.json")
    body = json.loads(fixture["payload"])
    assert body["service"] == "securacv"
    assert list(body["config"]) == ["host", "port", "token_file", "mqtt_topic_prefix", "device_id"]
    assert isinstance(body["config"]["port"], int)

    probes = fake_probe(monkeypatch, PROBE)
    flow = make_flow()
    info = types.SimpleNamespace(
        config=dict(body["config"]), name="SecuraCV",
        slug="d0491a67_privacy_witness_kernel", uuid="uuid-1",
    )
    result = drive(flow.async_step_hassio(info))
    assert result["type"] == "form" and result["step_id"] == "hassio_confirm"
    assert flow.unique_id == fixture["unique_id"]
    assert result["description_placeholders"]["url"] == fixture["url"]
    assert probes == [], "a payload naming its device id is not probed before confirm"

    result = drive(flow.async_step_hassio_confirm({}))
    assert result["type"] == "create_entry"
    data = result["data"]
    assert data[CONF_URL] == fixture["url"]
    assert data[CONF_TOKEN_FILE] == body["config"]["token_file"]
    assert data[CONF_MQTT_TOPIC_PREFIX] == body["config"]["mqtt_topic_prefix"]
    assert data[CONF_KERNEL_DEVICE_ID] == body["config"]["device_id"]
    assert data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE


# ── C2 pairing ──────────────────────────────────────────────────────────


class _Resp:
    def __init__(self, status: int, payload=None) -> None:
        self.status = status
        self._payload = payload

    async def json(self):
        return self._payload

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False


class _Session:
    """Answers queued responses; records what was sent."""

    def __init__(self, *responses: _Resp) -> None:
        self._responses = list(responses)
        self.posts: list[tuple[str, dict]] = []
        self.requests: list[tuple[str, str, dict]] = []

    def post(self, url, json=None, timeout=None):
        self.posts.append((url, json))
        return self._responses.pop(0)

    def request(self, method, url, headers=None, timeout=None, **kwargs):
        self.requests.append((method, url, dict(headers or {})))
        return self._responses.pop(0)


def test_pair_vector_is_the_request_the_integration_sends_and_the_answers_it_maps():
    fixture = load("pair.json")
    request = json.loads(fixture["request"])
    response = json.loads(fixture["response"])
    assert PAIRED_TOKEN_LABEL == request["label"]

    session = _Session(_Resp(200, response))
    result = run(async_pair_kernel(session, "http://192.168.1.20:8799/", fixture["code"]))
    assert session.posts == [("http://192.168.1.20:8799/api/pair", request)]
    assert list(session.posts[0][1]) == list(request), "the keys the kernel's PairRequest reads"
    assert result.token == response["token"]
    assert result.token_id == response["token_id"]
    assert result.device_id == response["device_id"]
    assert result.verifying_key == response["verifying_key"]
    assert result.kernel_version == response["kernel_version"]
    # The fingerprint the pair step shows is the one `pair` printed beside
    # the code (the receipt's `fingerprint`): one rule, both sides.
    receipt = json.loads(fixture["issue_pairing_code_stdout"])
    assert receipt["fingerprint"] == kernel_key_fingerprint(response["verifying_key"])
    assert receipt["fingerprint"] in fixture["pair_card"][1]
    assert receipt["pairing_code"] == fixture["code"] and fixture["code"] in fixture["pair_card"][0]

    # The one constant refusal maps to the form error; every other status
    # (403 `forbidden` included) and the malformed 400 are "cannot connect".
    refusal = fixture["refusal"]
    with pytest.raises(SecuraCVApiAuthError):
        run(async_pair_kernel(_Session(_Resp(refusal["status"], json.loads(refusal["body"]))),
                              "http://k:8799", fixture["code"]))
    forbidden = fixture["revoke"]["forbidden"]
    with pytest.raises(SecuraCVApiResponseError):
        run(async_pair_kernel(_Session(_Resp(forbidden["status"], json.loads(forbidden["body"]))),
                              "http://k:8799", fixture["code"]))
    malformed = fixture["malformed"]
    with pytest.raises(SecuraCVApiResponseError):
        run(async_pair_kernel(_Session(_Resp(malformed["status"], json.loads(malformed["body"]))),
                              "http://k:8799", fixture["code"]))


def test_revoke_vector_a_404_is_already_gone_and_a_403_is_not():
    fixture = load("pair.json")
    response = json.loads(fixture["response"])
    revoke = fixture["revoke"]

    ok = _Session(_Resp(revoke["ok"]["status"], json.loads(revoke["ok"]["body"])))
    api = SecuraCVApi("http://k:8799", response["token"], ok)
    assert run(api.async_revoke_paired_token(response["token_id"])) is None
    method, url, headers = ok.requests[0]
    assert (method, url) == ("DELETE", "http://k:8799" + revoke["path"])
    assert headers["Authorization"] == f"Bearer {response['token']}", "the paired bearer revokes itself"

    # The add-on (pairing off) and a kernel the operator already `unpair`ed
    # answer 404: the integration's removal treats that as done.
    gone = _Session(_Resp(revoke["gone"]["status"], json.loads(revoke["gone"]["body"])))
    assert run(SecuraCVApi("http://k:8799", response["token"], gone)
               .async_revoke_paired_token(response["token_id"])) is None
    # A paired bearer naming another id is refused, and that is an error.
    forbidden = _Session(_Resp(revoke["forbidden"]["status"], json.loads(revoke["forbidden"]["body"])))
    with pytest.raises(SecuraCVApiResponseError):
        run(SecuraCVApi("http://k:8799", response["token"], forbidden)
            .async_revoke_paired_token("ffffffff"))


# ── C5 event payload ────────────────────────────────────────────────────


def test_event_state_vector_yields_the_contract_s_attributes():
    fixture = load("event_state.json")
    event = json.loads(fixture["payload"])
    ha = fixture["ha"]
    assert coord_mod.event_bucket_start(event) == ha["time_bucket_start"]
    flat_only = {k: v for k, v in event.items() if k != "time_bucket"}
    assert coord_mod.event_bucket_start(flat_only) == ha["time_bucket_start"]
    assert normalize_event_type(event["event_type"]) == ha["event_type"]
    assert coord_mod.event_dedupe_key(event) == tuple(ha["dedupe_key"])
    assert coord_mod.event_dedupe_key(flat_only) == tuple(ha["dedupe_key"])

    attrs = witness_event_attributes(
        event, None, kernel_device_id=None,
        kernel_reported_chain_ok=None, kernel_verified_bucket_start=None,
    )
    assert attrs["zone_id"] == ha["zone_id"]
    assert attrs["zone"] == ha["zone"]
    assert attrs["time_bucket"] == event["time_bucket"]
    assert attrs["confidence"] == event["confidence"]
    assert attrs["attestation"] == ha["attestation"]
    assert attrs["friendly_event"] == ha["friendly_event"]
    assert attrs["kernel_version"] == event["kernel_version"]
    assert attrs["ruleset_id"] == event["ruleset_id"]
    assert attrs["kernel_device_id"] == event["device_id"]
    assert attrs["seq"] == ha["seq"]
    # The kernel's own last verify is a self-report, never `verified`.
    assert attrs["kernel_reported_chain_ok"] is ha["kernel_reported_chain_ok"]
    assert attrs["kernel_verified_bucket_start"] == ha["kernel_verified_bucket_start"]
    assert attrs["verification"] == ha["verification_without_a_walk"]
    assert attrs["trust_reason"] == ha["trust_reason_without_a_walk"]
    assert attrs["verified"] is False
    assert attrs["signed"] is True
    assert attrs["verified_bucket_start"] is None
    # What the timeline card and the strategy read, by name.
    for name in ("verification", "trust_reason", "seq", "friendly_event", "kernel_device_id",
                 "zone", "zone_id", "time_bucket", "confidence", "attestation", "verified",
                 "signed", "verified_bucket_start"):
        assert name in attrs, name


# ── C5 / D14 frigate_link ───────────────────────────────────────────────


class _Clock:
    def __init__(self, now: float) -> None:
        self._now = now

    def now(self, tz=None):
        return datetime.fromtimestamp(self._now, tz or timezone.utc)


def test_frigate_link_vector_drives_the_connectivity_sensor(monkeypatch):
    fixture = load("frigate_link.json")
    link = json.loads(fixture["payload"])
    assert fixture["topic"] == f"witness/{KERNEL_TOPIC_FRIGATE_LINK}"
    assert FRIGATE_LINK_STALE_AFTER_SECONDS == fixture["stale_after_secs"]

    _hass, entry, _api, coordinator = _coordinator()
    coordinator.kernel_online = True
    coordinator._handle_frigate_link(_msg(fixture["topic"], fixture["payload"]))
    assert coordinator.data["frigate_link"] == link

    sensor = SecuraCVKernelFrigateLinkSensor(coordinator, entry)
    heard = link["last_message_bucket_start"] + link["last_message_bucket_size"]
    monkeypatch.setattr(binary_sensor_mod, "datetime", _Clock(heard + 1800))
    assert sensor.is_on is True, "connected and heard within the hour"
    monkeypatch.setattr(binary_sensor_mod, "datetime",
                        _Clock(heard + fixture["stale_after_secs"] + 1))
    assert sensor.is_on is False, "a quiet hour is a dead link"
    attrs = sensor.extra_state_attributes
    assert attrs == {key: link[key] for key in attrs}
    for key in ("connected", "topic_prefix", "last_message_bucket_start",
                "last_message_bucket_size", "messages_since_start", "mode"):
        assert key in attrs, key

    disconnected = {**link, "connected": False}
    coordinator._handle_frigate_link(_msg(fixture["topic"], json.dumps(disconnected)))
    monkeypatch.setattr(binary_sensor_mod, "datetime", _Clock(heard + 1))
    assert sensor.is_on is False


# ── C4 identity ─────────────────────────────────────────────────────────


def test_identity_vector_pins_the_key_and_feeds_the_chain_head():
    fixture = load("identity.json")
    identity = json.loads(fixture["payload"])
    assert list(identity) == [
        "v", "device_id", "verifying_key", "key_algorithm", "kernel_version", "ruleset_id",
        "chain_length", "latest_hash", "last_verify", "fleet_rollcall", "paired_tokens",
    ]
    assert list(identity["last_verify"]) == ["at", "bucket_size", "ok", "verdict", "identity_verified"]
    assert identity["last_verify"]["at"] % TIME_BUCKET_SECONDS == 0, "a bucket start, never a precise time"

    api = FakeApi(identity=identity)
    _hass, entry, _api, coordinator = _coordinator(api=api, pinned=None)
    run(coordinator.async_refresh())
    assert coordinator.last_update_success is True
    assert coordinator.pinned_key == identity["verifying_key"]
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == identity["verifying_key"]
    assert coordinator.pinned_fingerprint == fixture["fingerprint"]
    assert coordinator.kernel_chain_length == identity["chain_length"]
    assert coordinator.kernel_latest_hash == identity["latest_hash"]
    assert coordinator.kernel_reported_chain_ok is True
    assert coordinator.kernel_reported_verified_bucket_start == identity["last_verify"]["at"]
    assert coordinator.key_mismatch is False
