"""icons.json: every entity key has an icon, and it agrees with the code.

Icon translations are keyed by platform and translation key, like the
names in strings.json. Until the in-code `_attr_icon` attributes are
retired (they are the fallback for cores that ignore icons.json), the two
must say the same thing, or a card shows one icon and the entity page
another.
"""

from __future__ import annotations

import json
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


def _class_icons(module, platform: str) -> dict[str, str]:
    """{translation_key: _attr_icon} for every class that sets both as class attributes."""
    found = {}
    for value in vars(module).values():
        if not isinstance(value, type) or value.__module__ != module.__name__:
            continue
        key = vars(value).get("_attr_translation_key")
        icon = vars(value).get("_attr_icon")
        if isinstance(key, str) and isinstance(icon, str):
            found[key] = icon
    return found


def test_in_code_icons_agree_with_the_translations():
    for platform, module in (
        ("sensor", sensor_platform),
        ("binary_sensor", bs_platform),
        ("event", event_platform),
        ("button", button_platform),
    ):
        for key, icon in _class_icons(module, platform).items():
            assert ICONS["entity"][platform][key]["default"] == icon, (platform, key)
    # The per-type tables feed the constructor; they agree too.
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
