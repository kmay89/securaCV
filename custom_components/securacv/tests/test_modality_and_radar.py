"""Unit tests for Phase 3 dashboard groundwork (canary-sense / MR60BHA2).

Covers:
  - the pure modality + attestation contract helpers in const.py
    (the field names/values the Rust adapter and firmware must match), and
  - the radar-link diagnostic sensor's frame-age math and state logic, plus
    the device_type resolution that gates its conditional creation.

const.py has no Home Assistant imports, so it loads under the repo-root
conftest stubs directly. sensor.py imports a handful of HA component modules
the base stubs don't cover; we install just those extra surfaces here (the
same lightweight-stub convention as conftest.py) so the entity classes import
and their pure helpers can be exercised without a real HA core.
"""

from __future__ import annotations

import sys
import types

from . import conftest  # noqa: F401  (installs the base HA stubs)


def _install_sensor_platform_stubs() -> None:
    """Add the sensor-platform stubs sensor.py needs on top of conftest's."""
    enum_ns = types.SimpleNamespace

    sensor_mod = sys.modules.get("homeassistant.components.sensor") or types.ModuleType(
        "homeassistant.components.sensor"
    )
    sensor_mod.SensorEntity = type("SensorEntity", (), {})
    sensor_mod.SensorDeviceClass = enum_ns(TEMPERATURE="temperature")
    sensor_mod.SensorStateClass = enum_ns(
        MEASUREMENT="measurement", TOTAL_INCREASING="total_increasing"
    )
    sys.modules["homeassistant.components.sensor"] = sensor_mod

    const_mod = sys.modules.get("homeassistant.const") or types.ModuleType(
        "homeassistant.const"
    )
    const_mod.CONF_URL = "url"
    const_mod.PERCENTAGE = "%"
    const_mod.EntityCategory = enum_ns(DIAGNOSTIC="diagnostic")
    const_mod.UnitOfTemperature = enum_ns(CELSIUS="°C")
    sys.modules["homeassistant.const"] = const_mod

    entity_mod = types.ModuleType("homeassistant.helpers.entity")
    entity_mod.DeviceInfo = dict
    sys.modules["homeassistant.helpers.entity"] = entity_mod

    plat_mod = types.ModuleType("homeassistant.helpers.entity_platform")
    plat_mod.AddEntitiesCallback = object
    sys.modules["homeassistant.helpers.entity_platform"] = plat_mod

    # CoordinatorEntity is `object` in the base stubs; sensor.py mixes it with
    # SensorEntity (CoordinatorEntity, SensorEntity), which is an illegal MRO
    # when both resolve to object. Make it a distinct class so the kernel
    # sensor classes can be defined at import time.
    uc_mod = sys.modules["homeassistant.helpers.update_coordinator"]
    uc_mod.CoordinatorEntity = type("CoordinatorEntity", (), {})


_install_sensor_platform_stubs()

from ..const import (  # noqa: E402
    ATTESTATION_ADAPTER,
    ATTESTATION_DEVICE,
    ATTESTATION_HA_BRIDGED,
    MODALITY_RADAR,
    MODALITY_UNKNOWN,
    modality_for,
    modality_metadata,
    normalize_attestation,
    normalize_modality,
)
from .. import sensor as sensor_mod  # noqa: E402


# ─── modality contract (const.py) ─────────────────────────────────────

def test_normalize_modality_known_and_aliases():
    assert normalize_modality("radar") == "radar"
    assert normalize_modality("wifi-csi") == "wifi-csi"
    assert normalize_modality("wifi_csi") == "wifi-csi"   # underscore spelling
    assert normalize_modality("CSI") == "wifi-csi"        # alias + case
    assert normalize_modality("mmwave") == "radar"        # alias
    assert normalize_modality("camera") == "camera"
    assert normalize_modality("contact") == "contact"
    # unknown / junk degrades to the render-as-before sentinel
    assert normalize_modality("teleporter") == MODALITY_UNKNOWN
    assert normalize_modality(None) == MODALITY_UNKNOWN
    assert normalize_modality("") == MODALITY_UNKNOWN


def test_modality_for_priority_and_fallback():
    # explicit event modality wins
    assert modality_for({"modality": "radar"}) == MODALITY_RADAR
    # event device_type fallback
    assert modality_for({"device_type": "canary-sense"}) == MODALITY_RADAR
    # device-level device_type fallback (event omits everything)
    assert modality_for({}, "canary-sense") == MODALITY_RADAR
    assert modality_for({}, "canary-vision") == "camera"
    # explicit modality beats a conflicting device_type
    assert modality_for({"modality": "camera", "device_type": "canary-sense"}) == "camera"
    # nothing resolvable → unknown (backward compatible: no indicator)
    assert modality_for({}) == MODALITY_UNKNOWN
    assert modality_for({}, "weather-station") == MODALITY_UNKNOWN
    assert modality_for(None) == MODALITY_UNKNOWN


def test_modality_metadata_shape():
    meta = modality_metadata("radar")
    assert meta == {"label": "Radar", "icon": "mdi:radar"}
    # unknown / unset → None so callers omit the indicator
    assert modality_metadata(MODALITY_UNKNOWN) is None
    assert modality_metadata(None) is None


# ─── attestation contract (const.py) ──────────────────────────────────

def test_normalize_attestation_defaults_to_device():
    # absent / junk → device-attested, so existing events are unchanged
    assert normalize_attestation(None) == ATTESTATION_DEVICE
    assert normalize_attestation("") == ATTESTATION_DEVICE
    assert normalize_attestation("nonsense") == ATTESTATION_DEVICE
    # explicit Track B provenance is honored
    assert normalize_attestation("adapter") == ATTESTATION_ADAPTER
    assert normalize_attestation("ha-bridged") == ATTESTATION_HA_BRIDGED
    assert normalize_attestation("ha_bridged") == ATTESTATION_HA_BRIDGED  # underscore
    # documented aliases
    assert normalize_attestation("kernel") == ATTESTATION_ADAPTER
    assert normalize_attestation("statestream") == ATTESTATION_HA_BRIDGED


# ─── radar-link sensor logic (sensor.py) ──────────────────────────────

RadarLink = None


def _radar_link():
    """Lazily grab the radar-link sensor class (import side-effect free)."""
    global RadarLink
    if RadarLink is None:
        RadarLink = sensor_mod.SecuraCVCanaryRadarLinkSensor
    return RadarLink


def test_radar_frame_age_prefers_explicit_then_derives():
    cls = _radar_link()
    # explicit age used as-is
    assert cls._frame_age_ms({"last_frame_age_ms": 1200}) == 1200
    # derived from device-clock timestamps when no explicit age
    assert cls._frame_age_ms({"now_ms": 5000, "last_frame_ms": 4200}) == 800
    # wrap-safe across a millis() rollover (now wrapped past 2^32)
    age = cls._frame_age_ms({"now_ms": 100, "last_frame_ms": 0xFFFFFF00})
    assert age == 100 + (0x100000000 - 0xFFFFFF00)
    # nothing to compute from → None
    assert cls._frame_age_ms({}) is None


def test_radar_link_state_decision():
    cls = _radar_link()
    # explicit link_ok false is a hard down regardless of age
    assert cls._link_state({"link_ok": False, "last_frame_age_ms": 0}) == "down"
    # fresh frame → ok
    assert cls._link_state({"link_ok": True, "last_frame_age_ms": 200}) == "ok"
    assert cls._link_state({"last_frame_age_ms": 200}) == "ok"
    # frame older than the stale threshold → stale (early warning)
    assert cls._link_state({"last_frame_age_ms": cls.STALE_FRAME_AGE_MS + 1}) == "stale"
    # no judgeable signal → unknown
    assert cls._link_state({}) == "unknown"


def test_device_type_for_reads_cached_status():
    hass = sensor_mod.HomeAssistant()
    entry = types.SimpleNamespace(entry_id="e1")
    hass.data = {
        sensor_mod.DOMAIN: {
            "e1": {
                "devices": {
                    # status stored as the raw JSON string (as __init__.py does)
                    "radar1": {"status": '{"device_type": "canary-sense"}'},
                    # status stored already-parsed as a dict
                    "radar2": {"status": {"device_type": "canary-sense"}},
                    # a non-radar device
                    "cam1": {"status": '{"device_type": "canary-vision"}'},
                    # status without a device_type
                    "old1": {"status": '{"firmware_version": "1.0"}'},
                }
            }
        }
    }
    f = sensor_mod._device_type_for
    assert f(hass, entry, "radar1") == "canary-sense"
    assert f(hass, entry, "radar2") == "canary-sense"
    assert f(hass, entry, "cam1") == "canary-vision"
    assert f(hass, entry, "old1") is None
    assert f(hass, entry, "missing") is None
