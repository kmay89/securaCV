"""quality_scale.yaml: the 54 rules, the hassfest value schema, and honesty.

hassfest does not grade a custom integration, so this is the gate: the
file names exactly the rule set, every value is one hassfest accepts, and
the rules a test can check against the code are not claimed `done` unless
the code backs them.
"""

from __future__ import annotations

import re
from pathlib import Path

import yaml

PACKAGE_DIR = Path(__file__).resolve().parent.parent
SCALE = PACKAGE_DIR / "quality_scale.yaml"

BRONZE = (
    "action-setup", "appropriate-polling", "brands", "common-modules", "config-flow",
    "config-flow-test-coverage", "dependency-transparency", "docs-actions", "docs-conditions",
    "docs-high-level-description", "docs-installation-instructions", "docs-removal-instructions",
    "docs-triggers", "entity-event-setup", "entity-unique-id", "has-entity-name", "runtime-data",
    "test-before-configure", "test-before-setup", "unique-config-entry",
)
SILVER = (
    "action-exceptions", "config-entry-unloading", "docs-configuration-parameters",
    "docs-installation-parameters", "entity-unavailable", "integration-owner",
    "log-when-unavailable", "parallel-updates", "reauthentication-flow", "test-coverage",
)
GOLD = (
    "devices", "diagnostics", "discovery", "discovery-update-info", "docs-data-update",
    "docs-examples", "docs-known-limitations", "docs-supported-devices", "docs-supported-functions",
    "docs-troubleshooting", "docs-use-cases", "dynamic-devices", "entity-category",
    "entity-device-class", "entity-disabled-by-default", "entity-translations",
    "exception-translations", "icon-translations", "reconfiguration-flow", "repair-issues",
    "stale-devices",
)
PLATINUM = ("async-dependency", "inject-websession", "strict-typing")
ALL_RULES = BRONZE + SILVER + GOLD + PLATINUM


def _rules() -> dict:
    doc = yaml.safe_load(SCALE.read_text(encoding="utf-8"))
    assert set(doc) == {"rules"}, "the file holds one top-level key, `rules`"
    return doc["rules"]


def _status(value) -> str:
    return value if isinstance(value, str) else value["status"]


def test_the_rule_set_is_exactly_the_fifty_four():
    assert len(ALL_RULES) == 54 and len(set(ALL_RULES)) == 54
    assert set(_rules()) == set(ALL_RULES)


def test_every_value_is_one_hassfest_accepts():
    for rule, value in _rules().items():
        assert re.fullmatch(r"[a-z]+(-[a-z]+)*", rule), rule
        if isinstance(value, str):
            assert value in ("done", "todo"), rule
            continue
        assert isinstance(value, dict), rule
        assert set(value) == {"status", "comment"}, rule
        assert value["status"] in ("done", "todo", "exempt"), rule
        assert isinstance(value["comment"], str) and value["comment"].strip(), rule


def test_exempt_rules_are_the_ones_that_do_not_apply():
    rules = _rules()
    assert _status(rules["docs-triggers"]) == "exempt"
    assert _status(rules["docs-conditions"]) == "exempt"
    for rule, value in rules.items():
        if _status(value) == "exempt":
            assert rule in ("docs-triggers", "docs-conditions"), rule


def _source(name: str) -> str:
    return (PACKAGE_DIR / name).read_text(encoding="utf-8")


def test_claims_the_code_can_back_are_backed():
    rules = _rules()
    claims = {
        "runtime-data": "entry.runtime_data = runtime" in _source("__init__.py"),
        "parallel-updates": all(
            "PARALLEL_UPDATES = 0" in _source(f"{platform}.py")
            for platform in ("sensor", "binary_sensor")
        ),
        "icon-translations": (PACKAGE_DIR / "icons.json").is_file(),
        "diagnostics": "async_redact_data" in _source("diagnostics.py"),
        "reauthentication-flow": "async def async_step_reauth(" in _source("config_flow.py"),
        "reconfiguration-flow": "async def async_step_reconfigure(" in _source("config_flow.py"),
        "repair-issues": "async def async_create_fix_flow(" in _source("repairs.py"),
        "stale-devices": "async def async_remove_config_entry_device(" in _source("__init__.py"),
        "common-modules": all((PACKAGE_DIR / f).is_file() for f in ("coordinator.py", "entity.py")),
        "brands": (PACKAGE_DIR / "brand" / "icon.png").is_file(),
        "entity-translations": '"entity"' in _source("strings.json"),
        "exception-translations": '"exceptions"' in _source("strings.json"),
        "test-before-setup": "ConfigEntryNotReady" in _source("__init__.py"),
        "config-entry-unloading": "async def async_unload_entry(" in _source("__init__.py"),
        "action-setup": "async_setup_services(hass)" in _source("__init__.py"),
        "entity-disabled-by-default": (
            "_attr_entity_registry_enabled_default = False" in _source("binary_sensor.py")
        ),
        "entity-category": any(
            "EntityCategory.DIAGNOSTIC" in _source(f"{platform}.py")
            for platform in ("sensor", "binary_sensor")
        ),
    }
    for rule, backed in claims.items():
        status = _status(rules[rule])
        assert status != "done" or backed, f"{rule} is claimed done but the code does not back it"
        assert status == "done" or not backed, f"{rule} is backed by the code but not claimed"


def test_undone_work_names_who_owes_it():
    for rule, value in _rules().items():
        if _status(value) == "todo":
            assert isinstance(value, dict), f"{rule}: a todo says why"
