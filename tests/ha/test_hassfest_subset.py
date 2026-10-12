"""What hassfest checks that a real core can check without a core checkout:
the manifest through the loader, strings/translations, services.yaml through
the service descriptions, icons.json through the icon helper."""
from __future__ import annotations

import json

import yaml
from homeassistant.core import HomeAssistant
from homeassistant.helpers import icon as icon_helper
from homeassistant.helpers import translation
from homeassistant.helpers.service import async_get_all_descriptions
from homeassistant.loader import async_get_integration

from conftest import REPO_ROOT, KernelMock
from custom_components.securacv import binary_sensor, button, event, sensor
from custom_components.securacv.const import DOMAIN

INTEGRATION = REPO_ROOT / "custom_components" / "securacv"


def translation_keys_in(module) -> set[str]:
    """Every `_attr_translation_key` a platform module's classes declare."""
    keys: set[str] = set()
    for name in dir(module):
        obj = getattr(module, name)
        if isinstance(obj, type):
            key = getattr(obj, "_attr_translation_key", None)
            if isinstance(key, str):
                keys.add(key)
    return keys


async def test_manifest_loads_through_the_loader(hass: HomeAssistant) -> None:
    """The manifest the loader reads: domain, deps, discovery filter, version."""
    integration = await async_get_integration(hass, DOMAIN)
    manifest = integration.manifest
    assert manifest["domain"] == DOMAIN
    assert manifest["name"] == "SecuraCV"
    assert manifest["config_flow"] is True
    assert manifest["integration_type"] == "hub"
    assert manifest["iot_class"] == "local_push"
    assert integration.dependencies == ["mqtt"]
    assert set(integration.after_dependencies) == {"http", "frontend", "media_source"}
    assert integration.mqtt == ["securacv/#", "witness/+/announce"]
    assert manifest["requirements"] == []
    assert manifest["version"].count(".") == 2
    assert manifest["codeowners"] == ["@kmay89"]
    assert manifest["documentation"].startswith("https://")
    assert manifest["issue_tracker"].startswith("https://")
    assert integration.quality_scale is None or isinstance(integration.quality_scale, str)
    # hacs.json and the manifest agree the integration is this one.
    hacs = json.loads((REPO_ROOT / "hacs.json").read_text())
    assert hacs["name"] == manifest["name"]
    assert hacs["homeassistant"] == "2026.5.0"


async def test_strings_match_translations_and_every_entity_key_resolves(hass: HomeAssistant) -> None:
    """strings.json is the source copy; en.json is byte-identical; every key used renders a name."""
    strings = (INTEGRATION / "strings.json").read_bytes()
    assert strings == (INTEGRATION / "translations" / "en.json").read_bytes()

    translations = await translation.async_get_translations(hass, "en", "entity", {DOMAIN})
    assert translations
    for platform, module in (("sensor", sensor), ("binary_sensor", binary_sensor), ("event", event), ("button", button)):
        for key in translation_keys_in(module):
            assert f"component.{DOMAIN}.entity.{platform}.{key}.name" in translations, (platform, key)
    # The per-type tamper and transport sensors' keys too.
    for tamper_type in binary_sensor.TAMPER_TYPE_SENSORS:
        assert f"component.{DOMAIN}.entity.binary_sensor.tamper_{tamper_type}.name" in translations
    for transport in binary_sensor.TRANSPORT_SENSORS:
        assert f"component.{DOMAIN}.entity.binary_sensor.transport_{transport}.name" in translations
    # The event entities' state attribute translations.
    assert translations[f"component.{DOMAIN}.entity.event.witness.state_attributes.event_type.state.boundary_crossing_object_large"] == "Person or vehicle"
    assert translations[f"component.{DOMAIN}.entity.event.verification_events.state_attributes.event_type.state.witness_key_changed"] == "Witness key changed"

    for category in ("config", "options", "issues", "exceptions", "services"):
        loaded = await translation.async_get_translations(hass, "en", category, {DOMAIN})
        assert loaded, category
    config = await translation.async_get_translations(hass, "en", "config", {DOMAIN})
    for step in ("user", "confirm", "hassio_confirm", "announce_confirm", "pair", "token_file", "mqtt_config", "kernel", "both", "reauth_confirm", "reconfigure"):
        assert f"component.{DOMAIN}.config.step.{step}.title" in config, step
    for abort in ("already_configured", "announce_signature_invalid", "announce_key_mismatch", "reauth_successful", "reconfigure_successful", "wrong_kernel", "not_securacv_device", "invalid_discovery_info"):
        assert f"component.{DOMAIN}.config.abort.{abort}" in config, abort
    assert f"component.{DOMAIN}.config.create_entry.no_kernel_found" in config
    issues = await translation.async_get_translations(hass, "en", "issues", {DOMAIN})
    for issue in ("no_kernel_found", "kernel_unreachable", "tofu_mismatch", "storage_replace", "entities_migrated", "mqtt_subscribe_failed"):
        assert f"component.{DOMAIN}.issues.{issue}.title" in issues, issue
    assert f"component.{DOMAIN}.issues.tofu_mismatch.fix_flow.step.confirm.title" in issues


async def test_services_yaml_renders_the_five_actions(hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry) -> None:
    """async_get_all_descriptions is what the UI reads: names, fields, selectors."""
    await setup_kernel_entry()
    descriptions = await async_get_all_descriptions(hass)
    actions = descriptions[DOMAIN]
    assert set(actions) == {"start_watch", "end_watch", "list_watches", "verify", "export_evidence"}
    export = actions["export_evidence"]
    assert export["name"] == "Export evidence"
    number = export["fields"]["window_hours"]["selector"]["number"]
    assert (number["min"], number["max"], number["mode"], number["unit_of_measurement"]) == (1, 720, "box", "h")
    assert export["fields"]["window_hours"]["default"] == 24
    assert export["fields"]["entry_id"]["selector"] == {"config_entry": {"integration": DOMAIN}}
    assert export["response"] == {"optional": True}
    verify = actions["verify"]
    assert verify["fields"]["entry_id"]["selector"] == {"config_entry": {"integration": DOMAIN}}
    assert verify["response"] == {"optional": True}
    assert actions["list_watches"]["response"] == {"optional": False}
    for name, action in actions.items():
        assert action["name"] and action["description"], name


async def test_icons_json_resolves_for_every_entity_key(hass: HomeAssistant) -> None:
    """icons.json loads through the icon helper and names every translation key."""
    icons = await icon_helper.async_get_icons(hass, "entity", {DOMAIN})
    # {domain: {platform: {translation_key: {"default": ..., "state": {...}}}}}
    entity_icons = icons[DOMAIN]
    for platform, module in (("sensor", sensor), ("binary_sensor", binary_sensor), ("event", event), ("button", button)):
        for key in translation_keys_in(module):
            assert key in entity_icons[platform], (platform, key)
            assert entity_icons[platform][key]["default"].startswith("mdi:")
    assert entity_icons["binary_sensor"]["chain_valid"]["state"]["off"] == "mdi:shield-alert-outline"
    assert entity_icons["binary_sensor"]["frigate_link"]["state"] == {"on": "mdi:video-check", "off": "mdi:video-off"}


def test_quality_scale_names_every_rule_once() -> None:
    """quality_scale.yaml is the 54-rule file in hassfest's shape; W7 owns the test rules."""
    data = yaml.safe_load((INTEGRATION / "quality_scale.yaml").read_text())
    rules = data["rules"]
    assert len(rules) == 54
    for name, value in rules.items():
        status = value if isinstance(value, str) else value["status"]
        assert status in ("done", "todo", "exempt"), name
        if status == "exempt":
            assert value["comment"], name
    assert rules["config-flow-test-coverage"]["status"] == "done"
    assert "tests/ha" in rules["config-flow-test-coverage"]["comment"]
    # Honest about the number: the rule wants 95% on every module and the
    # comment carries the measurement that says whether it is met.
    assert rules["test-coverage"]["status"] in ("done", "todo")
    assert "tests/ha" in rules["test-coverage"]["comment"]
