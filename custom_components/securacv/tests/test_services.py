"""The securacv.* actions — watches, the chain walk and the evidence export.

Five actions and no more: ``start_watch``, ``end_watch``, ``list_watches``
(response only), ``verify`` and ``export_evidence``. Before the watch
actions nothing could end a watch early, although the design says ending
"happens on an authenticated surface" and the voice answer at the cap told
people to "end one on the dashboard", which had no such control. Pin /
rotate / unpin are deliberately NOT actions (docs/device_trust.md, "Why
pin, rotate and unpin are not actions"): an automation reacting to a
key-mismatch notification must not be able to rotate the pin to the key it
just saw. ``verify`` and ``export_evidence`` only read: the walk against
the pinned key, and the kernel's signed bundle into a pack.

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

import asyncio
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
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError  # noqa: E402

from .. import (  # noqa: E402
    async_setup,
    async_setup_entry,
    async_unload_entry,
    services,
    watch_runtime,
    watches,
)
from ..api import (  # noqa: E402
    BundleDownload,
    SecuraCVApiAuthError,
    SecuraCVApiConnectionError,
    SecuraCVApiResponseError,
)
from ..const import (  # noqa: E402
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_SETUP_MODE,
    DOMAIN,
    SETUP_MODE_BOTH,
    SETUP_MODE_MQTT,
    VERIFY_TRIGGER_SERVICE,
)
from ..data import VerificationState  # noqa: E402
from .conftest import make_entry, make_runtime  # noqa: E402

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

    async def _unload(entry, platforms):
        return True

    hass.config_entries.add(ENTRY)
    assert run(async_setup(hass, {})) is True
    if with_entry:
        assert run(async_setup_entry(hass, ENTRY)) is True
        ENTRY.runtime_data.devices.update(DEVICES)
    return hass


def _call(hass, service: str, data: dict | None = None, *, call_cls=None):
    """Call a registered handler the way HA does. The conftest ServiceCall
    is the pre-2025.1 shape (no ``.hass``), the oldest this integration
    supports, so every test here would fail on a handler that reads it.
    The kernel actions are coroutines (they do I/O); those are driven."""
    registered = hass.services.registered[(DOMAIN, service)]
    call_cls = call_cls or ServiceCall
    result = registered.func(call_cls(DOMAIN, service, data or {}, return_response=True))
    if asyncio.iscoroutine(result):
        result = run(result)
    return result


# ── a kernel entry for the verify / export actions ──────────────────────

KERNEL_DEVICE_ID = "pwk_1a2b3c4d"
KERNEL_KEY = "ab" * 32


class FakeVerifier:
    """Stands in for kernel_verify.KernelVerifier: answers, or raises."""

    def __init__(self, *, state=None, error=None) -> None:
        self.state = state or VerificationState(
            ok=True, verified_seq=3, verified_entries=3, verified_bucket_start=1_700_000_400,
            walk_entries=2, head_seq=3,
        )
        self.error = error
        self.triggers: list[str] = []

    async def async_verify(self, trigger: str):
        self.triggers.append(trigger)
        if self.error is not None:
            raise self.error
        return self.state


class FakeKernelApi:
    """A kernel API for the export path: a canned bundle, or an error."""

    def __init__(self, *, error=None, bundle: bytes | None = None) -> None:
        self.error = error
        self.bundle = bundle if bundle is not None else json.dumps(
            {"artifact": {"batches": [{"buckets": [{"events": [{}, {}]}]}]}}
        ).encode()
        self.windows: list[int] = []

    async def async_download_export_bundle(self, window_hours: int) -> BundleDownload:
        self.windows.append(window_hours)
        if self.error is not None:
            raise self.error
        return BundleDownload(filename="securacv-events-1700000400.json", content=self.bundle)


def _kernel_hass(*entries_spec, verifier=None, api=None) -> HomeAssistant:
    """A Home Assistant with securacv's actions registered and `n` kernel
    entries (default one), each loaded with a fake coordinator, verifier and
    API. `verifier` / `api` apply to the first entry."""
    hass = HomeAssistant()
    hass.data = {}
    assert run(async_setup(hass, {})) is True
    specs = list(entries_spec) or ["k1"]
    for index, entry_id in enumerate(specs):
        coordinator = types.SimpleNamespace(
            pinned_key=KERNEL_KEY, kernel_available=True, key_mismatch=False, data={}
        )
        runtime = make_runtime(
            hass,
            entry_id,
            setup_mode=SETUP_MODE_BOTH,
            coordinator=coordinator,
            api=(api if index == 0 and api is not None else FakeKernelApi()),
            verifier=(verifier if index == 0 and verifier is not None else FakeVerifier()),
        )
        make_entry(
            hass,
            entry_id,
            data={CONF_SETUP_MODE: SETUP_MODE_BOTH, CONF_KERNEL_DEVICE_ID: KERNEL_DEVICE_ID},
            runtime=runtime,
        )
    return hass


def _bucket(hass) -> list:
    return hass.data[DOMAIN]["watches"]


def _stored(hass) -> list:
    """What the conftest Store stub's delayed save wrote through."""
    payload = hass.data[DOMAIN]["_watch_store"]._payload or {}
    return payload.get("watches", [])


# ── Registration ────────────────────────────────────────────────────────


def test_async_setup_registers_exactly_the_six_actions() -> None:
    hass = _hass(with_entry=False)
    assert set(hass.services.registered) == {
        (DOMAIN, "start_watch"),
        (DOMAIN, "end_watch"),
        (DOMAIN, "list_watches"),
        (DOMAIN, "verify"),
        (DOMAIN, "staged_update"),
        (DOMAIN, "export_evidence"),
    }
    registered = hass.services.registered
    assert registered[(DOMAIN, "start_watch")].supports_response == SupportsResponse.OPTIONAL
    assert registered[(DOMAIN, "end_watch")].supports_response == SupportsResponse.OPTIONAL
    assert registered[(DOMAIN, "list_watches")].supports_response == SupportsResponse.ONLY
    assert registered[(DOMAIN, "verify")].supports_response == SupportsResponse.OPTIONAL
    assert registered[(DOMAIN, "export_evidence")].supports_response == SupportsResponse.OPTIONAL
    assert registered[(DOMAIN, "start_watch")].schema is services.START_WATCH_SCHEMA
    assert registered[(DOMAIN, "end_watch")].schema is services.END_WATCH_SCHEMA
    assert registered[(DOMAIN, "list_watches")].schema is services.LIST_WATCHES_SCHEMA
    assert registered[(DOMAIN, "verify")].schema is services.VERIFY_SCHEMA
    assert registered[(DOMAIN, "export_evidence")].schema is services.EXPORT_EVIDENCE_SCHEMA
    # The kernel actions do I/O: coroutine handlers, never @callback.
    assert asyncio.iscoroutinefunction(registered[(DOMAIN, "verify")].func)
    assert asyncio.iscoroutinefunction(registered[(DOMAIN, "export_evidence")].func)


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


def test_the_vocabulary_is_the_six_and_nothing_else_registers_actions() -> None:
    """Action names are a compatibility promise, and trust decisions are
    not automatable: pin / rotate / unpin stay in the options flow and the
    Repairs fix flow; verify and export only read; staged_update installs
    firmware each node's own signed engine already accepts (docs/OTA.md §5)
    and touches no trust. A new registration anywhere else in the package
    (button.py included — the buttons call the same coroutines) would bypass
    this list."""
    assert services.SERVICES == (
        "start_watch", "end_watch", "list_watches", "verify", "export_evidence",
        "staged_update",
    )
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


class _CallBefore2025_1:
    """homeassistant.core.ServiceCall as cores through 2024.12 defined
    it: slotted, and no ``hass``. Below hacs.json's floor now, kept so
    the handlers provably never read ``call.hass``."""

    __slots__ = ("domain", "service", "data", "context", "return_response")

    def __init__(self, domain, service, data=None, context=None, return_response=False):
        self.domain, self.service, self.data = domain, service, dict(data or {})
        self.context, self.return_response = context, return_response


class _CallFrom2025_1(_CallBefore2025_1):
    """...and as 2025.1 onward define it, with ``hass`` added. The handler
    must ignore it: this one points at a different hub, so a handler that
    read it would act on the wrong instance and the test would see it."""

    __slots__ = ("hass",)

    def __init__(self, domain, service, data=None, context=None, return_response=False):
        super().__init__(domain, service, data, context, return_response)
        self.hass = HomeAssistant()


@pytest.mark.parametrize("call_cls", [_CallBefore2025_1, _CallFrom2025_1])
def test_every_action_works_with_either_service_call_shape(call_cls, delivered) -> None:
    """The handlers get ``hass`` from registration, never from the call:
    ServiceCall gained ``.hass`` only in Home Assistant 2025.1, and before
    that every securacv.* call raised AttributeError."""
    hass = _hass()
    started = _call(hass, "start_watch", {"subject": "the gate canary"}, call_cls=call_cls)
    assert [w["id"] for w in _bucket(hass)] == [started["id"]]
    listed = _call(hass, "list_watches", call_cls=call_cls)
    assert [row["id"] for row in listed["watches"]] == [started["id"]]
    ended = _call(hass, "end_watch", {"watch": started["id"]}, call_cls=call_cls)
    assert ended["id"] == started["id"]
    assert _bucket(hass) == []


def test_no_handler_reads_hass_off_the_call() -> None:
    """The source-level half of the guard above, for a handler path the
    behavioral test does not reach."""
    module = ast.parse((PACKAGE_DIR / "services.py").read_text(encoding="utf-8"))
    reads = [
        node.lineno
        for node in ast.walk(module)
        if isinstance(node, ast.Attribute) and node.attr == "hass"
    ]
    assert reads == [], f"services.py reads .hass on lines {reads}"


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


@pytest.mark.parametrize("duration", ["48 hours", "until Sunday", "14", "forever"])
def test_a_duration_the_parser_cannot_read_is_refused_not_defaulted(duration) -> None:
    """An automation is typed: a silent 14 days for "48 hours" would be a
    watch nobody asked for. Voice keeps its forgiving default."""
    hass = _hass()
    with pytest.raises(ServiceValidationError, match="days, weeks, months"):
        _call(hass, "start_watch", {"subject": "the gate canary", "duration": duration})
    assert _bucket(hass) == []

    _start(hass, "the gate canary", duration)
    (spoken,) = _bucket(hass)
    assert spoken["ends_at"] - spoken["started_at"] == watches.DEFAULT_DAYS * DAY


@pytest.mark.parametrize(
    ("duration", "days"),
    [("", watches.DEFAULT_DAYS), ("   ", watches.DEFAULT_DAYS), ("a fortnight", 14),
     ("3 nights", 3), ("two-weeks", 14), ("a year", 365), ("500 days", 365)],
)
def test_a_readable_duration_is_taken_and_a_blank_one_is_the_default(duration, days) -> None:
    hass = _hass()
    row = _call(hass, "start_watch", {"subject": "the gate canary", "duration": duration})
    assert row["ends_at"] - row["started_at"] == days * DAY


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


def test_every_action_the_speech_names_is_a_registered_action() -> None:
    """Speech that sends a person somewhere must send them somewhere real:
    the answer at the cap names securacv.end_watch, and a roster too long
    to read out names securacv.list_watches (HA10: it named a dashboard
    that lists no watches). Every string literal in the speech modules is
    read, so a renamed or invented action is caught wherever it is said."""
    named: set[str] = set()
    for module in ("watches.py", "intent.py", "voice.py"):
        tree = ast.parse((PACKAGE_DIR / module).read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                named |= set(re.findall(rf"\b{DOMAIN}\.([a-z_]+)", node.value))
    assert {services.SERVICE_END_WATCH, services.SERVICE_LIST_WATCHES} <= named
    assert named <= set(services.SERVICES), sorted(named - set(services.SERVICES))


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
    assert message.startswith("The gate canary watch ended"), message


@pytest.mark.parametrize("ref", ["the gate canary", "The Gate Canary", "gate canary", "  my gate   canary "])
def test_end_watch_by_label_ignores_case_spacing_and_the_article(ref, delivered) -> None:
    hass = _hass()
    _call(hass, "start_watch", {"subject": "the gate canary"})
    _call(hass, "start_watch", {"subject": "the back door"})
    _call(hass, "end_watch", {"watch": ref})
    assert [w["label"] for w in _bucket(hass)] == ["the back door"]


@pytest.mark.parametrize(
    ("said", "label"),
    [
        ("The gate canary", "the gate canary"),
        ("THE gate canary", "the gate canary"),
        ("My gate canary", "the gate canary"),
        ("the the gate canary", "the gate canary"),
        ("The my gate canary", "the gate canary"),
        ("  the   gate  canary ", "the gate canary"),
        ("The Gate Canary", "the Gate Canary"),
        ("gate canary", "the gate canary"),
        ("theater lights", "the theater lights"),
    ],
)
def test_a_watch_ends_by_the_words_it_was_started_with(said, label, delivered) -> None:
    """The label and the match key come from one normalization: the
    leading article is dropped in any case and however often it was said,
    so the label never doubles it ("the The gate canary") and ending by the
    same text always finds the watch. Voice and the action agree."""
    hass = _hass()
    started = _call(hass, "start_watch", {"subject": said})
    assert started["label"] == label
    assert not re.match(r"(the|my) (the|my)\b", label, re.IGNORECASE), label

    spoken = _hass()
    _start(spoken, said)
    assert _bucket(spoken)[0]["label"] == label, "voice and the action label alike"

    ended = _call(hass, "end_watch", {"watch": said})
    assert ended["id"] == started["id"]
    assert _bucket(hass) == []


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


def test_every_action_refuses_once_no_entry_runs_the_tick(delivered) -> None:
    """The actions outlive their entries (registered once, in async_setup),
    and the restore flag outlives them too. The tick does not: it is
    canceled with the entry that scheduled it. After the last entry
    unloads, a start would record a watch that nothing evaluates, delivers
    or expires, so every action refuses by name until an entry is back."""
    hass = _hass()
    _call(hass, "start_watch", {"subject": "the gate canary"})
    assert run(async_unload_entry(hass, ENTRY)) is True
    assert watch_runtime.watches_restored(hass), "the restore flag stays set"
    assert not watch_runtime.watches_hosted(hass)
    for service, data in (
        ("start_watch", {"subject": "the back door"}),
        ("end_watch", {"watch": "the gate canary"}),
        ("list_watches", {}),
    ):
        with pytest.raises(ServiceValidationError, match="no loaded entry"):
            _call(hass, service, data)
    assert [w["label"] for w in _bucket(hass)] == ["the gate canary"], "nothing changed"

    assert run(async_setup_entry(hass, ENTRY)) is True
    ENTRY.runtime_data.devices.update(DEVICES)
    rows = _call(hass, "list_watches")["watches"]
    assert [row["label"] for row in rows] == ["the gate canary"], "the roster is still there"


def test_one_entry_unloading_leaves_the_other_s_tick_hosting() -> None:
    hass = _hass()
    other = types.SimpleNamespace(entry_id="e2", data=dict(ENTRY.data))
    hass.config_entries.add(other)
    assert run(async_setup_entry(hass, other)) is True
    assert run(async_unload_entry(hass, ENTRY)) is True
    assert watch_runtime.watches_hosted(hass)
    _call(hass, "start_watch", {"subject": "the gate canary"})
    assert len(_bucket(hass)) == 1
    assert run(async_unload_entry(hass, other)) is True
    with pytest.raises(ServiceValidationError, match="no loaded entry"):
        _call(hass, "list_watches")


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
        "verify": services.VERIFY_SCHEMA,
        "export_evidence": services.EXPORT_EVIDENCE_SCHEMA,
        "staged_update": services.STAGED_UPDATE_SCHEMA,
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


def test_export_evidence_declares_the_config_entry_selector_and_the_window() -> None:
    """The two kernel actions target a kernel by config entry (the selector
    the frontend renders as a picker) and the export window is bounded in
    the file the frontend reads, not only in the handler."""
    text = (PACKAGE_DIR / "services.yaml").read_text(encoding="utf-8")
    export = text[text.index("\nexport_evidence:") :]
    assert "config_entry:" in export and "integration: securacv" in export
    assert re.search(r"min: 1\n\s+max: 720", export)
    verify = text[text.index("\nverify:") : text.index("\nexport_evidence:")]
    assert "config_entry:" in verify and "integration: securacv" in verify


# ── verify ───────────────────────────────────────────────────────────────


def test_verify_runs_the_entry_s_walk_and_shapes_the_response() -> None:
    verifier = FakeVerifier()
    hass = _kernel_hass(verifier=verifier)
    response = _call(hass, "verify")
    assert verifier.triggers == [VERIFY_TRIGGER_SERVICE]
    assert response["chain_valid"] is True
    assert response["events_verified"] == 2 and response["verified_entries"] == 3
    assert response["verified_seq"] == 3
    assert response["verified_bucket_start"] == 1_700_000_400
    assert response["verified_bucket_size"] == 600
    assert response["kernel_device_id"] == KERNEL_DEVICE_ID
    assert response["error"] is None and response["key_mismatch"] is False
    assert "2 new entries verified" in response["verdict"]
    json.dumps(response)


def test_verify_targets_the_named_entry_when_several_have_a_kernel() -> None:
    first = FakeVerifier()
    hass = _kernel_hass("k1", "k2", verifier=first)
    with pytest.raises(ServiceValidationError, match="say which one"):
        _call(hass, "verify")
    response = _call(hass, "verify", {"entry_id": "k1"})
    assert first.triggers == [VERIFY_TRIGGER_SERVICE]
    assert response["chain_valid"] is True
    with pytest.raises(ServiceValidationError, match="not loaded"):
        _call(hass, "verify", {"entry_id": "k9"})


def test_verify_without_any_kernel_entry_refuses_by_name() -> None:
    hass = _hass()  # the mqtt-only entry: no coordinator
    with pytest.raises(ServiceValidationError, match="no sealed log"):
        _call(hass, "verify")


def test_verify_reports_a_failed_walk_as_a_response_not_an_error() -> None:
    state = VerificationState(ok=False, error="sealed_events integrity check failed at index 1: "
                              "prev_hash mismatch", failed_seq=2, verified_seq=1)
    hass = _kernel_hass(verifier=FakeVerifier(state=state))
    response = _call(hass, "verify")
    assert response["chain_valid"] is False
    assert "prev_hash mismatch" in response["verdict"]
    assert response["error"] == state.error


@pytest.mark.parametrize(
    ("error", "match"),
    [
        (SecuraCVApiConnectionError("unable to reach API"), "did not answer"),
        (SecuraCVApiAuthError("unauthorized"), "rejected Home Assistant"),
        (SecuraCVApiResponseError("unexpected status: 500"), "unexpected status: 500"),
    ],
)
def test_verify_translates_kernel_failures(error, match) -> None:
    hass = _kernel_hass(verifier=FakeVerifier(error=error))
    with pytest.raises(HomeAssistantError, match=match) as info:
        _call(hass, "verify")
    assert not isinstance(info.value, ServiceValidationError), "a failure, not a refusal"
    assert info.value.translation_domain == DOMAIN


# ── export_evidence ──────────────────────────────────────────────────────


def test_export_evidence_defaults_to_24_hours_and_returns_the_pack(monkeypatch) -> None:
    api = FakeKernelApi()
    hass = _kernel_hass(api=api)
    response = _call(hass, "export_evidence")
    assert api.windows == [24]
    assert set(response) == {
        "export_id", "file", "url", "sha256", "events", "window_hours", "kernel_device_id"
    }
    assert re.fullmatch(r"[0-9a-f]{8}", response["export_id"])
    assert response["file"].endswith(f"-{response['export_id']}.zip")
    assert response["url"].startswith(f"/api/securacv/evidence/{response['export_id']}?authSig=")
    assert response["events"] == 2 and response["window_hours"] == 24
    assert response["kernel_device_id"] == KERNEL_DEVICE_ID
    json.dumps(response)


def test_export_evidence_takes_the_window_and_bounds_it() -> None:
    api = FakeKernelApi()
    hass = _kernel_hass(api=api)
    assert _call(hass, "export_evidence", {"window_hours": 168})["window_hours"] == 168
    assert _call(hass, "export_evidence", {"window_hours": "72"})["window_hours"] == 72
    assert api.windows == [168, 72]
    for bad in (0, 721, "soon", True, -5):
        with pytest.raises(ServiceValidationError, match="whole number of hours"):
            _call(hass, "export_evidence", {"window_hours": bad})
    assert api.windows == [168, 72], "a refused window never reaches the kernel"


@pytest.mark.parametrize(
    ("error", "match"),
    [
        (SecuraCVApiConnectionError("unable to reach API"), "did not answer"),
        (SecuraCVApiAuthError("unauthorized"), "rejected Home Assistant"),
        (SecuraCVApiResponseError("export bundle too large: 9 bytes"), "too large"),
    ],
)
def test_export_evidence_translates_kernel_failures(error, match) -> None:
    hass = _kernel_hass(api=FakeKernelApi(error=error))
    with pytest.raises(HomeAssistantError, match=match) as info:
        _call(hass, "export_evidence")
    assert not isinstance(info.value, ServiceValidationError)


def test_export_evidence_translates_a_write_failure(monkeypatch) -> None:
    from .. import evidence

    def _boom(*args, **kwargs):
        raise OSError("disk full")

    monkeypatch.setattr(evidence, "compose_export", _boom)
    hass = _kernel_hass()
    with pytest.raises(HomeAssistantError, match="disk full") as info:
        _call(hass, "export_evidence")
    assert info.value.translation_key == "export_write_failed"
