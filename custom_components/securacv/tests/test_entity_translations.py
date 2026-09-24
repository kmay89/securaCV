"""Entity names come from translations, and every translation is live.

Before this test existed strings.json declared nine entity keys that no
entity ever consulted: every class set ``_attr_name`` directly, and Home
Assistant renders ``_attr_name`` without looking at ``translation_key`` —
so the translations were dead, and one of them (``kernel_online``: "Kernel
Online") disagreed with what users actually saw ("Online").

Three properties, checked without a real HA core (the platform stubs from
test_mqtt_payload_hardening are enough to instantiate every entity class):

  1. strings.json and translations/en.json are the same document — HA
     reads the latter at runtime and hassfest validates the former, and
     nothing else kept them in step;
  2. every concrete entity class in sensor.py / binary_sensor.py (each
     tamper type and each transport, from the ALL_* lists) sets
     ``_attr_translation_key`` to a key strings.json declares for its
     platform, and NO class or instance defines ``_attr_name`` — an entity
     that keeps ``_attr_name`` silently bypasses its translation;
  3. every key strings.json declares is used by some class (no dead keys),
     and the rendered English names are pinned: they are what
     docs/homeassistant_setup.md promises and what
     canary-local/tools/gen_homeassistant.py bakes into the generated
     canary-local/devices/homeassistant.json, so a rename is a doc +
     generator change, never a silent one.
"""

from __future__ import annotations

import json
import types
from pathlib import Path

import pytest

from . import conftest  # noqa: F401  (installs the base HA stubs)

# Reuse the platform stubs the hardening tests install (idempotent).
from .test_mqtt_payload_hardening import _install_platform_stubs

_install_platform_stubs()

from .. import binary_sensor as bs_platform  # noqa: E402
from .. import sensor as sensor_platform  # noqa: E402
from ..const import ALL_TAMPER_TYPES, ALL_TRANSPORTS  # noqa: E402

PACKAGE_DIR = Path(sensor_platform.__file__).resolve().parent
STRINGS = PACKAGE_DIR / "strings.json"
EN_JSON = PACKAGE_DIR / "translations" / "en.json"

ENTRY = types.SimpleNamespace(entry_id="e1", data={"url": "http://kernel.local:8799"})
COORDINATOR = types.SimpleNamespace(data=None, last_update_success=True)

# The English names as users see them today. Pinned on purpose: these feed
# docs/homeassistant_setup.md's entity list and the generated Hub page data.
EXPECTED_NAMES: dict[str, dict[str, str]] = {
    "sensor": {
        "kernel_last_event": "SecuraCV Last Event",
        "storage_health": "Storage Health",
        "storage_free_pct": "Storage Free",
        "storage_wear_pct": "Storage Wear Estimate",
        "storage_write_rate": "Storage Write Rate",
        "soc_temperature": "SoC Temperature",
        "adapter_stats": "SecuraCV Adapter Host",
        "witness_count": "Witness Count",
        "chain_length": "Chain Length",
        "last_event": "Last Event",
        "health_status": "Health",
        "sd_wear": "SD Wear Estimate",
        "gps_fix": "GPS Fix",
        "radar_link": "Radar Link",
    },
    "binary_sensor": {
        "kernel_online": "Online",
        "storage_replace": "Storage Replacement Recommended",
        "online": "Online",
        "chain_valid": "Chain Valid",
        "tamper": "Tamper",
        "tamper_power_loss": "Power Loss",
        "tamper_sd_remove": "SD Removed",
        "tamper_sd_error": "SD Error",
        "tamper_gps_jamming": "GPS Jamming",
        "tamper_motion": "Unexpected Motion",
        "tamper_enclosure": "Enclosure Open",
        "tamper_gpio": "GPIO Tamper",
        "tamper_watchdog": "Watchdog Timeout",
        "tamper_unexpected_reboot": "Unexpected Reboot",
        "tamper_memory_critical": "Memory Critical",
        "sd_replace": "SD Replacement Recommended",
        "transport_wifi_ap": "WiFi AP",
        "transport_wifi_sta": "WiFi Station",
        "transport_mqtt": "MQTT",
        "transport_ble": "Bluetooth",
        "transport_mesh": "Mesh Network",
        "transport_chirp": "Chirp Network",
        "motion": "Motion",
        "occupancy": "Occupancy",
        "mesh_connected": "Mesh Connected",
        "chirp_active": "Chirp Active",
    },
}


def _strings() -> dict:
    return json.loads(STRINGS.read_text(encoding="utf-8"))


def _canary(cls, *extra):
    return cls("securacv", "canary01", ENTRY, *extra)


def _kernel(cls):
    return cls(COORDINATOR, ENTRY)


# Every concrete entity class, with a constructor for it. Per-type classes
# are instantiated once per ADVERTISED type so each translation key they
# derive is exercised.
SENSOR_ENTITIES = [
    (sensor_platform.SecuraCVKernelLastEventSensor, [_kernel]),
    (sensor_platform.SecuraCVKernelStorageHealthSensor, [_kernel]),
    (sensor_platform.SecuraCVKernelStorageFreeSensor, [_kernel]),
    (sensor_platform.SecuraCVKernelStorageWearSensor, [_kernel]),
    (sensor_platform.SecuraCVKernelStorageWriteRateSensor, [_kernel]),
    (sensor_platform.SecuraCVKernelTemperatureSensor, [_kernel]),
    (sensor_platform.SecuraCVAdapterStatsSensor, [_kernel]),
    (sensor_platform.SecuraCVCanaryWitnessCountSensor, [_canary]),
    (sensor_platform.SecuraCVCanaryChainLengthSensor, [_canary]),
    (sensor_platform.SecuraCVCanaryLastEventSensor, [_canary]),
    (sensor_platform.SecuraCVCanaryHealthSensor, [_canary]),
    (sensor_platform.SecuraCVCanarySDWearSensor, [_canary]),
    (sensor_platform.SecuraCVCanaryGPSSensor, [_canary]),
    (sensor_platform.SecuraCVCanaryRadarLinkSensor, [_canary]),
]
BINARY_SENSOR_ENTITIES = [
    (bs_platform.SecuraCVKernelOnlineSensor, [_kernel]),
    (bs_platform.SecuraCVKernelStorageReplaceSensor, [_kernel]),
    (bs_platform.SecuraCVCanaryOnlineSensor, [_canary]),
    (bs_platform.SecuraCVCanaryChainValidSensor, [_canary]),
    (bs_platform.SecuraCVCanaryTamperSensor, [_canary]),
    (
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        [
            (lambda cls, t=t: _canary(cls, t, bs_platform.TAMPER_TYPE_SENSORS[t]))
            for t in ALL_TAMPER_TYPES
        ],
    ),
    (bs_platform.SecuraCVCanarySDReplaceSensor, [_canary]),
    (
        bs_platform.SecuraCVCanaryTransportSensor,
        [
            (lambda cls, t=t: _canary(cls, t, bs_platform.TRANSPORT_SENSORS[t]))
            for t in ALL_TRANSPORTS
        ],
    ),
    (bs_platform.SecuraCVCanaryMotionSensor, [_canary]),
    (bs_platform.SecuraCVCanaryOccupancySensor, [_canary]),
    (bs_platform.SecuraCVCanaryMeshConnectedSensor, [_canary]),
    (bs_platform.SecuraCVCanaryChirpActiveSensor, [_canary]),
]
PLATFORMS = {
    "sensor": (sensor_platform, SENSOR_ENTITIES),
    "binary_sensor": (bs_platform, BINARY_SENSOR_ENTITIES),
}


@pytest.fixture(autouse=True)
def _coordinator_entity_init(monkeypatch):
    """The stubbed CoordinatorEntity has no __init__(coordinator); give it
    the real one's shape so kernel entities construct under the stubs."""
    seen = set()
    for _platform, entities in PLATFORMS.values():
        for cls, _ctors in entities:
            for base in cls.__mro__:
                if base.__name__ == "CoordinatorEntity" and base not in seen:
                    seen.add(base)
                    monkeypatch.setattr(
                        base,
                        "__init__",
                        lambda self, coordinator: setattr(self, "coordinator", coordinator),
                        raising=False,
                    )


def _concrete_entity_classes(module, platform_base_name: str) -> set[type]:
    """Every entity subclass the platform module defines, minus the bases."""
    found = set()
    for value in vars(module).values():
        if not isinstance(value, type) or value.__module__ != module.__name__:
            continue
        if not any(b.__name__ == platform_base_name for b in value.__mro__):
            continue
        if value.__name__.endswith("Base"):
            continue
        found.add(value)
    return found


def _package_classes(cls) -> list[type]:
    """The classes in cls's MRO that this integration defines."""
    return [k for k in cls.__mro__ if k.__module__.startswith(sensor_platform.__name__.rsplit(".", 1)[0])]


# ─── 1. the two files are one document ────────────────────────────────


def test_strings_json_and_en_json_are_identical() -> None:
    strings_text = STRINGS.read_text(encoding="utf-8")
    en_text = EN_JSON.read_text(encoding="utf-8")
    assert json.loads(strings_text) == json.loads(en_text), (
        "strings.json (hassfest's input) and translations/en.json (what HA "
        "renders) have drifted — copy strings.json over en.json"
    )
    # Byte-identical, not merely equal: both are carried verbatim to the
    # HACS mirror, and one hand-edit is the whole way they drift.
    assert strings_text == en_text


# ─── 2. every entity class translates, none bypasses it ───────────────


@pytest.mark.parametrize("platform", sorted(PLATFORMS))
def test_entity_table_covers_every_concrete_class(platform: str) -> None:
    module, entities = PLATFORMS[platform]
    base_name = "SensorEntity" if platform == "sensor" else "BinarySensorEntity"
    concrete = _concrete_entity_classes(module, base_name)
    assert concrete, f"no entity classes found in {module.__name__}"
    assert {cls for cls, _ in entities} == concrete, (
        "an entity class was added to the platform without a row in this "
        "test's constructor table"
    )


@pytest.mark.parametrize("platform", sorted(PLATFORMS))
def test_every_entity_uses_a_declared_translation_key(platform: str) -> None:
    declared = _strings()["entity"][platform]
    _module, entities = PLATFORMS[platform]
    for cls, ctors in entities:
        for klass in _package_classes(cls):
            assert "_attr_name" not in vars(klass), (
                f"{klass.__name__} sets _attr_name, which bypasses translation_key"
            )
        for ctor in ctors:
            inst = ctor(cls)
            assert "_attr_name" not in vars(inst), (
                f"{cls.__name__} sets _attr_name on the instance, which bypasses "
                "translation_key"
            )
            key = getattr(inst, "_attr_translation_key", None)
            assert isinstance(key, str) and key, f"{cls.__name__} sets no _attr_translation_key"
            assert key in declared, (
                f"{cls.__name__} uses translation key {key!r} but strings.json "
                f"entity.{platform} does not declare it — HA would fall back to "
                "the bare device name"
            )
            assert declared[key].get("name"), f"entity.{platform}.{key} has no name"
            assert getattr(inst, "_attr_has_entity_name", False) is True, (
                f"{cls.__name__}: translation_key names are only applied when "
                "has_entity_name is True"
            )


# ─── 3. no dead keys, and the rendered names are pinned ───────────────


@pytest.mark.parametrize("platform", sorted(PLATFORMS))
def test_every_declared_key_is_used_and_named_as_expected(platform: str) -> None:
    declared = _strings()["entity"][platform]
    _module, entities = PLATFORMS[platform]
    used = {ctor(cls)._attr_translation_key for cls, ctors in entities for ctor in ctors}
    assert set(declared) == used, (
        f"strings.json entity.{platform} keys and the keys entities use differ: "
        f"dead={sorted(set(declared) - used)} missing={sorted(used - set(declared))}"
    )
    assert {k: v["name"] for k, v in declared.items()} == EXPECTED_NAMES[platform], (
        "a rendered entity name changed — that moves friendly_name, new-install "
        "entity_ids, docs/homeassistant_setup.md's entity list and the generated "
        "canary-local/devices/homeassistant.json; update all of them together"
    )


def test_strings_declare_only_the_two_platforms() -> None:
    assert set(_strings()["entity"]) == set(PLATFORMS)
