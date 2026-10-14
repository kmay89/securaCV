"""icons.json: every entity key has an icon, and icons.json is the one source.

Icon translations are keyed by platform and translation key, like the
names in strings.json. An entity that also sets `_attr_icon` in code hides
its translation: Home Assistant renders `_attr_icon` and never consults
icons.json for it, so the state icons (`chain_valid` off → alert) would be
dead. Every core the floor admits (hacs.json: 2026.5.0) honors icons.json,
so no class sets a static icon any more; the only in-code icons left are
the per-type tables that feed the tamper / transport constructors (held
equal to icons.json below) and the last-event sensors' DYNAMIC icon, which
follows the event type and otherwise defers to the translation.
"""

from __future__ import annotations

import json
import types
from pathlib import Path

from . import conftest  # noqa: F401  (installs the base HA stubs)

from .. import binary_sensor as bs_platform  # noqa: E402
from .. import button as button_platform  # noqa: E402
from .. import event as event_platform  # noqa: E402
from .. import sensor as sensor_platform  # noqa: E402
from ..const import ALL_TAMPER_TYPES, ALL_TRANSPORTS  # noqa: E402

PACKAGE_DIR = Path(sensor_platform.__file__).resolve().parent
ICONS = json.loads((PACKAGE_DIR / "icons.json").read_text(encoding="utf-8"))
STRINGS = json.loads((PACKAGE_DIR / "strings.json").read_text(encoding="utf-8"))

# Entities whose icon follows their state, by platform and key.
STATE_ICONS = {
    "binary_sensor": {"chain_valid", "tamper", "frigate_link", "kernel_online", "online",
                      "storage_replace"},
}


def test_every_translated_entity_has_a_default_icon():
    assert set(ICONS) == {"entity"}
    assert set(ICONS["entity"]) == set(STRINGS["entity"])
    for platform, keys in STRINGS["entity"].items():
        assert set(ICONS["entity"][platform]) == set(keys), platform
        for key, icon in ICONS["entity"][platform].items():
            assert icon["default"].startswith("mdi:"), (platform, key)
            assert set(icon) <= {"default", "state", "state_attributes"}, (platform, key)
            for state, state_icon in icon.get("state", {}).items():
                assert state in ("on", "off"), (platform, key, state)
                assert state_icon.startswith("mdi:"), (platform, key, state)


def test_the_state_icons_are_where_the_contract_wants_them():
    for platform, keys in STATE_ICONS.items():
        for key in keys:
            assert "state" in ICONS["entity"][platform][key], (platform, key)
    chain = ICONS["entity"]["binary_sensor"]["chain_valid"]["state"]
    assert chain["on"] != chain["off"]
    tamper = ICONS["entity"]["binary_sensor"]["tamper"]["state"]
    assert tamper["on"] != tamper["off"]
    link = ICONS["entity"]["binary_sensor"]["frigate_link"]["state"]
    assert link["on"] != link["off"]


PLATFORM_MODULES = (
    ("sensor", sensor_platform),
    ("binary_sensor", bs_platform),
    ("event", event_platform),
    ("button", button_platform),
)


def _package_classes(module):
    for value in vars(module).values():
        if isinstance(value, type) and value.__module__ == module.__name__:
            yield value


def test_no_class_hides_its_icon_translation_behind_a_static_icon():
    """A class-level `_attr_icon` wins over icons.json at runtime, so the
    translated default and every state icon would be dead for that entity."""
    for platform, module in PLATFORM_MODULES:
        for cls in _package_classes(module):
            for klass in cls.__mro__:
                if klass.__module__.startswith(sensor_platform.__name__.rsplit(".", 1)[0]):
                    assert "_attr_icon" not in vars(klass), (
                        f"{platform}.{klass.__name__} sets _attr_icon, which hides its "
                        "icons.json entry (and any state icon) from Home Assistant"
                    )


def test_the_last_event_sensor_defers_to_the_translation_until_an_event_arrives():
    """The kernel last-event icon follows the event type (a dynamic icon the
    translation cannot express); with no event it returns None so the
    icons.json default applies rather than a second in-code default."""
    coordinator = types.SimpleNamespace(data=None, last_update_success=True)
    entry = types.SimpleNamespace(entry_id="e1", data={})
    sensor = sensor_platform.SecuraCVKernelLastEventSensor.__new__(
        sensor_platform.SecuraCVKernelLastEventSensor
    )
    sensor.coordinator = coordinator
    sensor._entry = entry
    assert sensor.icon is None
    coordinator.data = {"latest_event": {"event_type": "BoundaryCrossingObjectLarge"}}
    assert sensor.icon == "mdi:car"
    assert ICONS["entity"]["sensor"]["kernel_last_event"]["default"] == "mdi:shield-eye"


def test_the_per_type_tables_agree_with_the_translations():
    # The per-type tables feed the tamper / transport constructors (one row
    # per advertised type, tests/test_feature_flags.py); they agree too.
    for tamper_type in ALL_TAMPER_TYPES:
        assert (
            ICONS["entity"]["binary_sensor"][f"tamper_{tamper_type}"]["default"]
            == bs_platform.TAMPER_TYPE_SENSORS[tamper_type]
        )
    for transport in ALL_TRANSPORTS:
        assert (
            ICONS["entity"]["binary_sensor"][f"transport_{transport}"]["default"]
            == bs_platform.TRANSPORT_SENSORS[transport]
        )
