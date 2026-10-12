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
