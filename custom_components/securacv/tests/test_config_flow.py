"""Config-flow tests — the flow shipped broken because it had none.

Covers the bugs that actually bit:
  - MQTT discovery crashed on attribute-style MqttServiceInfo (HA >= 2022.6
    passes an object, the flow called .get() on it -> "Unknown error");
  - discovery re-prompted an already-configured system instead of aborting;
  - `auto` mode: submitting it must NEVER show another form — kernel up
    means a "both" entry, kernel down means an MQTT-only entry;
  - Supervisor add-on (hassio) discovery: create on first sight, update the
    kernel URL + abort on an existing entry;
  - one system, two subscriptions: a both-mode entry (URL-keyed unique_id)
    must not stack a second MQTT subscription on a prefix an MQTT-only
    entry (prefix-keyed unique_id) already covers;
  - a validation error must rebuild the form with the just-typed values as
    defaults, not the static defaults.

Runs against the stub ConfigFlow base from conftest — no HA core needed.
"""

from __future__ import annotations

from types import SimpleNamespace

from . import conftest  # noqa: F401  (installs/augments ha stubs at import time)
from .conftest import run

from homeassistant import data_entry_flow

from .. import config_flow
from ..config_flow import (
    CannotConnect,
    InvalidAuth,
    SecuraCVConfigFlow,
)
from ..const import (
    CONF_ENABLE_MQTT,
    CONF_MQTT_PREFIX,
    CONF_SETUP_MODE,
    CONF_TOKEN_FILE,
    DEFAULT_KERNEL_URL,
    DEFAULT_MQTT_PREFIX,
    DEFAULT_TOKEN_FILE,
    SETUP_MODE_AUTO,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    SETUP_MODE_MQTT,
)

CONF_TOKEN = "token"
CONF_URL = "url"


def drive(coro):
    """Run a flow step; convert AbortFlow (raised by the unique-id
    helpers, normally caught by HA's flow manager) into an abort result
    dict so tests can assert on it uniformly."""
    try:
        return run(coro)
    except data_entry_flow.AbortFlow as err:
        return {"type": "abort", "reason": err.reason}


def make_flow(entries=None, hass=None):
    flow = SecuraCVConfigFlow()
    flow.hass = hass or SimpleNamespace(
        config_entries=SimpleNamespace(async_update_entry=lambda *a, **kw: None),
        data={},
    )
    flow._test_entries = list(entries or [])
    return flow


def make_entry(data, unique_id=None, entry_id="entry-1"):
    return SimpleNamespace(data=dict(data), unique_id=unique_id, entry_id=entry_id)


def schema_defaults(result):
    """{key: default} for every marker in a form result's schema."""
    out = {}
    for marker in result["data_schema"].schema:
        out[marker.key] = marker.default
    return out


# ---------------------------------------------------------------------------
# MQTT discovery
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
    # Zero-input confirm: the form has no fields.
    assert result["data_schema"].schema == {}
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
        {
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: "securacv",
            CONF_SETUP_MODE: SETUP_MODE_MQTT,
        },
        unique_id="mqtt_securacv",
    )
    flow = make_flow(entries=[existing])
    result = drive(
        flow.async_step_mqtt(SimpleNamespace(topic="securacv/canary-abc/status"))
    )
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


def test_mqtt_discovery_aborts_when_both_entry_covers_prefix():
    """A both-mode entry keys on the URL, so unique_id alone would miss
    it — the prefix scan must still abort discovery."""
    existing = make_entry(
        {
            CONF_URL: "http://kernel:8799",
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: "securacv",
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        },
        unique_id="http://kernel:8799",
    )
    flow = make_flow(entries=[existing])
    result = drive(
        flow.async_step_mqtt(SimpleNamespace(topic="securacv/canary-abc/status"))
    )
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


# ---------------------------------------------------------------------------
# auto mode
# ---------------------------------------------------------------------------


def test_auto_mode_kernel_answers_creates_both_entry(monkeypatch):
    probes = []

    async def fake_validate(hass, data):
        probes.append(dict(data))
        return None  # kernel answered

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "create_entry", "auto must never show a form"
    assert result["data"] == {
        CONF_URL: DEFAULT_KERNEL_URL,
        CONF_TOKEN: "",
        CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
        CONF_ENABLE_MQTT: True,
        CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
        CONF_SETUP_MODE: SETUP_MODE_BOTH,
    }
    # The probe used the documented defaults.
    assert probes[0][CONF_URL] == DEFAULT_KERNEL_URL
    assert probes[0][CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE


def test_auto_mode_invalid_auth_still_counts_as_kernel_present(monkeypatch):
    """An auth error proves something answered at the kernel URL."""

    async def fake_validate(hass, data):
        raise InvalidAuth

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))
    assert result["type"] == "create_entry"
    assert result["data"][CONF_SETUP_MODE] == SETUP_MODE_BOTH


def test_auto_mode_kernel_down_creates_mqtt_entry(monkeypatch):
    async def fake_validate(hass, data):
        raise CannotConnect

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    flow = make_flow()
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "create_entry", "auto must never show a form"
    assert result["data"] == {
        CONF_ENABLE_MQTT: True,
        CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
        CONF_SETUP_MODE: SETUP_MODE_MQTT,
    }


def test_auto_mode_aborts_on_equivalent_entry(monkeypatch):
    async def fake_validate(hass, data):
        raise CannotConnect

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    existing = make_entry(
        {
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
            CONF_SETUP_MODE: SETUP_MODE_MQTT,
        },
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
    entry to `both` so the kernel is actually used.
    """

    async def fake_validate(hass, data):
        return None  # kernel answered

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    existing = make_entry(
        {
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
            CONF_SETUP_MODE: SETUP_MODE_MQTT,
        },
        unique_id=f"mqtt_{DEFAULT_MQTT_PREFIX}",
    )
    updates = []
    hass = SimpleNamespace(
        config_entries=SimpleNamespace(
            async_update_entry=lambda entry, **kw: updates.append((entry, kw))
        ),
        data={},
    )
    flow = make_flow(entries=[existing], hass=hass)
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert len(updates) == 1
    data = updates[0][1]["data"]
    assert data[CONF_URL] == DEFAULT_KERNEL_URL
    assert data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    # The subscription it already had survives.
    assert data[CONF_ENABLE_MQTT] is True
    assert data[CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX


def test_auto_mode_leaves_promoted_entry_alone_on_rerun(monkeypatch):
    """A second auto run against an already-promoted entry changes nothing."""

    async def fake_validate(hass, data):
        return None

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    existing = make_entry(
        {
            CONF_URL: "http://custom-host:8799",
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        },
        unique_id=f"mqtt_{DEFAULT_MQTT_PREFIX}",
    )
    updates = []
    hass = SimpleNamespace(
        config_entries=SimpleNamespace(
            async_update_entry=lambda entry, **kw: updates.append((entry, kw))
        ),
        data={},
    )
    flow = make_flow(entries=[existing], hass=hass)
    result = drive(flow.async_step_user({CONF_SETUP_MODE: SETUP_MODE_AUTO}))

    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert updates == []  # a user's custom kernel URL is never stomped


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
# Supervisor (hassio) discovery
# ---------------------------------------------------------------------------


def test_hassio_discovery_creates_both_entry():
    flow = make_flow()
    info = SimpleNamespace(
        config={"host": "d0491a67-privacy-witness-kernel", "port": 8799},
        name="Privacy Witness Kernel",
        slug="d0491a67_privacy_witness_kernel",
        uuid="uuid-1",
    )
    result = drive(flow.async_step_hassio(info))
    assert result["type"] == "form"
    assert result["step_id"] == "hassio_confirm"
    assert result["data_schema"].schema == {}
    assert (
        result["description_placeholders"]["url"]
        == "http://d0491a67-privacy-witness-kernel:8799"
    )

    result = drive(flow.async_step_hassio_confirm({}))
    assert result["type"] == "create_entry"
    assert result["data"] == {
        CONF_URL: "http://d0491a67-privacy-witness-kernel:8799",
        CONF_TOKEN: "",
        CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
        CONF_ENABLE_MQTT: True,
        CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
        CONF_SETUP_MODE: SETUP_MODE_BOTH,
    }


def test_hassio_discovery_plain_dict():
    flow = make_flow()
    result = drive(
        flow.async_step_hassio({"config": {"host": "10.0.0.7", "port": 8799}})
    )
    assert result["type"] == "form"
    assert result["step_id"] == "hassio_confirm"

    result = drive(flow.async_step_hassio_confirm({}))
    assert result["type"] == "create_entry"
    assert result["data"][CONF_URL] == "http://10.0.0.7:8799"


def test_hassio_discovery_updates_existing_entry_and_aborts():
    """An already-set-up system gets its kernel URL refreshed, no new entry."""
    existing = make_entry(
        {
            CONF_URL: "http://old-host:8799",
            CONF_TOKEN: "",
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        },
        unique_id="http://old-host:8799",
    )
    updates = []
    hass = SimpleNamespace(
        config_entries=SimpleNamespace(
            async_update_entry=lambda entry, **kw: updates.append((entry, kw))
        ),
        data={},
    )
    flow = make_flow(entries=[existing], hass=hass)
    result = drive(
        flow.async_step_hassio(
            SimpleNamespace(config={"host": "new-host", "port": 8799})
        )
    )
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    assert len(updates) == 1
    entry, kwargs = updates[0]
    assert entry is existing
    assert kwargs["data"][CONF_URL] == "http://new-host:8799"
    # Existing MQTT settings survive the update.
    assert kwargs["data"][CONF_MQTT_PREFIX] == DEFAULT_MQTT_PREFIX


def test_hassio_discovery_promotes_mqtt_only_entry():
    """When the kernel add-on announces itself to a system that was
    MQTT-only, the entry gains the kernel (both mode) instead of a
    second entry appearing."""
    existing = make_entry(
        {
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
            CONF_SETUP_MODE: SETUP_MODE_MQTT,
        },
        unique_id=f"mqtt_{DEFAULT_MQTT_PREFIX}",
    )
    updates = []
    hass = SimpleNamespace(
        config_entries=SimpleNamespace(
            async_update_entry=lambda entry, **kw: updates.append((entry, kw))
        ),
        data={},
    )
    flow = make_flow(entries=[existing], hass=hass)
    result = drive(
        flow.async_step_hassio({"config": {"host": "kernel-host", "port": 8799}})
    )
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"
    data = updates[0][1]["data"]
    assert data[CONF_URL] == "http://kernel-host:8799"
    assert data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE


def test_hassio_discovery_missing_host_aborts():
    flow = make_flow()
    result = drive(flow.async_step_hassio({"config": {"port": 8799}}))
    assert result["type"] == "abort"
    assert result["reason"] == "invalid_discovery_info"


# ---------------------------------------------------------------------------
# Duplicate-subscription guard
# ---------------------------------------------------------------------------


def test_both_mode_aborts_on_duplicate_prefix(monkeypatch):
    """mqtt-only entries key on mqtt_<prefix>, both-mode on the URL —
    without the prefix scan one system gets two MQTT subscriptions."""

    async def fake_validate(hass, data):
        return None

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    existing = make_entry(
        {
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: "securacv",
            CONF_SETUP_MODE: SETUP_MODE_MQTT,
        },
        unique_id="mqtt_securacv",
    )
    flow = make_flow(entries=[existing])
    result = drive(
        flow.async_step_both(
            {
                CONF_URL: "http://somewhere-else:8799",
                CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
                CONF_MQTT_PREFIX: "securacv",
            }
        )
    )
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


def test_mqtt_config_aborts_on_prefix_covered_by_both_entry():
    existing = make_entry(
        {
            CONF_URL: "http://kernel:8799",
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: "securacv",
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        },
        unique_id="http://kernel:8799",
    )
    flow = make_flow(entries=[existing])
    result = drive(flow.async_step_mqtt_config({CONF_MQTT_PREFIX: "securacv"}))
    assert result["type"] == "abort"
    assert result["reason"] == "already_configured"


# ---------------------------------------------------------------------------
# Error path preserves typed input
# ---------------------------------------------------------------------------


def test_kernel_error_rebuilds_form_with_typed_values(monkeypatch):
    async def fake_validate(hass, data):
        raise CannotConnect

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

    flow = make_flow()
    typed = {
        CONF_URL: "http://my-kernel.lan:9999",
        CONF_TOKEN_FILE: "/share/securacv/api_token",
        CONF_TOKEN: "abc123",
    }
    result = drive(flow.async_step_kernel(dict(typed)))

    assert result["type"] == "form"
    assert result["errors"] == {"base": "cannot_connect"}
    defaults = schema_defaults(result)
    assert defaults[CONF_URL] == "http://my-kernel.lan:9999"
    assert defaults[CONF_TOKEN_FILE] == "/share/securacv/api_token"
    assert defaults[CONF_TOKEN] == "abc123"


def test_both_error_rebuilds_form_with_typed_values(monkeypatch):
    async def fake_validate(hass, data):
        raise InvalidAuth

    monkeypatch.setattr(config_flow, "_async_validate_kernel", fake_validate)

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


def test_kernel_fresh_form_uses_static_defaults():
    flow = make_flow()
    result = drive(flow.async_step_kernel(None))
    assert result["type"] == "form"
    defaults = schema_defaults(result)
    assert defaults[CONF_URL] == DEFAULT_KERNEL_URL
    assert defaults[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE


# ---------------------------------------------------------------------------
# Manual modes still route
# ---------------------------------------------------------------------------


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
