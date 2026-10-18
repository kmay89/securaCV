"""Config-flow tests — every path keys a kernel entry on the kernel's identity.

Covers:
  - MQTT Canary discovery (attribute-style MqttServiceInfo, dedupe, the
    zero-field confirm);
  - the kernel announce: announce_confirm → pair (the fingerprint shown
    beside the code field, the URL prefilled), the kernel's constant 401,
    a key that disagrees with the announce, and — once a pin exists — the
    announce signature check and the Repairs issue a changed key raises;
  - Automatic: the app's hostname first, then a five-second listen for an
    announce, then the MQTT-only entry with the final-screen pointer and
    Repairs ``no_kernel_found``; an MQTT-only entry is promoted, never
    duplicated, and a custom URL is never stomped on a rerun;
  - Supervisor discovery (contract C6): unique_id ``kernel_<device_id>``,
    the legacy payload asked for its identity, ``reload_on_update``, and
    legacy-keyed entries adopted with a scheduled reload;
  - the manual forms (identity probe, error rebuilds keep typed values);
  - reauth and reconfigure through ``async_update_reload_and_abort``, and
    the refusal to move an entry to a different kernel.

Runs against the stub ConfigFlow base from conftest — no HA core needed.
"""

from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from . import conftest  # noqa: F401  (installs/augments ha stubs at import time)
from .conftest import issues, run

from homeassistant import data_entry_flow
from homeassistant.core import HomeAssistant

from .. import config_flow
from ..api import (
    PairResult,
    SecuraCVApiAuthError,
    SecuraCVApiConnectionError,
    SecuraCVApiPairLimitError,
    SecuraCVApiResponseError,
)
from ..config_flow import (
    CannotConnect,
    IdentityUnavailable,
    InvalidAuth,
    KernelProbe,
    SecuraCVConfigFlow,
)
from ..const import (
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_TOKEN_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_PAIRING_CODE,
    CONF_SETUP_MODE,
    CONF_TOKEN_FILE,
    DEFAULT_KERNEL_URL,
    DEFAULT_MQTT_PREFIX,
    DEFAULT_MQTT_TOPIC_PREFIX,
    DEFAULT_TOKEN_FILE,
    DOMAIN,
    ISSUE_NO_KERNEL_FOUND,
    ISSUE_TOFU_MISMATCH,
    SETUP_MODE_AUTO,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    SETUP_MODE_MQTT,
)
from ..kernel_trust import Announce, announce_canonical_bytes, kernel_key_fingerprint

CONF_TOKEN = "token"
CONF_URL = "url"

DEVICE_ID = "pwk_1a2b3c4d"
UNIQUE_ID = f"kernel_{DEVICE_ID}"
PRIV = Ed25519PrivateKey.from_private_bytes(b"\x07" * 32)
KEY = PRIV.public_key().public_bytes_raw().hex()
OTHER_KEY = Ed25519PrivateKey.from_private_bytes(b"\x08" * 32).public_key().public_bytes_raw().hex()
PROBE = KernelProbe(DEVICE_ID, KEY, "2.4.16")
PAIRED = PairResult(token="f1" * 32, token_id="0a1b2c3d", device_id=DEVICE_ID,
                    verifying_key=KEY, kernel_version="2.4.16")


def drive(coro):
    """Run a flow step; convert AbortFlow (raised by the unique-id
    helpers, normally caught by HA's flow manager) into an abort result
    dict so tests can assert on it uniformly."""
    try:
        return run(coro)
    except data_entry_flow.AbortFlow as err:
        return {"type": "abort", "reason": err.reason,
                "description_placeholders": err.description_placeholders or {}}


def make_hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {}
    return hass


def make_flow(entries=None, hass=None, source="user", context=None):
    flow = SecuraCVConfigFlow()
    flow.hass = hass or make_hass()
    flow.context = {"source": source, **(context or {})}
    flow._test_entries = list(entries or [])
    for entry in flow._test_entries:
        flow.hass.config_entries.add(entry)
    return flow


def make_entry(data, unique_id=None, entry_id="entry-1"):
    return SimpleNamespace(data=dict(data), unique_id=unique_id, entry_id=entry_id, options={})


def schema_defaults(result):
    """{key: default} for every marker in a form result's schema."""
    out = {}
    for marker in result["data_schema"].schema:
        out[marker.key] = marker.default
    return out


def schema_keys(result):
    return [marker.key for marker in result["data_schema"].schema]


def fake_probe(monkeypatch, outcome):
    """Replace the identity probe; `outcome` is a KernelProbe or an exception."""
    probes: list[dict] = []

    async def _probe(hass, data):
        probes.append(dict(data))
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    monkeypatch.setattr(config_flow, "_async_probe_kernel", _probe)
    return probes


def fake_pair(monkeypatch, outcome):
    """Replace POST /api/pair; `outcome` is a PairResult or an exception."""
    calls: list[tuple[str, str]] = []

    async def _pair(session, url, code, **kwargs):
        calls.append((url, code))
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    monkeypatch.setattr(config_flow, "async_pair_kernel", _pair)
    return calls


def no_announce(monkeypatch):
    async def _listen(self):
        return None

    monkeypatch.setattr(SecuraCVConfigFlow, "_async_listen_for_announce", _listen)


def announce_payload(**overrides) -> dict:
    payload = {
        "v": 1,
        "device_id": DEVICE_ID,
        "kernel_version": "2.4.16",
        "verifying_key": KEY,
        "api": {"scheme": "http", "bind": "all", "port": 8799, "host": "192.168.1.20"},
        "pairing": "required",
        "mqtt_topic_prefix": "witness",
        "discovery_prefix": "homeassistant",
        "availability_topic": "witness/status",
        "announced_bucket_start": 1760140800,
        "announced_bucket_size": 600,
    }
    payload.update(overrides)
    return payload


def signed_announce(priv=PRIV, **overrides) -> dict:
    payload = announce_payload(**overrides)
    payload["sig"] = priv.sign(announce_canonical_bytes(payload)).hex()
    return payload


def announce_info(payload: dict | str) -> SimpleNamespace:
    body = payload if isinstance(payload, str) else json.dumps(payload)
    return SimpleNamespace(
        topic=f"witness/{DEVICE_ID}/announce", payload=body, qos=1, retain=True,
        subscribed_topic="witness/+/announce", timestamp=0,
    )


# ---------------------------------------------------------------------------
# MQTT Canary discovery
# ---------------------------------------------------------------------------


def test_mqtt_discovery_attribute_style_service_info():
    """HA >= 2022.6 passes an attribute-style MqttServiceInfo — the old
    .get() call crashed here with AttributeError ("Unknown error")."""
    flow = make_flow()
    info = SimpleNamespace(
        topic="securacv/canary-abc/status",
        payload="{}",
        qos=0,
        retain=False,
        subscribed_topic="securacv/#",
        timestamp=0,
    )
    result = drive(flow.async_step_mqtt(info))

    assert result["type"] == "form"
    assert result["step_id"] == "confirm"
    # Zero-input confirm: the form has no fields and is confirm-only.
    assert result["data_schema"].schema == {}
    assert flow.confirm_only is True
    assert result["description_placeholders"] == {"prefix": "securacv"}
    assert flow.unique_id == "mqtt_securacv"
    assert flow.context["title_placeholders"] == {"name": "securacv"}

    # Submitting the empty form creates the entry with the discovered prefix.
    result = drive(flow.async_step_confirm({}))
    assert result["type"] == "create_entry"
    assert result["data"] == {
        CONF_ENABLE_MQTT: True,
        CONF_MQTT_PREFIX: "securacv",
        CONF_SETUP_MODE: SETUP_MODE_MQTT,
    }


def test_mqtt_discovery_plain_dict_service_info():
    """Older cores / stubs hand a plain dict; both shapes must work."""
    flow = make_flow()
    result = drive(flow.async_step_mqtt({"topic": "customprefix/dev-2/events"}))

    assert result["type"] == "form"
    assert result["step_id"] == "confirm"
    assert result["description_placeholders"] == {"prefix": "customprefix"}

    result = drive(flow.async_step_confirm({}))
    assert result["type"] == "create_entry"
    assert result["data"][CONF_MQTT_PREFIX] == "customprefix"


def test_mqtt_discovery_aborts_when_already_configured():
    """A configured system must abort discovery, not re-prompt."""
    existing = make_entry(
        {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT},
        unique_id="mqtt_securacv",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt(SimpleNamespace(topic="securacv/canary-abc/status")))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


def test_mqtt_discovery_aborts_when_a_kernel_entry_covers_the_prefix():
    """A kernel entry keys on the kernel id, so unique_id alone would miss
    it — the prefix scan must still abort discovery."""
    existing = make_entry(
        {
            CONF_URL: "http://kernel:8799",
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: "securacv",
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        },
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt(SimpleNamespace(topic="securacv/canary-abc/status")))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


# ---------------------------------------------------------------------------
# The kernel announce → announce_confirm → pair
# ---------------------------------------------------------------------------


def test_announce_shows_the_fingerprint_then_the_pair_form():
    flow = make_flow()
    result = drive(flow.async_step_mqtt(announce_info(announce_payload())))

    assert result["type"] == "form" and result["step_id"] == "announce_confirm"
    assert flow.confirm_only is True
    assert flow.unique_id == UNIQUE_ID
    assert flow.context["title_placeholders"] == {"name": "sidecar at 192.168.1.20"}
    assert result["description_placeholders"] == {
        "device_id": DEVICE_ID,
        "fingerprint": kernel_key_fingerprint(KEY),
        "kernel_version": "2.4.16",
        "url": "http://192.168.1.20:8799",
    }

    result = drive(flow.async_step_announce_confirm({}))
    assert result["type"] == "form" and result["step_id"] == "pair"
    assert schema_keys(result) == [CONF_URL, CONF_PAIRING_CODE]
    assert schema_defaults(result)[CONF_URL] == "http://192.168.1.20:8799"
    assert result["description_placeholders"]["fingerprint"] == kernel_key_fingerprint(KEY)
    assert result["description_placeholders"]["device_id"] == DEVICE_ID


def test_announce_without_a_host_prefills_the_broker_host():
    """The sidecar does not know its LAN address; HA's own broker is the guess."""
    hass = make_hass()
    hass.config_entries.add(SimpleNamespace(domain="mqtt", data={"broker": "10.0.0.5"}))
    flow = make_flow(hass=hass)
    payload = announce_payload()
    del payload["api"]["host"]
    drive(flow.async_step_mqtt(announce_info(payload)))
    assert flow.context["title_placeholders"] == {"name": f"sidecar {DEVICE_ID}"}
    result = drive(flow.async_step_announce_confirm({}))
    assert schema_defaults(result)[CONF_URL] == "http://10.0.0.5:8799"


def test_pairing_creates_the_both_entry(monkeypatch):
    calls = fake_pair(monkeypatch, PAIRED)
    flow = make_flow()
    drive(flow.async_step_mqtt(announce_info(announce_payload())))
    drive(flow.async_step_announce_confirm({}))

    result = drive(flow.async_step_pair({CONF_URL: "http://192.168.1.20:8799/",
                                         CONF_PAIRING_CODE: " 483921 "}))
    assert calls == [("http://192.168.1.20:8799", "483921")]
    assert result["type"] == "create_entry"
    data = result["data"]
    assert data[CONF_URL] == "http://192.168.1.20:8799"
    assert data[CONF_TOKEN] == PAIRED.token
    assert data[CONF_AUTH_MODE] == AUTH_MODE_PAIRED
    assert data[CONF_KERNEL_TOKEN_ID] == "0a1b2c3d"
    assert data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID
    assert data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert data[CONF_ENABLE_MQTT] is True and data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX
    assert data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert len(data[CONF_CLAIM_NONCE]) == 16 and int(data[CONF_CLAIM_NONCE], 16) >= 0
    assert CONF_TOKEN_FILE not in data


def test_pairing_errors_keep_the_person_on_the_form(monkeypatch):
    for outcome, expected in (
        (SecuraCVApiAuthError("invalid_pairing_code"), {"base": "invalid_pairing_code"}),
        (SecuraCVApiResponseError("unexpected status: 403"), {"base": "cannot_connect"}),
        # A full paired set: the code is still valid, the remedy is named.
        (SecuraCVApiPairLimitError("paired_token_limit"), {"base": "paired_token_limit"}),
        (SecuraCVApiConnectionError("unable to reach API"), {"base": "cannot_connect"}),
    ):
        fake_pair(monkeypatch, outcome)
        flow = make_flow()
        drive(flow.async_step_mqtt(announce_info(announce_payload())))
        result = drive(flow.async_step_pair({CONF_URL: "http://192.168.1.20:8799",
                                             CONF_PAIRING_CODE: "483921"}))
        assert result["type"] == "form" and result["step_id"] == "pair"
        assert result["errors"] == expected
        # The typed URL survives the error.
        assert schema_defaults(result)[CONF_URL] == "http://192.168.1.20:8799"


def test_a_malformed_code_never_reaches_the_kernel(monkeypatch):
    calls = fake_pair(monkeypatch, PAIRED)
    flow = make_flow()
    drive(flow.async_step_mqtt(announce_info(announce_payload())))
    for bad in ("12345", "1234567", "48392a", ""):
        result = drive(flow.async_step_pair({CONF_URL: "http://192.168.1.20:8799",
                                             CONF_PAIRING_CODE: bad}))
        assert result["errors"] == {CONF_PAIRING_CODE: "invalid_pairing_code"}
    result = drive(flow.async_step_pair({CONF_URL: "192.168.1.20:8799", CONF_PAIRING_CODE: "483921"}))
    assert result["errors"] == {CONF_URL: "invalid_url"}
    assert calls == []


def test_a_pair_response_whose_key_disagrees_with_the_announce_aborts(monkeypatch):
    """The pin never comes from the announce — but the two must agree, or
    something between HA and the kernel is lying. The minted token is revoked."""
    fake_pair(monkeypatch, PairResult(token="f1" * 32, token_id="0a1b2c3d", device_id=DEVICE_ID,
                                      verifying_key=OTHER_KEY, kernel_version="2.4.16"))
    revoked: list[str] = []

    class _Api:
        def __init__(self, url, token, session, token_file=None):
            pass

        async def async_revoke_paired_token(self, token_id):
            revoked.append(token_id)

    monkeypatch.setattr(config_flow, "SecuraCVApi", _Api)
    flow = make_flow()
    drive(flow.async_step_mqtt(announce_info(announce_payload())))
    result = drive(flow.async_step_pair({CONF_URL: "http://192.168.1.20:8799",
                                         CONF_PAIRING_CODE: "483921"}))
    assert result["type"] == "abort" and result["reason"] == "announce_key_mismatch"
    assert result["description_placeholders"] == {
        "pinned_fingerprint": kernel_key_fingerprint(KEY),
        "received_fingerprint": kernel_key_fingerprint(OTHER_KEY),
    }
    assert revoked == ["0a1b2c3d"]


def test_pairing_promotes_an_mqtt_only_entry_instead_of_a_second_one(monkeypatch):
    """J2 then J3: Canaries first, the sidecar paired later."""
    fake_pair(monkeypatch, PAIRED)
    existing = make_entry(
        {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_MQTT,
         CONF_CLAIM_NONCE: "feedfacefeedface"},
        unique_id="mqtt_securacv",
    )
    flow = make_flow(entries=[existing])
    drive(flow.async_step_mqtt(announce_info(announce_payload())))
    result = drive(flow.async_step_pair({CONF_URL: "http://192.168.1.20:8799",
                                         CONF_PAIRING_CODE: "483921"}))
    assert result["type"] == "abort" and result["reason"] == "already_configured"
    assert existing.unique_id == UNIQUE_ID
    assert existing.data[CONF_URL] == "http://192.168.1.20:8799"
    assert existing.data[CONF_AUTH_MODE] == AUTH_MODE_PAIRED
    assert existing.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert existing.data[CONF_CLAIM_NONCE] == "feedfacefeedface", "a published claim keeps its nonce"
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_an_announce_for_a_pinned_kernel_must_carry_its_signature():
    existing = make_entry(
        {CONF_URL: "http://old:8799", CONF_KERNEL_VERIFYING_KEY: KEY, CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_MQTT_TOPIC_PREFIX: "witness",
         CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )
    # Unsigned → ignored.
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt(announce_info(announce_payload())))
    assert result["type"] == "abort" and result["reason"] == "announce_signature_invalid"
    # Signed by someone else → ignored.
    other = Ed25519PrivateKey.from_private_bytes(b"\x09" * 32)
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt(announce_info(signed_announce(priv=other))))
    assert result["reason"] == "announce_signature_invalid"
    assert existing.data[CONF_URL] == "http://old:8799"


def test_a_signed_announce_updates_the_entry_and_reloads_it():
    """discovery-update-info: a pinned kernel that announces a new host moves the entry."""
    existing = make_entry(
        {CONF_URL: "http://old:8799", CONF_KERNEL_VERIFYING_KEY: KEY, CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_MQTT_TOPIC_PREFIX: "witness",
         CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt(announce_info(signed_announce(mqtt_topic_prefix="pwk"))))
    assert result["type"] == "abort" and result["reason"] == "already_configured"
    assert existing.data[CONF_URL] == "http://192.168.1.20:8799"
    assert existing.data[CONF_MQTT_TOPIC_PREFIX] == "pwk"
    assert flow._test_reloads == ["entry-1"]
    assert issues(flow.hass) == {}


def test_a_signed_announce_with_another_key_raises_the_repairs_issue():
    """The kernel's own key changed (signed by the pinned key, announcing a
    different one): a person decides, through Repairs — never the flow."""
    existing = make_entry(
        {CONF_URL: "http://old:8799", CONF_KERNEL_VERIFYING_KEY: KEY, CONF_ENABLE_MQTT: True,
         CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt(announce_info(signed_announce(verifying_key=OTHER_KEY))))
    assert result["type"] == "abort" and result["reason"] == "announce_key_mismatch"
    issue = issues(flow.hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_entry-1")]
    assert issue["is_fixable"] is True
    assert issue["translation_placeholders"]["pinned_fingerprint"] == kernel_key_fingerprint(KEY)
    assert issue["translation_placeholders"]["received_fingerprint"] == kernel_key_fingerprint(OTHER_KEY)
    assert issue["data"]["received_key"] == OTHER_KEY
    assert existing.data[CONF_URL] == "http://old:8799", "nothing moved"


def test_an_unpinned_legacy_entry_takes_nothing_from_an_unverifiable_announce():
    """No pin, no signature check: a retained announce anyone on the broker
    can publish must not move the entry's push subscriptions (its topic
    prefix) or its URL, nor reload it — alternating prefixes would reload
    the entry on every publish and point it at topics the publisher feeds."""
    existing = make_entry(
        {CONF_URL: "http://old:8799", CONF_ENABLE_MQTT: True, CONF_SETUP_MODE: SETUP_MODE_BOTH,
         CONF_MQTT_TOPIC_PREFIX: "witness"},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    hostile = announce_payload(mqtt_topic_prefix="x")
    result = drive(flow.async_step_mqtt(announce_info(hostile)))
    assert result["type"] == "abort" and result["reason"] == "already_configured"
    assert existing.data[CONF_URL] == "http://old:8799", "an unverifiable announce never moves the URL"
    assert existing.data[CONF_MQTT_TOPIC_PREFIX] == "witness", "nor the topic prefix"
    assert flow._test_reloads == [] and flow.hass.config_entries.reloads == []
    assert flow.hass.config_entries.updates == []
    # Even a signed announce means nothing without a pin to check it against.
    result = drive(flow.async_step_mqtt(announce_info(signed_announce(mqtt_topic_prefix="x"))))
    assert result["reason"] == "already_configured"
    assert existing.data[CONF_MQTT_TOPIC_PREFIX] == "witness" and flow._test_reloads == []


def test_the_token_file_announce_path_probes_and_pins(monkeypatch):
    """The app announcing over MQTT (pairing: token_file) ends in the
    token-file form, which probes /api/identity and pins what it serves."""
    probes = fake_probe(monkeypatch, PROBE)
    flow = make_flow()
    drive(flow.async_step_mqtt(announce_info(announce_payload(pairing="token_file"))))
    result = drive(flow.async_step_announce_confirm({}))
    assert result["type"] == "form" and result["step_id"] == "token_file"
    assert schema_defaults(result)[CONF_URL] == "http://192.168.1.20:8799"
    assert schema_defaults(result)[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE

    result = drive(flow.async_step_token_file({CONF_URL: "http://192.168.1.20:8799",
                                               CONF_TOKEN_FILE: "/config/api_token"}))
    assert probes[0][CONF_TOKEN_FILE] == "/config/api_token"
    assert result["type"] == "create_entry"
    assert result["data"][CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert result["data"][CONF_KERNEL_VERIFYING_KEY] == KEY
    assert result["data"][CONF_MQTT_TOPIC_PREFIX] == "witness"


def test_the_token_file_announce_path_refuses_a_disagreeing_identity(monkeypatch):
    fake_probe(monkeypatch, KernelProbe(DEVICE_ID, OTHER_KEY, "2.4.16"))
    flow = make_flow()
    drive(flow.async_step_mqtt(announce_info(announce_payload(pairing="token_file"))))
    drive(flow.async_step_announce_confirm({}))
    result = drive(flow.async_step_token_file({CONF_URL: "http://192.168.1.20:8799",
                                               CONF_TOKEN_FILE: "/config/api_token"}))
    assert result["type"] == "abort" and result["reason"] == "announce_key_mismatch"


# ---------------------------------------------------------------------------
# Automatic
# ---------------------------------------------------------------------------


def test_auto_mode_kernel_answers_creates_both_entry(monkeypatch):
    probes = fake_probe(monkeypatch, PROBE)
    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "create_entry", "auto must never show a form for the app"
    data = result["data"]
    assert data[CONF_URL] == DEFAULT_KERNEL_URL
    assert data[CONF_TOKEN] == ""
    assert data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID
    assert data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert data[CONF_MQTT_TOPIC_PREFIX] == DEFAULT_MQTT_TOPIC_PREFIX
    assert data[CONF_ENABLE_MQTT] is True and data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX
    assert data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert flow.unique_id == UNIQUE_ID
    # The probe used the documented defaults.
    assert probes[0][CONF_URL] == DEFAULT_KERNEL_URL
    assert probes[0][CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE


def test_auto_mode_kernel_without_identity_still_counts_as_present(monkeypatch):
    """An auth error proves something answered at the app's URL; the entry
    is created the legacy way and adopts the identity on the first backstop."""
    for outcome in (InvalidAuth(), IdentityUnavailable()):
        fake_probe(monkeypatch, outcome)
        flow = make_flow()
        result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))
        assert result["type"] == "create_entry"
        assert result["data"][CONF_SETUP_MODE] == SETUP_MODE_BOTH
        assert result["data"][CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
        assert CONF_KERNEL_DEVICE_ID not in result["data"]
        assert flow.unique_id == DEFAULT_KERNEL_URL


def test_auto_mode_nothing_found_creates_mqtt_entry_and_says_where_to_look(monkeypatch):
    fake_probe(monkeypatch, CannotConnect())
    no_announce(monkeypatch)
    hass = make_hass()
    hass.config_entries.add(SimpleNamespace(domain="mqtt", data={"broker": "10.0.0.5"}))
    flow = make_flow(hass=hass)
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "create_entry", "auto must never dead-end in a form"
    assert result["data"] == {
        CONF_ENABLE_MQTT: True,
        CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
        CONF_SETUP_MODE: SETUP_MODE_MQTT,
    }
    # The final screen says so, with the two links (J2).
    assert result["description"] == "no_kernel_found"
    assert set(result["description_placeholders"]) == {"addon_repo_url", "docker_quickstart_url"}
    assert result["description_placeholders"]["addon_repo_url"].startswith("https://my.home-assistant.io/")
    issue = issues(flow.hass)[(DOMAIN, ISSUE_NO_KERNEL_FOUND)]
    assert issue["is_fixable"] is False and issue["severity"] == "warning"
    assert issue["translation_placeholders"] == result["description_placeholders"]


def test_auto_mode_without_an_mqtt_integration_says_the_broker_comes_first(monkeypatch):
    """No MQTT entry in Home Assistant: no announce can ever be heard, so the
    final screen must not promise to connect by itself — it names the
    missing MQTT integration and the shared broker instead (same links)."""
    fake_probe(monkeypatch, CannotConnect())
    no_announce(monkeypatch)
    flow = make_flow()
    assert flow.hass.config_entries.async_entries("mqtt") == []
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))
    assert result["type"] == "create_entry"
    assert result["description"] == "no_kernel_found_no_mqtt"
    assert set(result["description_placeholders"]) == {"addon_repo_url", "docker_quickstart_url"}
    strings = json.loads((Path(config_flow.__file__).resolve().parent / "strings.json").read_text(encoding="utf-8"))
    text = strings["config"]["create_entry"]["no_kernel_found_no_mqtt"]
    assert "MQTT integration is not set up" in text and "share a broker" in text
    assert "{addon_repo_url}" in text and "{docker_quickstart_url}" in text
    assert "will connect by itself when one appears" not in text
    assert (DOMAIN, ISSUE_NO_KERNEL_FOUND) in issues(flow.hass)


def test_auto_mode_finds_a_sidecar_announce(monkeypatch):
    fake_probe(monkeypatch, CannotConnect())
    announce = Announce(
        device_id=DEVICE_ID, kernel_version="2.4.16", verifying_key=KEY, api_scheme="http",
        api_bind="all", api_port=8799, api_host="192.168.1.20", pairing="required",
        mqtt_topic_prefix="witness", discovery_prefix="homeassistant",
        availability_topic="witness/status", announced_bucket_start=1760140800, sig=None,
        raw=announce_payload(),
    )

    async def _listen(self):
        return announce

    monkeypatch.setattr(SecuraCVConfigFlow, "_async_listen_for_announce", _listen)
    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))
    assert result["type"] == "form" and result["step_id"] == "announce_confirm"
    assert flow.unique_id == UNIQUE_ID
    assert issues(flow.hass) == {}


def test_auto_mode_aborts_on_equivalent_entry(monkeypatch):
    fake_probe(monkeypatch, CannotConnect())
    no_announce(monkeypatch)
    existing = make_entry(
        {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_MQTT},
        unique_id=f"mqtt_{DEFAULT_MQTT_PREFIX}",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


def test_auto_mode_promotes_mqtt_only_entry_when_kernel_appears(monkeypatch):
    """Canaries first, kernel later: Automatic must not strand the kernel.

    With an MQTT-only entry already owning the default prefix and a kernel
    now answering, auto aborts (no second entry) but promotes the existing
    entry to `both`, keys it on the kernel, and reloads it.
    """
    fake_probe(monkeypatch, PROBE)
    existing = make_entry(
        {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_MQTT},
        unique_id=f"mqtt_{DEFAULT_MQTT_PREFIX}",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert existing.unique_id == UNIQUE_ID
    assert existing.data[CONF_URL] == DEFAULT_KERNEL_URL
    assert existing.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert existing.data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert existing.data[CONF_KERNEL_VERIFYING_KEY] == KEY
    # The subscription it already had survives.
    assert existing.data[CONF_ENABLE_MQTT] is True
    assert existing.data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_auto_mode_leaves_promoted_entry_alone_on_rerun(monkeypatch):
    """A second auto run against an already-promoted entry changes nothing."""
    fake_probe(monkeypatch, PROBE)
    existing = make_entry(
        {CONF_URL: "http://custom-host:8799", CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert existing.data[CONF_URL] == "http://custom-host:8799"  # never stomped
    assert flow.hass.config_entries.updates == []


def test_listen_for_announce_returns_the_retained_announce_and_unsubscribes(monkeypatch):
    """The real listener: a retained announce arrives on subscribe."""
    released: list[str] = []
    mqtt = config_flow.mqtt

    async def _subscribe(hass, topic, callback, **kwargs):
        callback(SimpleNamespace(topic=f"witness/{DEVICE_ID}/announce",
                                 payload=json.dumps(announce_payload()).encode()))
        return lambda: released.append(topic)

    monkeypatch.setattr(mqtt, "async_subscribe", _subscribe)
    flow = make_flow()
    announce = run(flow._async_listen_for_announce())
    assert announce is not None and announce.device_id == DEVICE_ID
    assert released == ["witness/+/announce"]


def test_listen_for_announce_gives_up_without_a_broker(monkeypatch):
    flow = make_flow()
    flow.hass.data["_securacv_test_mqtt_available"] = False
    assert run(flow._async_listen_for_announce()) is None


def test_user_form_defaults_to_auto():
    flow = make_flow()
    result = drive(flow.async_step_user(None))
    assert result["type"] == "form"
    markers = list(result["data_schema"].schema)
    assert markers[0].key == CONF_SETUP_MODE
    assert markers[0].default == SETUP_MODE_AUTO
    choices = list(result["data_schema"].schema[markers[0]].container)
    assert choices[0] == SETUP_MODE_AUTO


# ---------------------------------------------------------------------------
# Supervisor (hassio) discovery — contract C6
# ---------------------------------------------------------------------------

HASSIO_CONFIG = {
    "host": "d0491a67-privacy-witness-kernel",
    "port": 8799,
    "token_file": "/config/api_token",
    "mqtt_topic_prefix": "witness",
    "device_id": DEVICE_ID,
    "addon": "SecuraCV",
}


def test_hassio_discovery_creates_both_entry(monkeypatch):
    probes = fake_probe(monkeypatch, PROBE)
    flow = make_flow()
    info = SimpleNamespace(config=dict(HASSIO_CONFIG), name="SecuraCV",
                           slug="d0491a67_privacy_witness_kernel", uuid="uuid-1")
    result = drive(flow.async_step_hassio(info))
    assert result["type"] == "form"
    assert result["step_id"] == "hassio_confirm"
    assert result["data_schema"].schema == {}
    assert flow.confirm_only is True
    assert flow.unique_id == UNIQUE_ID
    # flow_title is "SecuraCV {name}": the card reads "SecuraCV app", not the
    # app's name twice; the confirm copy still has the name itself.
    assert flow.context["title_placeholders"] == {"name": "app"}
    assert result["description_placeholders"] == {
        "name": "SecuraCV",
        "url": "http://d0491a67-privacy-witness-kernel:8799",
    }
    assert probes == [], "a payload that names its device id is not probed before confirm"

    result = drive(flow.async_step_hassio_confirm({}))
    assert probes[0][CONF_URL] == "http://d0491a67-privacy-witness-kernel:8799"
    assert result["type"] == "create_entry"
    data = result["data"]
    assert data[CONF_URL] == "http://d0491a67-privacy-witness-kernel:8799"
    assert data[CONF_TOKEN] == ""
    assert data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID
    assert data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert data[CONF_ENABLE_MQTT] is True and data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX
    assert data[CONF_SETUP_MODE] == SETUP_MODE_BOTH


def test_hassio_discovery_legacy_payload_asks_the_kernel_who_it_is(monkeypatch):
    """A two-key payload (the old app): identity is read BEFORE the unique_id."""
    probes = fake_probe(monkeypatch, PROBE)
    flow = make_flow()
    result = drive(flow.async_step_hassio({"config": {"host": "10.0.0.7", "port": 8799}}))
    assert result["type"] == "form"
    assert result["step_id"] == "hassio_confirm"
    assert flow.unique_id == UNIQUE_ID
    assert probes[0][CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE

    result = drive(flow.async_step_hassio_confirm({}))
    assert result["type"] == "create_entry"
    assert result["data"][CONF_URL] == "http://10.0.0.7:8799"
    assert result["data"][CONF_KERNEL_DEVICE_ID] == DEVICE_ID


def test_hassio_discovery_legacy_payload_with_an_unreachable_kernel_aborts(monkeypatch):
    fake_probe(monkeypatch, CannotConnect())
    flow = make_flow()
    result = drive(flow.async_step_hassio({"config": {"host": "10.0.0.7", "port": 8799}}))
    assert result["type"] == "abort" and result["reason"] == "cannot_connect"


def test_hassio_confirm_reports_an_unreadable_token_file(monkeypatch):
    fake_probe(monkeypatch, InvalidAuth())
    flow = make_flow()
    drive(flow.async_step_hassio({"config": dict(HASSIO_CONFIG)}))
    result = drive(flow.async_step_hassio_confirm({}))
    assert result["type"] == "form" and result["step_id"] == "hassio_confirm"
    assert result["errors"] == {"base": "invalid_auth"}


def test_hassio_discovery_updates_a_kernel_keyed_entry_and_reloads_it():
    """A rediscovered app (new hostname) updates the entry through the base
    class: data changed → reload scheduled → abort. No update listener."""
    existing = make_entry(
        {CONF_URL: "http://old-host:8799", CONF_TOKEN: "", CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
         CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
         CONF_SETUP_MODE: SETUP_MODE_BOTH, CONF_KERNEL_DEVICE_ID: DEVICE_ID},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_hassio(SimpleNamespace(
        config={**HASSIO_CONFIG, "host": "new-host"}, name="SecuraCV", slug="s", uuid="u")))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert existing.data[CONF_URL] == "http://new-host:8799"
    assert existing.data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    # Existing MQTT settings survive the update.
    assert existing.data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX
    assert flow._test_reloads == ["entry-1"]


def test_hassio_discovery_identical_rediscovery_does_not_reload():
    existing = make_entry(
        {CONF_URL: "http://d0491a67-privacy-witness-kernel:8799", CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
         CONF_MQTT_TOPIC_PREFIX: "witness", CONF_KERNEL_DEVICE_ID: DEVICE_ID,
         CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_hassio({"config": dict(HASSIO_CONFIG)}))
    assert result["reason"] == "already_configured"
    assert flow._test_reloads == []


def test_hassio_discovery_adopts_a_legacy_keyed_entry(monkeypatch):
    """An entry keyed on the URL by an older version is moved to the kernel
    id, updated and reloaded — the base class cannot see it, so the step does."""
    fake_probe(monkeypatch, PROBE)
    existing = make_entry(
        {CONF_URL: "http://old-host:8799", CONF_TOKEN: "", CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id="http://old-host:8799",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_hassio(SimpleNamespace(
        config={**HASSIO_CONFIG, "host": "new-host"}, name="SecuraCV", slug="s", uuid="u")))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert existing.unique_id == UNIQUE_ID
    assert existing.data[CONF_URL] == "http://new-host:8799"
    assert existing.data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID
    assert existing.data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert existing.data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_hassio_discovery_legacy_payload_updates_a_legacy_entry_when_identity_is_unreadable(monkeypatch):
    """Both sides legacy and the kernel not identifiable yet: the entry still
    follows the new URL and reloads, and migrates itself when identity answers."""
    fake_probe(monkeypatch, CannotConnect())
    existing = make_entry(
        {CONF_URL: "http://old-host:8799", CONF_TOKEN: "", CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id="hassio",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_hassio({"config": {"host": "new-host", "port": 8799}}))
    assert result["reason"] == "already_configured"
    assert existing.unique_id == "hassio"
    assert existing.data[CONF_URL] == "http://new-host:8799"
    assert existing.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_hassio_discovery_promotes_mqtt_only_entry(monkeypatch):
    """When the kernel add-on announces itself to a system that was
    MQTT-only, the entry gains the kernel (both mode) instead of a
    second entry appearing."""
    fake_probe(monkeypatch, PROBE)
    existing = make_entry(
        {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_MQTT},
        unique_id=f"mqtt_{DEFAULT_MQTT_PREFIX}",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_hassio({"config": {**HASSIO_CONFIG, "host": "kernel-host"}}))
    assert result["type"] == "form" and result["step_id"] == "hassio_confirm"
    result = drive(flow.async_step_hassio_confirm({}))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert existing.unique_id == UNIQUE_ID
    assert existing.data[CONF_URL] == "http://kernel-host:8799"
    assert existing.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert existing.data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_hassio_discovery_missing_host_aborts():
    flow = make_flow()
    result = drive(flow.async_step_hassio({"config": {"port": 8799}}))
    assert result["type"] == "abort"
    assert result["reason"] == "invalid_discovery_info"


# ---------------------------------------------------------------------------
# The manual forms
# ---------------------------------------------------------------------------


def test_kernel_form_keys_the_entry_on_the_identity(monkeypatch):
    fake_probe(monkeypatch, PROBE)
    flow = make_flow()
    result = drive(flow.async_step_kernel({CONF_URL: "http://kernel.lan:8799", CONF_TOKEN: "abc"}))
    assert result["type"] == "create_entry"
    assert flow.unique_id == UNIQUE_ID
    data = result["data"]
    assert data[CONF_AUTH_MODE] == AUTH_MODE_STATIC
    assert data[CONF_ENABLE_MQTT] is False and data[CONF_SETUP_MODE] == SETUP_MODE_KERNEL
    assert data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID and data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert CONF_MQTT_PREFIX not in data


def test_both_mode_promotes_the_mqtt_only_entry_on_the_prefix(monkeypatch):
    """mqtt-only entries key on mqtt_<prefix>, kernel entries on the kernel —
    one system must end with ONE subscription on the prefix."""
    fake_probe(monkeypatch, PROBE)
    existing = make_entry(
        {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT},
        unique_id="mqtt_securacv",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_both({
        CONF_URL: "http://somewhere-else:8799",
        CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
        CONF_MQTT_PREFIX: "securacv",
    }))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert existing.data[CONF_URL] == "http://somewhere-else:8799"
    assert existing.unique_id == UNIQUE_ID


def test_mqtt_config_aborts_on_prefix_covered_by_kernel_entry():
    existing = make_entry(
        {CONF_URL: "http://kernel:8799", CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv",
         CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt_config({CONF_MQTT_PREFIX: "securacv"}))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


def test_kernel_error_rebuilds_form_with_typed_values(monkeypatch):
    fake_probe(monkeypatch, CannotConnect())
    flow = make_flow()
    typed = {
        CONF_URL: "http://my-kernel.lan:9999",
        CONF_TOKEN_FILE: "/share/securacv/api_token",
        CONF_TOKEN: "abc123",
        CONF_MQTT_TOPIC_PREFIX: "pwk",
    }
    result = drive(flow.async_step_kernel(dict(typed)))

    assert result["type"] == "form"
    assert result["errors"] == {"base": "cannot_connect"}
    defaults = schema_defaults(result)
    assert defaults[CONF_URL] == "http://my-kernel.lan:9999"
    assert defaults[CONF_TOKEN_FILE] == "/share/securacv/api_token"
    assert defaults[CONF_TOKEN] == "abc123"
    assert defaults[CONF_MQTT_TOPIC_PREFIX] == "pwk"


def test_both_error_rebuilds_form_with_typed_values(monkeypatch):
    fake_probe(monkeypatch, InvalidAuth())
    flow = make_flow()
    typed = {
        CONF_URL: "http://my-kernel.lan:9999",
        CONF_TOKEN_FILE: "/share/securacv/api_token",
        CONF_MQTT_PREFIX: "customprefix",
    }
    result = drive(flow.async_step_both(dict(typed)))

    assert result["type"] == "form"
    assert result["errors"] == {"base": "invalid_auth"}
    defaults = schema_defaults(result)
    assert defaults[CONF_URL] == "http://my-kernel.lan:9999"
    assert defaults[CONF_MQTT_PREFIX] == "customprefix"


def test_an_old_kernel_without_identity_is_named_as_such(monkeypatch):
    fake_probe(monkeypatch, IdentityUnavailable())
    flow = make_flow()
    result = drive(flow.async_step_kernel({CONF_URL: "http://old:8799", CONF_TOKEN: "abc"}))
    assert result["errors"] == {"base": "kernel_identity_unavailable"}


def test_kernel_fresh_form_uses_static_defaults():
    flow = make_flow()
    result = drive(flow.async_step_kernel(None))
    assert result["type"] == "form"
    defaults = schema_defaults(result)
    assert defaults[CONF_URL] == DEFAULT_KERNEL_URL
    assert defaults[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert defaults[CONF_MQTT_TOPIC_PREFIX] == DEFAULT_MQTT_TOPIC_PREFIX


def test_user_step_routes_manual_modes():
    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_MQTT}))
    assert result["step_id"] == "mqtt_config"

    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_KERNEL}))
    assert result["step_id"] == "kernel"

    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_BOTH}))
    assert result["step_id"] == "both"


def test_default_kernel_url_uses_resolvable_supervisor_hostname():
    """Supervisor DNS publishes the full slug with dashes; the old bare
    'privacy_witness_kernel' name never resolved from HA core."""
    assert DEFAULT_KERNEL_URL == "http://d0491a67-privacy-witness-kernel:8799"


# ---------------------------------------------------------------------------
# Reauth and reconfigure
# ---------------------------------------------------------------------------


def _paired_entry():
    return make_entry(
        {CONF_URL: "http://192.168.1.20:8799", CONF_TOKEN: "old" * 16, CONF_AUTH_MODE: AUTH_MODE_PAIRED,
         CONF_KERNEL_TOKEN_ID: "deadbeef", CONF_KERNEL_DEVICE_ID: DEVICE_ID,
         CONF_KERNEL_VERIFYING_KEY: KEY, CONF_MQTT_TOPIC_PREFIX: "witness", CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )


def _token_file_entry():
    return make_entry(
        {CONF_URL: DEFAULT_KERNEL_URL, CONF_TOKEN: "", CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
         CONF_AUTH_MODE: AUTH_MODE_TOKEN_FILE, CONF_KERNEL_DEVICE_ID: DEVICE_ID,
         CONF_KERNEL_VERIFYING_KEY: KEY, CONF_MQTT_TOPIC_PREFIX: "witness", CONF_ENABLE_MQTT: True,
         CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX, CONF_SETUP_MODE: SETUP_MODE_BOTH},
        unique_id=UNIQUE_ID,
    )


def test_reauth_of_a_paired_entry_asks_for_a_new_code(monkeypatch):
    calls = fake_pair(monkeypatch, PAIRED)
    entry = _paired_entry()
    flow = make_flow(entries=[entry], source="reauth", context={"entry_id": "entry-1"})
    result = drive(flow.async_step_reauth(entry.data))
    assert result["type"] == "form" and result["step_id"] == "reauth_confirm"
    assert schema_keys(result) == [CONF_URL, CONF_PAIRING_CODE]
    assert schema_defaults(result)[CONF_URL] == "http://192.168.1.20:8799"

    result = drive(flow.async_step_reauth_confirm({CONF_URL: "http://192.168.1.20:8799",
                                                   CONF_PAIRING_CODE: "483921"}))
    assert calls == [("http://192.168.1.20:8799", "483921")]
    assert result["type"] == "abort" and result["reason"] == "reauth_successful"
    assert entry.data[CONF_TOKEN] == PAIRED.token
    assert entry.data[CONF_KERNEL_TOKEN_ID] == "0a1b2c3d"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == KEY, "a re-pair never moves the pin"
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_reauth_refuses_a_different_kernel_and_revokes_the_token_it_minted(monkeypatch):
    """The code was redeemed on kernel B while re-authing the entry for A:
    B minted a live bearer nobody will hold — revoke it before refusing."""
    fake_pair(monkeypatch, PairResult(token="f1" * 32, token_id="0a1b2c3d", device_id="pwk_ffffffff",
                                      verifying_key=OTHER_KEY, kernel_version="2.4.16"))
    revoked: list[tuple[str, str, str]] = []

    class _Api:
        def __init__(self, url, token, session, token_file=None):
            self.url, self.token = url, token

        async def async_revoke_paired_token(self, token_id):
            revoked.append((self.url, self.token, token_id))

    monkeypatch.setattr(config_flow, "SecuraCVApi", _Api)
    entry = _paired_entry()
    flow = make_flow(entries=[entry], source="reauth", context={"entry_id": "entry-1"})
    drive(flow.async_step_reauth(entry.data))
    result = drive(flow.async_step_reauth_confirm({CONF_URL: "http://192.168.1.99:8799",
                                                   CONF_PAIRING_CODE: "483921"}))
    assert result["type"] == "abort" and result["reason"] == "wrong_kernel"
    assert entry.data[CONF_URL] == "http://192.168.1.20:8799"
    assert entry.data[CONF_TOKEN] != "f1" * 32, "the foreign token never reaches the entry"
    assert revoked == [("http://192.168.1.99:8799", "f1" * 32, "0a1b2c3d")], "revoked with the token it minted, on the kernel that minted it"
    assert flow.hass.config_entries.reloads == []


def test_reauth_wrong_kernel_still_aborts_when_the_revoke_fails(monkeypatch):
    fake_pair(monkeypatch, PairResult(token="f1" * 32, token_id="0a1b2c3d", device_id="pwk_ffffffff",
                                      verifying_key=OTHER_KEY, kernel_version="2.4.16"))

    class _Api:
        def __init__(self, url, token, session, token_file=None):
            pass

        async def async_revoke_paired_token(self, token_id):
            raise SecuraCVApiConnectionError("gone")

    monkeypatch.setattr(config_flow, "SecuraCVApi", _Api)
    entry = _paired_entry()
    flow = make_flow(entries=[entry], source="reconfigure", context={"entry_id": "entry-1"})
    result = drive(flow.async_step_reconfigure({CONF_URL: "http://192.168.1.99:8799",
                                                CONF_PAIRING_CODE: "483921"}))
    assert result["type"] == "abort" and result["reason"] == "wrong_kernel"


def test_reauth_of_a_token_file_entry_asks_for_the_file(monkeypatch):
    probes = fake_probe(monkeypatch, PROBE)
    entry = _token_file_entry()
    flow = make_flow(entries=[entry], source="reauth", context={"entry_id": "entry-1"})
    result = drive(flow.async_step_reauth(entry.data))
    assert result["type"] == "form" and result["step_id"] == "reauth_confirm"
    assert CONF_TOKEN_FILE in schema_keys(result)
    assert schema_defaults(result)[CONF_URL] == DEFAULT_KERNEL_URL

    result = drive(flow.async_step_reauth_confirm({CONF_URL: DEFAULT_KERNEL_URL,
                                                   CONF_TOKEN_FILE: "/share/api_token"}))
    assert probes[0][CONF_TOKEN_FILE] == "/share/api_token"
    assert result["type"] == "abort" and result["reason"] == "reauth_successful"
    assert entry.data[CONF_TOKEN_FILE] == "/share/api_token"
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE


def test_reconfigure_moves_the_url_and_prefix_on_the_same_kernel(monkeypatch):
    fake_probe(monkeypatch, PROBE)
    entry = _token_file_entry()
    flow = make_flow(entries=[entry], source="reconfigure", context={"entry_id": "entry-1"})
    result = drive(flow.async_step_reconfigure(None))
    assert result["type"] == "form" and result["step_id"] == "reconfigure"
    assert schema_defaults(result)[CONF_MQTT_TOPIC_PREFIX] == "witness"

    result = drive(flow.async_step_reconfigure({CONF_URL: "http://10.0.0.9:8799",
                                                CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
                                                CONF_MQTT_TOPIC_PREFIX: "pwk"}))
    assert result["type"] == "abort" and result["reason"] == "reconfigure_successful"
    assert entry.data[CONF_URL] == "http://10.0.0.9:8799"
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "pwk"
    assert entry.unique_id == UNIQUE_ID
    assert flow.hass.config_entries.reloads == ["entry-1"]


def test_reconfigure_refuses_a_different_kernel(monkeypatch):
    fake_probe(monkeypatch, KernelProbe("pwk_ffffffff", OTHER_KEY, "2.4.16"))
    entry = _token_file_entry()
    flow = make_flow(entries=[entry], source="reconfigure", context={"entry_id": "entry-1"})
    result = drive(flow.async_step_reconfigure({CONF_URL: "http://10.0.0.9:8799",
                                                CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE}))
    assert result["type"] == "abort" and result["reason"] == "wrong_kernel"
    assert entry.data[CONF_URL] == DEFAULT_KERNEL_URL
