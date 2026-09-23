"""The securacv.* actions — the automation surface for watches.

Three actions and no more: ``start_watch``, ``end_watch`` and
``list_watches`` (response only). Before them nothing could end a watch
early, although the design says ending "happens on an authenticated
surface" and the voice answer at the cap told people to "end one on the
dashboard", which had no such control. Pin / rotate / unpin are
deliberately NOT actions (docs/device_trust.md, "Why pin, rotate and
unpin are not actions"): an automation reacting to a key-mismatch
notification must not be able to rotate the pin to the key it just saw.

What is pinned here:
  - registration: exactly the three, once, from ``async_setup``, never fatal;
  - one start path: the action builds the same watch the voice intent does;
  - ending: by id, then by label (case and the leading article do not
    count), refusing an unknown or ambiguous reference, announced;
  - listing: the response shape, and that an expired watch is announced
    rather than listed;
  - the cap, and the refusal before the restore has run;
  - services.yaml, strings.json and the voluptuous schemas name the same
    actions and fields (hassfest checks the files' shape in CI; nothing
    else holds the three together).
"""

from __future__ import annotations

import ast
import json
import logging
import re
import time
import types
from pathlib import Path

import pytest

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run

# Installs the intent-platform stubs at import, and lends the voice path so
# the shared-start-path test compares against a watch a person started.
from .test_intent_start_watch import DEVICES, GATE, _start  # noqa: E402

from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse  # noqa: E402
from homeassistant.exceptions import ServiceValidationError  # noqa: E402

from .. import async_setup, async_setup_entry, services, watch_runtime, watches  # noqa: E402
from ..const import (  # noqa: E402
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    DOMAIN,
    SETUP_MODE_MQTT,
)

PACKAGE_DIR = Path(__file__).resolve().parent.parent
NOW = 1_700_000_000.0
DAY = watches.DAY

ENTRY = types.SimpleNamespace(
    entry_id="e1",
    data={CONF_SETUP_MODE: SETUP_MODE_MQTT, CONF_ENABLE_MQTT: False},
)


@pytest.fixture(autouse=True)
def _frozen_clock(monkeypatch):
    """Every module here reads ``time.time()``; one frozen value lets the
    voice-started and the action-started watch be compared field for field."""
    monkeypatch.setattr(time, "time", lambda: NOW)


@pytest.fixture
def delivered(monkeypatch) -> list[tuple[str, str]]:
    notes: list[tuple[str, str]] = []
    monkeypatch.setattr(
        watch_runtime, "_notify",
        lambda _hass, title, message, _nid: notes.append((title, message)),
    )
    return notes


def _hass(*, with_entry: bool = True) -> HomeAssistant:
    """A Home Assistant start through the integration's real async_setup
    and (optionally) async_setup_entry, which is where watches are restored."""
    hass = HomeAssistant()
    hass.data = {}

    async def _forward(entry, platforms):
        return True

    hass.config_entries = types.SimpleNamespace(async_forward_entry_setups=_forward)
    assert run(async_setup(hass, {})) is True
    if with_entry:
        assert run(async_setup_entry(hass, ENTRY)) is True
        hass.data[DOMAIN]["e1"]["devices"] = dict(DEVICES)
    return hass


def _call(hass, service: str, data: dict | None = None):
    registered = hass.services.registered[(DOMAIN, service)]
    return registered.func(
        ServiceCall(hass, DOMAIN, service, data or {}, return_response=True)
    )


def _bucket(hass) -> list:
    return hass.data[DOMAIN]["watches"]


def _stored(hass) -> list:
    """What the conftest Store stub's delayed save wrote through."""
    payload = hass.data[DOMAIN]["_watch_store"]._payload or {}
    return payload.get("watches", [])


# ── Registration ────────────────────────────────────────────────────────


def test_async_setup_registers_exactly_the_three_watch_actions() -> None:
    hass = _hass(with_entry=False)
    assert set(hass.services.registered) == {
        (DOMAIN, "start_watch"),
        (DOMAIN, "end_watch"),
        (DOMAIN, "list_watches"),
    }
    registered = hass.services.registered
    assert registered[(DOMAIN, "start_watch")].supports_response == SupportsResponse.OPTIONAL
    assert registered[(DOMAIN, "end_watch")].supports_response == SupportsResponse.OPTIONAL
    assert registered[(DOMAIN, "list_watches")].supports_response == SupportsResponse.ONLY
    assert registered[(DOMAIN, "start_watch")].schema is services.START_WATCH_SCHEMA
    assert registered[(DOMAIN, "end_watch")].schema is services.END_WATCH_SCHEMA
    assert registered[(DOMAIN, "list_watches")].schema is services.LIST_WATCHES_SCHEMA


def test_registration_happens_once() -> None:
    hass = _hass(with_entry=False)
    first = dict(hass.services.registered)
    run(async_setup(hass, {}))
    services.async_setup_services(hass)
    assert hass.services.registered.keys() == first.keys()
    for key, registered in first.items():
        assert hass.services.registered[key] is registered, f"{key} was registered twice"


def test_a_failure_to_register_never_fails_setup(monkeypatch, caplog) -> None:
    def _boom(hass):
        raise RuntimeError("registry unavailable")

    monkeypatch.setattr(services, "async_setup_services", _boom)
    hass = HomeAssistant()
    with caplog.at_level(logging.WARNING):
        assert run(async_setup(hass, {})) is True
    assert hass.services.registered == {}
    assert any("actions not registered" in r.getMessage() for r in caplog.records)


def test_the_vocabulary_is_watches_only_and_nothing_else_registers_actions() -> None:
    """Action names are a compatibility promise, and trust decisions are
    not automatable: pin / rotate / unpin stay in the options flow. A new
    registration anywhere else in the package would bypass this list."""
    assert services.SERVICES == ("start_watch", "end_watch", "list_watches")
    for name in services.SERVICES:
        assert not re.search(r"pin|rotate|trust|key", name), name

    registering = sorted(
        path.name
        for path in PACKAGE_DIR.glob("*.py")
        if re.search(
            r"services\.(async_)?register\b|async_register_entity_service|async_register_admin_service",
            path.read_text(encoding="utf-8"),
        )
    )
    assert registering == ["services.py"]


def test_config_schema_is_declared_because_async_setup_exists() -> None:
    """hassfest's config_schema rule, held here too (hassfest is CI-only):
    an integration with ``async_setup`` must declare CONFIG_SCHEMA."""
    module = ast.parse((PACKAGE_DIR / "__init__.py").read_text(encoding="utf-8"))
    functions = {
        node.name for node in module.body if isinstance(node, ast.AsyncFunctionDef)
    }
    assigned = {
        target.id
        for node in module.body
        if isinstance(node, ast.Assign)
        for target in node.targets
        if isinstance(target, ast.Name)
    }
    assert "async_setup" in functions
    assert "CONFIG_SCHEMA" in assigned


# ── start_watch ─────────────────────────────────────────────────────────


def test_start_watch_builds_the_same_watch_the_voice_intent_does() -> None:
    spoken = _hass()
    _start(spoken, "the gate canary", "two weeks")
    automated = _hass()
    row = _call(automated, "start_watch", {"subject": "the gate canary", "duration": "two weeks"})

    assert _bucket(automated) == _bucket(spoken)
    watch = _bucket(automated)[0]
    assert watch["subject"] == {"kind": "event", "ref": GATE}
    assert watch["ends_at"] - watch["started_at"] == 14 * DAY
    assert row == services.watch_row(watch, NOW)
    assert row["id"] == watch["id"]
    assert "observations" not in row, "a diary is not a response"


def test_start_watch_is_persisted_like_a_spoken_one() -> None:
    hass = _hass()
    _call(hass, "start_watch", {"subject": "the gate canary"})
    assert [w["id"] for w in _stored(hass)] == [w["id"] for w in _bucket(hass)]


def test_start_watch_takes_a_concern_and_otherwise_reads_the_words() -> None:
    hass = _hass()
    stated = _call(hass, "start_watch", {"subject": "the gate canary", "concern": "stopped"})
    defaulted = _call(hass, "start_watch", {"subject": "the back door"})
    assert stated["concern"] == watches.CONCERN_STOPPED
    assert defaulted["concern"] == watches.CONCERN_UNUSUAL
    assert defaulted["ends_at"] - defaulted["started_at"] == watches.DEFAULT_DAYS * DAY


def test_an_unbound_subject_is_accepted_and_the_log_says_it_cannot_fire(caplog) -> None:
    hass = _hass()
    with caplog.at_level(logging.WARNING, logger=services._LOGGER.name):
        row = _call(hass, "start_watch", {"subject": "the litter box"})
    assert row["subject"] == {"kind": "unbound", "ref": "the litter box"}
    assert any("nothing in the fleet reports" in r.getMessage() for r in caplog.records)


def test_start_watch_refuses_an_empty_subject() -> None:
    hass = _hass()
    with pytest.raises(ServiceValidationError):
        _call(hass, "start_watch", {"subject": "   "})
    assert _bucket(hass) == []


def test_the_cap_is_honored_and_both_surfaces_name_end_watch() -> None:
    hass = _hass()
    for i in range(watches.MAX_WATCHES):
        _call(hass, "start_watch", {"subject": f"thing {i}"})
    assert len(_bucket(hass)) == watches.MAX_WATCHES

    with pytest.raises(ServiceValidationError, match="securacv.end_watch"):
        _call(hass, "start_watch", {"subject": "one more"})
    assert len(_bucket(hass)) == watches.MAX_WATCHES

    # The spoken refusal points at the same action, not at a dashboard
    # control that does not exist.
    response = _start(hass, "one more")
    assert "securacv.end_watch" in response.speech
    assert "dashboard" not in response.speech
    assert len(_bucket(hass)) == watches.MAX_WATCHES


def test_ids_stay_unique_after_an_end_in_the_same_second() -> None:
    hass = _hass()
    first = _call(hass, "start_watch", {"subject": "the gate canary"})
    second = _call(hass, "start_watch", {"subject": "the back door"})
    _call(hass, "end_watch", {"watch": first["id"]})
    third = _call(hass, "start_watch", {"subject": "the shed"})
    ids = [w["id"] for w in _bucket(hass)]
    assert len(set(ids)) == len(ids) == 2
    assert third["id"] != second["id"]


# ── end_watch ───────────────────────────────────────────────────────────


def test_end_watch_by_id_removes_exactly_that_watch_and_announces_it(delivered) -> None:
    hass = _hass()
    gate = _call(hass, "start_watch", {"subject": "the gate canary"})
    back = _call(hass, "start_watch", {"subject": "the back door"})

    ended = _call(hass, "end_watch", {"watch": gate["id"]})

    assert [w["id"] for w in _bucket(hass)] == [back["id"]]
    assert [w["id"] for w in _stored(hass)] == [back["id"]], "the end is persisted"
    assert ended["id"] == gate["id"]
    assert ended["state"] == watches.STATE_ENDED
    assert ended["ends_at"] == NOW
    assert ended["days_left"] == 0
    # A watch that ends says so, even when someone asked for the end.
    assert len(delivered) == 1
    title, message = delivered[0]
    assert title == "SecuraCV: a watch was ended early"
    assert "the gate canary" in message


@pytest.mark.parametrize("ref", ["the gate canary", "The Gate Canary", "gate canary", "  my gate   canary "])
def test_end_watch_by_label_ignores_case_spacing_and_the_article(ref, delivered) -> None:
    hass = _hass()
    _call(hass, "start_watch", {"subject": "the gate canary"})
    _call(hass, "start_watch", {"subject": "the back door"})
    _call(hass, "end_watch", {"watch": ref})
    assert [w["label"] for w in _bucket(hass)] == ["the back door"]


def test_an_ambiguous_label_is_refused_and_an_id_still_works(delivered) -> None:
    hass = _hass()
    first = _call(hass, "start_watch", {"subject": "the gate canary"})
    second = _call(hass, "start_watch", {"subject": "the gate canary", "concern": "stopped"})

    with pytest.raises(ServiceValidationError, match="end it by id"):
        _call(hass, "end_watch", {"watch": "the gate canary"})
    assert len(_bucket(hass)) == 2, "ending is the silencing direction: never guessed"
    assert delivered == []

    _call(hass, "end_watch", {"watch": second["id"]})
    assert [w["id"] for w in _bucket(hass)] == [first["id"]]


def test_an_unknown_watch_is_refused(delivered) -> None:
    hass = _hass()
    _call(hass, "start_watch", {"subject": "the gate canary"})
    for ref in ("the shed", "w9-1", ""):
        with pytest.raises(ServiceValidationError):
            _call(hass, "end_watch", {"watch": ref})
    assert len(_bucket(hass)) == 1
    assert delivered == []


# ── list_watches ────────────────────────────────────────────────────────


def test_list_watches_returns_every_running_watch() -> None:
    hass = _hass()
    assert _call(hass, "list_watches") == {"watches": []}
    gate = _call(hass, "start_watch", {"subject": "the gate canary", "duration": "10 days"})
    _call(hass, "start_watch", {"subject": "the litter box"})

    response = _call(hass, "list_watches")
    assert set(response) == {"watches"}
    rows = response["watches"]
    assert [row["id"] for row in rows] == [w["id"] for w in _bucket(hass)]
    assert rows[0] == gate
    assert rows[0]["days_left"] == 10
    assert rows[0]["state"] == watches.STATE_SETTLING
    assert rows[1]["subject"]["kind"] == "unbound"
    json.dumps(response)  # a response must be plain data


def test_list_watches_announces_an_expired_watch_instead_of_listing_it(delivered) -> None:
    hass = _hass()
    _bucket(hass).append(
        watches.make_watch("w-old", "the shed", {"kind": "event", "ref": GATE}, NOW - 3 * DAY, days=1)
    )
    _call(hass, "start_watch", {"subject": "the gate canary"})

    rows = _call(hass, "list_watches")["watches"]
    assert [row["label"] for row in rows] == ["the gate canary"]
    assert [title for title, _m in delivered] == ["SecuraCV: a watch ended"]
    assert [w["label"] for w in _bucket(hass)] == ["the gate canary"]


# ── Before the restore ──────────────────────────────────────────────────


def test_every_action_refuses_until_the_integration_has_loaded() -> None:
    """Before the restore the bucket is not the truth: a start would be
    written over by the rows about to be read back, a list would say
    "none" about watches that exist. So the action says so instead."""
    hass = _hass(with_entry=False)
    for service, data in (
        ("start_watch", {"subject": "the gate canary"}),
        ("end_watch", {"watch": "the gate canary"}),
        ("list_watches", {}),
    ):
        with pytest.raises(ServiceValidationError, match="not loaded yet"):
            _call(hass, service, data)
    assert "watches" not in hass.data.get(DOMAIN, {})


# ── The tick, around an announcement that could not be made ─────────────


def test_an_end_that_could_not_be_announced_is_retried_not_dropped(monkeypatch) -> None:
    """The expiry moved into the shared path (the tick and the listings
    both take it); the tick's old guarantee must hold through the move:
    an ending that could not be delivered keeps the watch for the next
    pass, and an ended watch never fires in the meantime."""
    hass = _hass()
    watch = watches.make_watch(
        "w-old", "the gate", {"kind": "event", "ref": GATE}, NOW - 3 * DAY,
        days=1, concern=watches.CONCERN_EVERY,
    )
    watch["observations"] = [[NOW - 3 * DAY + 60, 1.0]]
    _bucket(hass).append(watch)
    sent: list[str] = []
    endings_fail = True

    def _notify(_hass, title, _message, _nid):
        if endings_fail and title == "SecuraCV: a watch ended":
            raise RuntimeError("notifications unavailable")
        sent.append(title)

    monkeypatch.setattr(watch_runtime, "_notify", _notify)
    watch_runtime.async_tick(hass, NOW)
    assert _bucket(hass) == [watch], "kept, so the next pass can announce it"
    assert sent == [] and watch["fired"] == 0

    endings_fail = False
    watch_runtime.async_tick(hass, NOW + 300)
    assert sent == ["SecuraCV: a watch ended"]
    assert _bucket(hass) == []


# ── services.yaml, strings.json and the schemas agree ───────────────────


def _services_yaml() -> dict[str, dict[str, bool]]:
    """{action: {field: required}} from services.yaml, read by indentation
    (no yaml dependency in the test environment; the file's shape is
    hassfest's to validate)."""
    parsed: dict[str, dict[str, bool]] = {}
    action = field = None
    in_fields = False
    for line in (PACKAGE_DIR / "services.yaml").read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if m := re.fullmatch(r"([a-z_]+):", line):
            action, field, in_fields = m.group(1), None, False
            parsed[action] = {}
        elif line == "  fields:":
            in_fields = True
        elif in_fields and (m := re.fullmatch(r"    ([a-z_]+):", line)):
            field = m.group(1)
            parsed[action][field] = False
        elif in_fields and field and line.strip() == "required: true":
            parsed[action][field] = True
    return parsed


def _schema_fields(schema) -> dict[str, bool]:
    """{field: required} from the voluptuous stub's recorded markers."""
    return {
        marker.key: type(marker).__name__ == "_Required" for marker in schema.schema
    }


def test_services_yaml_strings_and_schemas_name_the_same_fields() -> None:
    declared = _services_yaml()
    strings = json.loads((PACKAGE_DIR / "strings.json").read_text(encoding="utf-8"))
    en = json.loads((PACKAGE_DIR / "translations" / "en.json").read_text(encoding="utf-8"))

    assert tuple(declared) == services.SERVICES
    assert tuple(strings["services"]) == services.SERVICES
    assert en["services"] == strings["services"]

    schemas = {
        "start_watch": services.START_WATCH_SCHEMA,
        "end_watch": services.END_WATCH_SCHEMA,
        "list_watches": services.LIST_WATCHES_SCHEMA,
    }
    for action in services.SERVICES:
        assert declared[action] == _schema_fields(schemas[action]), action
        assert set(strings["services"][action].get("fields", {})) == set(declared[action]), action
        assert strings["services"][action]["name"]
        assert strings["services"][action]["description"]


def test_the_concern_choices_are_the_engine_s_concerns() -> None:
    concern = services.START_WATCH_SCHEMA.schema[
        next(m for m in services.START_WATCH_SCHEMA.schema if m.key == "concern")
    ]
    assert tuple(concern.container) == watches.CONCERNS
    text = (PACKAGE_DIR / "services.yaml").read_text(encoding="utf-8")
    assert tuple(re.findall(r"- value: ([a-z]+)", text)) == watches.CONCERNS
