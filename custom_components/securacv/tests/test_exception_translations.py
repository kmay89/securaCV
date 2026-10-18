"""Every refusal the securacv.* actions raise is translatable, and reads
exactly as it did before it was.

The actions (services.py) refuse with ``ServiceValidationError``. Until
HA9 every refusal was a plain English string with no translation key, so
an install in another language read English errors. Now each one is
raised with ``translation_domain``, ``translation_key`` and
``translation_placeholders``, and the ``exceptions`` section of
strings.json (copied byte for byte to translations/en.json, which
test_entity_translations.py holds) declares every key.

Four properties, each one a way the two sides drift apart unnoticed:

  1. every ``ServiceValidationError`` / ``HomeAssistantError`` the package
     constructs carries a translation domain and key, and every key is a
     literal the scan can read;
  2. the keys raised and the keys declared are one set: a key raised but
     not declared renders as the bare key in a non-English frontend, and a
     key declared but never raised is a dead string a translator still
     translates;
  3. the English table in services.py is strings.json's text word for
     word, and each template renders with the placeholders it is raised
     with to exactly the message raised;
  4. every refusal, driven through the real handlers, reads to the user as
     it did before HA9: the messages are pinned here, one case per key.
"""

from __future__ import annotations

import ast
import json
import re
import time
import types
from pathlib import Path
from typing import Any, Callable, NamedTuple

import pytest

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run
from .test_services import (
    ENTRY,
    FakeKernelApi,
    FakeVerifier,
    _call,
    _hass,
    _kernel_hass,
)

from homeassistant.exceptions import HomeAssistantError, ServiceValidationError  # noqa: E402

from .. import async_unload_entry, evidence, services, watches  # noqa: E402
from ..api import (  # noqa: E402
    SecuraCVApiAuthError,
    SecuraCVApiConnectionError,
    SecuraCVApiResponseError,
)
from ..const import DOMAIN  # noqa: E402

PACKAGE_DIR = Path(services.__file__).resolve().parent
STRINGS = PACKAGE_DIR / "strings.json"
NOW = 1_700_000_000.0

# The classes a refusal is raised as. UpdateFailed (the coordinators in
# __init__.py) is also a HomeAssistantError, but it is not an action
# refusal and its messages are not translated yet (strategy/11,
# exception-translations).
_ERROR_CLASSES = frozenset({"ServiceValidationError", "HomeAssistantError"})


@pytest.fixture(autouse=True)
def _frozen_clock(monkeypatch):
    """Watch ids embed the clock; one frozen value pins them in a message."""
    monkeypatch.setattr(time, "time", lambda: NOW)


def _declared() -> dict[str, dict[str, str]]:
    return json.loads(STRINGS.read_text(encoding="utf-8"))["exceptions"]


# ─── 1 + 2. what the package raises, read off its source ──────────────


def _callee(node: ast.Call) -> str | None:
    if isinstance(node.func, ast.Name):
        return node.func.id
    if isinstance(node.func, ast.Attribute):
        return node.func.attr
    return None


def _scan_raised_keys() -> tuple[set[str], list[str]]:
    """(keys, problems) for every error the package constructs.

    A construction whose ``translation_key`` is a literal names its key
    directly. One whose key is a variable is a builder (services.py's
    ``_refusal``): every call to that builder must pass its key as a
    literal first argument, so this scan sees every key that can be
    raised without running anything.
    """
    keys: set[str] = set()
    problems: list[str] = []
    for path in sorted(PACKAGE_DIR.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        parents = {child: node for node in ast.walk(tree) for child in ast.iter_child_nodes(node)}
        builders: set[str] = set()
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call) or _callee(node) not in _ERROR_CLASSES:
                continue
            where = f"{path.name}:{node.lineno}"
            kwargs = {kw.arg: kw.value for kw in node.keywords}
            if "translation_domain" not in kwargs or "translation_key" not in kwargs:
                problems.append(f"{where} raises {_callee(node)} without a translation key")
                continue
            key = kwargs["translation_key"]
            if isinstance(key, ast.Constant) and isinstance(key.value, str):
                keys.add(key.value)
                continue
            scope = parents.get(node)
            while scope is not None and not isinstance(scope, (ast.FunctionDef, ast.AsyncFunctionDef)):
                scope = parents.get(scope)
            if scope is None:
                problems.append(f"{where}: a computed translation key outside a function")
            else:
                builders.add(scope.name)
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call) or _callee(node) not in builders:
                continue
            first = node.args[0] if node.args else None
            if isinstance(first, ast.Constant) and isinstance(first.value, str):
                keys.add(first.value)
            else:
                problems.append(
                    f"{path.name}:{node.lineno} calls {_callee(node)} with a key that is "
                    "not a literal"
                )
    return keys, problems


def test_every_error_the_package_raises_carries_a_translation_key() -> None:
    _keys, problems = _scan_raised_keys()
    assert problems == [], problems


def test_the_keys_raised_are_the_keys_declared() -> None:
    raised, _problems = _scan_raised_keys()
    declared = set(_declared())
    assert raised, "the scan found no refusal at all"
    assert raised == declared, (
        f"raised but not declared in strings.json exceptions: {sorted(raised - declared)}; "
        f"declared but never raised: {sorted(declared - raised)}"
    )


# ─── 3. one English, rendered the same from both sides ────────────────


def test_the_english_table_is_strings_json_word_for_word() -> None:
    declared = _declared()
    for key, entry in declared.items():
        assert set(entry) == {"message"}, f"exceptions.{key} has {sorted(entry)}"
    assert services.REFUSALS == {key: entry["message"] for key, entry in declared.items()}


def test_templates_are_ones_hassfest_and_the_frontend_can_render() -> None:
    """hassfest refuses a placeholder in single quotes and any HTML; the
    frontend renders ``{name}`` fields. The quotes a message shows around
    a watch or a duration travel in the placeholder's value instead."""
    for key, entry in _declared().items():
        message = entry["message"]
        assert not re.search(r"'\{\w+\}'", message), f"{key}: a quoted placeholder"
        assert "<" not in message and ">" not in message, f"{key}: markup"
        fields = re.findall(r"\{([^{}]*)\}", message)
        assert all(re.fullmatch(r"[a-z_]+", field) for field in fields), (key, fields)
        assert message.count("{") == message.count("}") == len(fields), key


def test_the_actions_a_refusal_names_are_registered_actions() -> None:
    named = {
        name
        for entry in _declared().values()
        for name in re.findall(rf"\b{DOMAIN}\.([a-z_]+)", entry["message"])
    }
    assert named == {services.SERVICE_END_WATCH, services.SERVICE_LIST_WATCHES}
    assert named <= set(services.SERVICES)


# ─── 4. every refusal, as a user reads it ─────────────────────────────


def _unloaded():
    hass = _hass()
    assert run(async_unload_entry(hass, ENTRY)) is True
    return hass


def _unreadable():
    hass = _hass(with_entry=False)
    hass.data.setdefault(DOMAIN, {})["_watches_unreadable"] = True
    return hass


def _full():
    hass = _hass()
    for i in range(watches.MAX_WATCHES):
        _call(hass, "start_watch", {"subject": f"thing {i}"})
    return hass


def _twins():
    hass = _hass()
    _call(hass, "start_watch", {"subject": "the gate canary"})
    _call(hass, "start_watch", {"subject": "the gate canary", "concern": "stopped"})
    return hass


def _two_kernels():
    return _kernel_hass("k1", "k2")


def _unreachable_kernel():
    return _kernel_hass(verifier=FakeVerifier(error=SecuraCVApiConnectionError("x")))


def _rejecting_kernel():
    return _kernel_hass(verifier=FakeVerifier(error=SecuraCVApiAuthError("unauthorized")))


def _odd_kernel():
    return _kernel_hass(
        api=FakeKernelApi(error=SecuraCVApiResponseError("export bundle too large: 9 bytes"))
    )


def _full_disk():
    def _boom(*args, **kwargs):
        raise OSError("disk full")

    hass = _kernel_hass()
    hass.data["_securacv_test_compose"] = _boom
    return hass


def _no_canaries():
    # The stub core has no entity registry, so there is nothing to roll out.
    return _hass()


def _rollout_refused_install():
    hass = _hass()

    class _State:
        def __init__(self, state, **attributes):
            self.state = state
            self.attributes = dict(attributes)

    states = {"update.porch_firmware": _State("on", installed_version="2.4.16", latest_version="2.5.0")}
    hass.states = types.SimpleNamespace(get=states.get)

    async def _boom(domain, service, data, blocking=False):
        raise RuntimeError("the broker is down")

    hass.services.async_call = _boom
    return hass


class Case(NamedTuple):
    key: str
    setup: Callable[[], Any]
    action: str
    data: dict[str, str]
    english: str


# The messages exactly as the actions raised them before they were
# translatable (HEAD cd6d703), which is what a user reads today.
CASES = [
    Case(
        "no_loaded_entry", _unloaded, "list_watches", {},
        "SecuraCV has no loaded entry, so nothing is running its watches. "
        "Enable or reload the integration to use them.",
    ),
    Case(
        "watches_unreadable", _unreadable, "list_watches", {},
        "SecuraCV could not read its stored watches (the log has the reason), so its "
        "watches are not available. Reload the integration to try again.",
    ),
    Case(
        "not_loaded_yet", lambda: _hass(with_entry=False), "start_watch",
        {"subject": "the gate canary"},
        "SecuraCV is not loaded yet, so its watches are not available. "
        "Try again once the integration has started.",
    ),
    Case(
        "duration_unreadable", _hass, "start_watch",
        {"subject": "the gate canary", "duration": "48 hours"},
        "Can't tell how long '48 hours' is. Give it in days, weeks, months, seasons "
        'or years ("two weeks", "10 days"), or leave it out for 14 days.',
    ),
    Case(
        "subject_empty", _hass, "start_watch", {"subject": "   "},
        "Say what to keep an eye on: subject is empty.",
    ),
    Case(
        "watch_limit_reached", _full, "start_watch", {"subject": "one more"},
        "Already running 20 watches, which is as many as the hub keeps. "
        "End one with securacv.end_watch first.",
    ),
    Case(
        "watch_ref_empty", _hass, "end_watch", {"watch": "  "},
        "Say which watch to end: its id or its label.",
    ),
    Case(
        "watch_not_found", _hass, "end_watch", {"watch": "the shed"},
        "No watch is called 'the shed'. securacv.list_watches names the ones running.",
    ),
    # repr picks the quotes, as the f-string did: a label with an
    # apostrophe is shown in double quotes.
    Case(
        "watch_not_found", _hass, "end_watch", {"watch": "the gate's canary"},
        "No watch is called \"the gate's canary\". securacv.list_watches names the ones "
        "running.",
    ),
    Case(
        "watch_ambiguous", _twins, "end_watch", {"watch": "the gate canary"},
        "'the gate canary' names 2 watches (w1-1700000000, w2-1700000000); end it by id.",
    ),
    # The kernel actions.
    Case(
        "no_kernel_entry", _hass, "verify", {},
        "No SecuraCV entry with a witness kernel is loaded, so there is no sealed log to "
        "verify or export. Add the SecuraCV app or sidecar first.",
    ),
    Case(
        "kernel_entry_ambiguous", _two_kernels, "verify", {},
        "2 SecuraCV entries have a witness kernel; say which one with entry_id.",
    ),
    Case(
        "entry_not_loaded", _kernel_hass, "export_evidence", {"entry_id": "k9"},
        "SecuraCV entry k9 is not loaded or has no witness kernel.",
    ),
    Case(
        "window_out_of_range", _kernel_hass, "export_evidence", {"window_hours": 0},
        "window_hours is 0; give a whole number of hours from 1 to 720.",
    ),
    Case(
        "kernel_unreachable", _unreachable_kernel, "verify", {},
        "The witness kernel did not answer. Check that the SecuraCV app or sidecar is "
        "running, then try again.",
    ),
    Case(
        "kernel_auth_failed", _rejecting_kernel, "verify", {},
        "The witness kernel rejected Home Assistant's credential. Re-authenticate the "
        "SecuraCV entry under Settings → Devices & services.",
    ),
    Case(
        "kernel_bad_response", _odd_kernel, "export_evidence", {},
        "The witness kernel answered with something other than the sealed log or the "
        "evidence bundle: export bundle too large: 9 bytes.",
    ),
    Case(
        "export_write_failed", _full_disk, "export_evidence", {},
        "The evidence pack could not be written under the configuration directory: disk full.",
    ),
    # The staged firmware rollout (docs/OTA.md §5).
    Case(
        "rollout_no_update_entities", _no_canaries, "staged_update", {},
        "No Canary has a firmware update entity yet. A Canary announces one over MQTT once "
        "it is on the broker; check that the fleet is online.",
    ),
    Case(
        "rollout_not_update_entities", _hass, "staged_update", {"entity_id": ["sensor.porch_lux"]},
        "['sensor.porch_lux'] are not update entities; name the Canaries' firmware update "
        "entities (the update domain) or leave entity_id out.",
    ),
    Case(
        "rollout_aborted", _rollout_refused_install, "staged_update",
        {"entity_id": ["update.porch_firmware"], "node_timeout": 60},
        "The staged rollout stopped: update.porch_firmware: the install could not be sent "
        "(the broker is down). 0 installed, 0 not attempted. Fix that node, then run it again.",
    ),
]

# The refusals (validation) and the failures (the kernel did not do it).
FAILURE_KEYS = {
    "kernel_unreachable", "kernel_auth_failed", "kernel_bad_response", "export_write_failed",
    "rollout_aborted",
}


def test_every_declared_key_has_a_pinned_case() -> None:
    assert {case.key for case in CASES} == set(_declared())


@pytest.mark.parametrize("case", CASES, ids=lambda case: case.key)
def test_each_refusal_reads_as_it_always_did(case: Case, monkeypatch) -> None:
    hass = case.setup()
    if (compose := hass.data.get("_securacv_test_compose")) is not None:
        monkeypatch.setattr(evidence, "compose_export", compose)
    expected_cls = HomeAssistantError if case.key in FAILURE_KEYS else ServiceValidationError
    with pytest.raises(expected_cls) as info:
        _call(hass, case.action, case.data)
    err = info.value
    if case.key in FAILURE_KEYS:
        assert not isinstance(err, ServiceValidationError), "a failure is not a refusal"

    assert str(err) == case.english
    assert err.translation_domain == DOMAIN
    assert err.translation_key == case.key

    placeholders = err.translation_placeholders or {}
    assert all(isinstance(v, str) for v in placeholders.values()), placeholders
    template = _declared()[case.key]["message"]
    assert set(re.findall(r"\{([a-z_]+)\}", template)) == set(placeholders), (
        "every placeholder the template names is supplied, and nothing else"
    )
    assert template.format(**placeholders) == case.english
