"""Every blueprint under docs/blueprints loads and renders a valid automation
on a real core, through the blueprint helpers the automation integration uses."""
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any

import pytest
from homeassistant.components.automation.config import async_validate_config_item
from homeassistant.components.automation.helpers import async_get_blueprints
from homeassistant.components.blueprint.models import Blueprint
from homeassistant.core import HomeAssistant

from conftest import REPO_ROOT

BLUEPRINTS = sorted((REPO_ROOT / "docs" / "blueprints").glob("*.yaml"))
assert BLUEPRINTS, "no blueprints to test"

# Sample values by selector type for inputs without a default.
ENTITY_DOMAIN_SAMPLES = {
    "sensor": "sensor.sample",
    "binary_sensor": "binary_sensor.sample",
    "event": "event.securacv_front_door",
    "light": "light.sample",
    "switch": "switch.sample",
}


def sample_for(name: str, definition: dict[str, Any]) -> Any:
    """A plausible value for an input, from its selector."""
    selector = (definition or {}).get("selector") or {}
    if "entity" in selector:
        spec = selector["entity"] or {}
        domain = spec.get("domain")
        if isinstance(domain, list):
            domain = domain[0]
        if spec.get("multiple"):
            return [ENTITY_DOMAIN_SAMPLES.get(domain or "sensor", f"{domain}.sample")]
        return ENTITY_DOMAIN_SAMPLES.get(domain or "sensor", f"{domain}.sample")
    if "text" in selector:
        return "front_door" if "camera" in name else "notify.mobile_app_phone"
    if "number" in selector:
        spec = selector["number"] or {}
        return spec.get("min", 1)
    if "time" in selector:
        return "08:00:00"
    if "boolean" in selector:
        return False
    if "duration" in selector:
        return {"minutes": 5}
    if "device" in selector:
        return False
    if "select" in selector:
        options = (selector["select"] or {}).get("options") or ["a"]
        first = options[0]
        return first["value"] if isinstance(first, dict) else first
    return "sample"


@pytest.fixture
async def installed_blueprints(hass: HomeAssistant) -> Path:
    """Copy docs/blueprints/*.yaml where the add-on installs them."""
    target = Path(hass.config.path("blueprints", "automation", "securacv"))
    target.mkdir(parents=True, exist_ok=True)
    for path in BLUEPRINTS:
        shutil.copy(path, target / path.name)
    return target


@pytest.mark.parametrize("path", BLUEPRINTS, ids=[p.name for p in BLUEPRINTS])
async def test_blueprint_loads_and_renders_a_valid_automation(
    hass: HomeAssistant, installed_blueprints: Path, path: Path
) -> None:
    """Schema-valid, supported on this core, and its substituted automation validates."""
    blueprints = async_get_blueprints(hass)
    blueprint: Blueprint = await blueprints.async_get_blueprint(f"securacv/{path.name}")
    assert blueprint.domain == "automation"
    assert blueprint.validate() is None, blueprint.validate()
    assert blueprint.metadata["homeassistant"]["min_version"] == "2026.5.0"
    assert blueprint.name.startswith(("SecuraCV", "Canary Sense")), blueprint.name

    inputs = {
        name: sample_for(name, definition)
        for name, definition in blueprint.inputs.items()
        if not (isinstance(definition, dict) and "default" in definition)
    }
    config = {"alias": f"test {path.stem}", "use_blueprint": {"path": f"securacv/{path.name}", "input": inputs}}
    automation = await async_validate_config_item(hass, path.stem, config)
    assert automation is not None
    assert automation.validation_status.value == "ok", automation.validation_error
    assert automation.raw_blueprint_inputs["use_blueprint"] == config["use_blueprint"]
    assert automation.get("triggers") or automation.get("trigger")
    assert automation.get("actions") or automation.get("action")


# ── the digest's chain line, through the core's own template engine ──────

DIGEST = REPO_ROOT / "docs" / "blueprints" / "securacv_daily_digest.yaml"


def _digest_variables() -> dict[str, str]:
    """The digest blueprint's `variables:` block, `!input` resolved to one
    chain sensor, in the order the automation renders them."""
    import yaml

    class _Loader(yaml.SafeLoader):
        pass

    def _input(loader: yaml.SafeLoader, node: yaml.Node) -> str:
        name = loader.construct_scalar(node)
        return {
            "digest_sensor": "sensor.securacv_daily_digest",
            "chain_sensor": "binary_sensor.securacv_chain_valid",
            "chain_length_sensor": "sensor.securacv_chain_length",
            "notify_device": "",
            "notify_service": "notify.mobile_app_phone",
            "digest_time": "08:00:00",
        }[name]

    _Loader.add_constructor("!input", _input)
    document = yaml.load(DIGEST.read_text(encoding="utf-8"), Loader=_Loader)
    return document["variables"]


def _render_digest(hass: HomeAssistant) -> dict[str, Any]:
    """Render the digest's variables and its notification title the way the
    automation does (each variable may read the ones before it)."""
    from homeassistant.helpers.template import Template

    context: dict[str, Any] = {}
    for name, value in _digest_variables().items():
        if name == "notify_target":
            context[name] = "notify.mobile_app_phone"
            continue
        context[name] = Template(str(value), hass).async_render(variables=dict(context))
    import yaml

    class _Loader(yaml.SafeLoader):
        pass

    _Loader.add_constructor("!input", lambda loader, node: "x")
    document = yaml.load(DIGEST.read_text(encoding="utf-8"), Loader=_Loader)
    action = document["action"][0]["data"]
    context["title"] = Template(action["title"], hass).async_render(variables=dict(context))
    context["message"] = Template(action["message"], hass).async_render(variables=dict(context))
    context["importance"] = Template(action["data"]["importance"], hass).async_render(variables=dict(context))
    return context


@pytest.mark.parametrize(
    "chain_state,attributes,expected_title,expected_line,importance",
    [
        (
            "unknown",
            {"key_mismatch": True, "trust_reason": "kernel_key_mismatch",
             "error": "the witness key changed; confirm it in Settings → Repairs", "verified_entries": 1204},
            "SecuraCV: witness key changed",
            "Witness key changed — confirm it in Settings → Repairs.",
            "high",
        ),
        (
            "off",
            {"key_mismatch": False, "trust_reason": "ha_verify_failed", "failed_seq": 1190, "verified_entries": 1189},
            "SecuraCV: chain check failed",
            "Chain check FAILED — the witness log may have been altered; export now, entries before the break still verify.",
            "high",
        ),
        (
            "on",
            {"key_mismatch": False, "trust_reason": "ha_verified_chain", "verified_entries": 1204},
            "SecuraCV daily digest",
            "Chain verified ✓ · 1,204 entries.",
            "default",
        ),
        (
            "unknown",
            {"key_mismatch": False, "trust_reason": None},
            "SecuraCV daily digest",
            "Chain not verified yet.",
            "default",
        ),
    ],
    ids=["key-changed", "failed", "verified", "not-yet"],
)
async def test_the_digest_renders_a_key_change_as_repairs_never_as_failed(
    hass: HomeAssistant, chain_state: str, attributes: dict[str, Any], expected_title: str, expected_line: str, importance: str
) -> None:
    """D10 in the morning notification: a key change is 'Witness key changed —
    confirm it in Settings → Repairs', with the digest's title saying so, and
    only a failed walk is 'Chain check FAILED'."""
    hass.states.async_set("sensor.securacv_daily_digest", "0", {"per_zone": {}, "per_period": {}})
    hass.states.async_set("binary_sensor.securacv_chain_valid", chain_state, attributes)
    hass.states.async_set("sensor.securacv_chain_length", "1204")
    await hass.async_block_till_done()

    rendered = _render_digest(hass)
    assert rendered["chain_key_changed"] is (attributes.get("key_mismatch") is True)
    assert rendered["chain_line"] == expected_line
    assert rendered["title"] == expected_title
    assert rendered["importance"] == importance
    assert rendered["message"] == f"Quiet night: no witness events. {expected_line}"
    if attributes.get("key_mismatch"):
        assert "FAILED" not in rendered["message"] and "altered" not in rendered["message"]
        assert rendered["chain_failed"] is False


# ── the battery blueprint's low-battery condition, on the core's engine ──

BATTERY = REPO_ROOT / "docs" / "blueprints" / "canary_battery_health.yaml"


def _battery_low_alerts(hass: HomeAssistant) -> bool:
    """Render the battery blueprint's `battery_pct` variable and its low-battery
    condition the way the automation does, on `sensor.canary_health`."""
    import yaml
    from homeassistant.helpers.template import Template

    class _Loader(yaml.SafeLoader):
        pass

    inputs = {"health_sensor": "sensor.canary_health", "low_threshold": 25}
    _Loader.add_constructor("!input", lambda loader, node: inputs.get(loader.construct_scalar(node), ""))
    document = yaml.load(BATTERY.read_text(encoding="utf-8"), Loader=_Loader)
    context: dict[str, Any] = {name: inputs[name] for name in ("health_sensor", "low_threshold")}
    context["battery_pct"] = Template(document["variables"]["battery_pct"], hass).async_render(variables=dict(context))
    (low,) = [
        option for option in document["action"][0]["choose"]
        if {"condition": "trigger", "id": "low"} in option["conditions"]
    ]
    (condition,) = [c for c in low["conditions"] if c["condition"] == "template"]
    return Template(condition["value_template"], hass).async_render(variables=dict(context))


@pytest.mark.parametrize(
    "state,attributes,alerts",
    [
        # A discharging battery below the threshold: the alert this exists for.
        ("critical", {"battery_present": True, "battery_percent": 8}, True),
        ("warning", {"battery_present": True, "battery_percent": 24}, True),
        # Critical on low memory with the battery fine, or on mains: not a battery alert.
        ("critical", {"battery_present": True, "battery_percent": 80}, False),
        ("critical", {"battery_present": False, "battery_percent": 100}, False),
        # A battery that sent no number for its charge (HA60): battery_percent
        # None, which `| int(0)` reads as 0, so the template would have sent a
        # "battery critical (0%)" push for a unit low on memory.
        ("critical", {"battery_present": True, "battery_percent": None}, False),
        ("critical", {"battery_present": True}, False),
    ],
    ids=["low", "warning", "memory", "mains", "no-charge", "no-attribute"],
)
async def test_the_battery_blueprint_alerts_only_on_a_charge_the_unit_sent(
    hass: HomeAssistant, state: str, attributes: dict[str, Any], alerts: bool
) -> None:
    """The Health sensor shows battery_percent None for a present battery that
    sent no number (sweep HA60) and reads healthy on it, so the blueprint's
    low-battery branch fires only on a charge below the threshold."""
    hass.states.async_set("sensor.canary_health", state, attributes)
    await hass.async_block_till_done()
    assert _battery_low_alerts(hass) is alerts
