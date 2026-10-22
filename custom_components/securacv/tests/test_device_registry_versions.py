"""A Canary's device page: its firmware, its model, and nothing else invented.

Every Canary family publishes its firmware version on its health topic
(``firmware_version``) and none on status, while the integration read the
version from status only, so no Canary's device page showed its firmware
(device-details design §1.3, PR 2). The model was the constant "SecuraCV
Canary" whatever the status's ``device_type`` said. These tests drive the
integration's own status and health subscriptions with each family's real
body shape (``fixtures/canary_mqtt_payloads.json``) and read what reached the
device registry:

  - the firmware comes from health, the status's version only until a health
    has reported one, and a new version (an update) is written again;
  - the model is the product name for the published device_type, kept
    through the last will that names none, and the same rule every entity's
    device_info applies, so an entity cannot put the plain model back;
  - every value is a str, broker text is bounded to 64 printable
    characters, a non-string version is never ``str()``-ed, and no serial
    number is set on a Canary;
  - a heartbeat that changes nothing writes nothing;
  - the health topic still pins the key first (trust on first use).

In the monorepo two more checks hold the fixture and the product-name table
to the firmware: every key a fixture body carries is spelled by the producer
named beside it, and the table's keys are exactly the device types the
firmware configs publish (plus the one legacy display type). The HACS mirror
carries no firmware, so those two skip there; the rest run on the fixtures.
"""

from __future__ import annotations

import json
import re
import types
from pathlib import Path

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import device_registry, make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402

import securacv as package  # noqa: E402
from securacv import async_setup_entry  # noqa: E402
from securacv import entity as entity_mod  # noqa: E402
from securacv.const import (  # noqa: E402
    CANARY_PRODUCT_NAMES,
    CONF_ENABLE_MQTT,
    CONF_MQTT_PREFIX,
    CONF_SETUP_MODE,
    DOMAIN,
    MANUFACTURER,
    MODEL_CANARY,
    SETUP_MODE_MQTT,
    canary_model,
)
from securacv.device_trust import TrustStore  # noqa: E402

FIXTURES = json.loads(
    (Path(__file__).parent / "fixtures" / "canary_mqtt_payloads.json").read_text(encoding="utf-8")
)
FAMILIES: dict[str, dict] = FIXTURES["families"]

# The device page's model per family: the product name for the device_type
# its status names; the flagship's status names none.
EXPECTED_MODEL = {
    "canary": MODEL_CANARY,
    "canary-wap": "Canary WAP",
    "canary-sense": "Canary Sense",
    "canary-vision": "Canary Vision",
    "canary-sentinel": "Canary Sentinel",
    "canary-display": "Canary Dash",
}

# The registry fields a Canary's device carries, each a str when set.
STRING_FIELDS = ("manufacturer", "model", "name", "sw_version", "hw_version", "configuration_url")


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {}
    return hass


def _mqtt_entry(hass: HomeAssistant):
    """An MQTT-only entry, set up through the integration's own setup."""
    entry = make_entry(data={
        CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT,
    })
    hass.config_entries.add(entry)
    assert run(async_setup_entry(hass, entry)) is True
    return entry


def _callbacks(hass: HomeAssistant) -> dict:
    return dict(hass.data["_securacv_test_mqtt_subscriptions"])


def _publish(hass: HomeAssistant, device_id: str, topic: str, body) -> None:
    payload = body if isinstance(body, (str, bytes)) else json.dumps(body)
    callback = _callbacks(hass)[f"securacv/+/{topic}"]
    callback(types.SimpleNamespace(topic=f"securacv/{device_id}/{topic}", payload=payload))


def _device(hass: HomeAssistant, device_id: str):
    return device_registry(hass).async_get_device(identifiers={(DOMAIN, f"canary_{device_id}")})


def _calls_for(hass: HomeAssistant, device_id: str) -> list[dict]:
    wanted = (DOMAIN, f"canary_{device_id}")
    return [c for c in device_registry(hass).calls if wanted in set(c.get("identifiers") or ())]


def _assert_strings(device) -> None:
    for field in STRING_FIELDS:
        value = getattr(device, field, None)
        assert value is None or isinstance(value, str), (field, value)
    assert getattr(device, "serial_number", None) is None, "a Canary's device carries no serial number"


# ── every family, as it publishes ────────────────────────────────────────


@pytest.mark.parametrize("family", sorted(FAMILIES))
def test_every_family_shows_its_firmware_and_its_model(family: str) -> None:
    hass = _hass()
    _mqtt_entry(hass)
    sample = FAMILIES[family]
    device_id = f"{family.replace('-', '')}01"

    _publish(hass, device_id, "status", sample["status"])
    _publish(hass, device_id, "health", sample["health"])

    device = _device(hass, device_id)
    assert device is not None
    assert device.sw_version == sample["health"]["firmware_version"]
    assert device.model == EXPECTED_MODEL[family]
    assert device.manufacturer == MANUFACTURER
    assert device.name == f"SecuraCV Canary {device_id}"
    _assert_strings(device)


def test_health_before_status_still_gives_the_page_its_firmware() -> None:
    """Retained topics arrive in any order: a health first records the
    version, and the status that follows names the device and keeps it."""
    hass = _hass()
    _mqtt_entry(hass)
    sense = FAMILIES["canary-sense"]

    _publish(hass, "sense01", "health", sense["health"])
    assert _device(hass, "sense01").sw_version == "2.4.16"

    _publish(hass, "sense01", "status", sense["status"])
    device = _device(hass, "sense01")
    assert device.sw_version == "2.4.16"
    assert device.model == "Canary Sense"
    assert device.name == "SecuraCV Canary sense01"
    _assert_strings(device)


def test_health_wins_over_status_and_an_update_is_written() -> None:
    hass = _hass()
    _mqtt_entry(hass)
    status = dict(FAMILIES["canary-wap"]["status"], firmware_version="2.4.15")

    # Only a status version so far: the page shows it (older publishers).
    _publish(hass, "wap01", "status", status)
    assert _device(hass, "wap01").sw_version == "2.4.15"

    # Health reports the running version: it wins, and a stale status does not
    # put the old one back.
    _publish(hass, "wap01", "health", dict(FAMILIES["canary-wap"]["health"], firmware_version="2.4.16"))
    assert _device(hass, "wap01").sw_version == "2.4.16"
    _publish(hass, "wap01", "status", status)
    assert _device(hass, "wap01").sw_version == "2.4.16"

    # An update: the next health names the new version, and it is written.
    _publish(hass, "wap01", "health", dict(FAMILIES["canary-wap"]["health"], firmware_version="2.4.17"))
    assert _device(hass, "wap01").sw_version == "2.4.17"


def test_a_heartbeat_that_changes_nothing_writes_nothing() -> None:
    hass = _hass()
    _mqtt_entry(hass)
    sense = FAMILIES["canary-sense"]
    _publish(hass, "sense01", "status", sense["status"])
    _publish(hass, "sense01", "health", sense["health"])
    writes = len(_calls_for(hass, "sense01"))

    for _ in range(3):
        _publish(hass, "sense01", "status", dict(sense["status"], rssi=-70, ts_ms=7200000))
        _publish(hass, "sense01", "health", dict(sense["health"], uptime=7200, memory_free=190000))
    assert len(_calls_for(hass, "sense01")) == writes


# ── the model ────────────────────────────────────────────────────────────


def test_the_last_will_keeps_the_model_and_so_does_a_late_entity() -> None:
    """The WAP's last will is ``{"online": false}`` (csi_mqtt.cpp will_msg):
    no device_type. The model outlives it, on the page and in the
    device_info an entity registered while the WAP is offline would apply."""
    hass = _hass()
    entry = _mqtt_entry(hass)
    _publish(hass, "wap01", "status", FAMILIES["canary-wap"]["status"])
    _publish(hass, "wap01", "status", '{"online":false}')

    assert _device(hass, "wap01").model == "Canary WAP"
    assert entry.runtime_data.devices["wap01"]["online"] is False
    info = entity_mod.canary_device_info(hass, entry, "wap01")
    assert info["model"] == "Canary WAP"
    assert info["name"] == "SecuraCV Canary wap01"


def test_the_model_comes_from_a_table_never_from_the_wire() -> None:
    # Spelling is canonicalized the way the rest of the integration does it.
    assert canary_model("canary_sense") == "Canary Sense"
    assert canary_model(" Canary-Nightstand7 ") == "Canary Nightstand 7"
    # The Dash 7 and the Nightstand Touch publish their siblings' types.
    assert canary_model("canary-dash") == "Canary Dash"
    # Absent, not a string, or a type this build never heard of: the plain
    # model, never the wire string dressed up.
    for unknown in (None, "", 7, {"x": 1}, "canary-something-new", "canary-dash7", "canary"):
        assert canary_model(unknown) == MODEL_CANARY, unknown
    for name in CANARY_PRODUCT_NAMES.values():
        assert isinstance(name, str) and 0 < len(name) <= 64


def test_an_unknown_device_type_keeps_the_plain_model() -> None:
    hass = _hass()
    _mqtt_entry(hass)
    _publish(hass, "newer01", "status", {"online": True, "device_type": "canary-something-new"})
    assert _device(hass, "newer01").model == MODEL_CANARY


# ── hostile or malformed broker text ────────────────────────────────────


def test_broker_text_is_bounded_and_never_stringified() -> None:
    hass = _hass()
    _mqtt_entry(hass)

    _publish(hass, "long01", "health", {"firmware_version": "9" * 500})
    assert _device(hass, "long01").sw_version == "9" * 64

    _publish(hass, "ctl01", "health", {"firmware_version": "2.4.16\x1b[31m\x00\n  beta "})
    assert _device(hass, "ctl01").sw_version == "2.4.16[31m beta"

    # A version that is not a string is no version: never "2.4" from 2.4, and
    # never a dict's repr on a device page.
    for device_id, value in (("num01", 2.4), ("obj01", {"evil": 1}), ("list01", ["2.4.16"]),
                             ("bool01", True), ("blank01", "   ")):
        _publish(hass, device_id, "health", {"firmware_version": value})
        assert _device(hass, device_id) is None, device_id

    # The same rule on status: board and version bounded, a non-string dropped.
    _publish(hass, "stat01", "status", {
        "online": True, "device_type": "canary-sense", "fw_version": "1" * 300,
        "hardware": {"board": "x"}, "board": "b" * 300,
    })
    device = _device(hass, "stat01")
    assert device.sw_version == "1" * 64
    assert device.hw_version == "b" * 64
    _assert_strings(device)


def test_a_rejected_address_never_clears_a_good_one() -> None:
    hass = _hass()
    _mqtt_entry(hass)
    _publish(hass, "sense01", "status", dict(FAMILIES["canary-sense"]["status"], ap_ip="192.168.4.1"))
    assert _device(hass, "sense01").configuration_url == "http://192.168.4.1"
    # A public address is refused (an off-LAN link); the page keeps the last good one.
    _publish(hass, "sense01", "status", dict(FAMILIES["canary-sense"]["status"], ap_ip="8.8.8.8"))
    assert _device(hass, "sense01").configuration_url == "http://192.168.4.1"


def test_a_device_first_heard_on_availability_is_still_named_by_its_status() -> None:
    """The availability topic makes the runtime record first; the status that
    follows must still register the device with its name and model."""
    hass = _hass()
    _mqtt_entry(hass)
    _publish(hass, "base01", "availability", "online")
    assert _device(hass, "base01") is None
    _publish(hass, "base01", "status", dict(FAMILIES["canary-sense"]["status"], device_name="Hall"))
    device = _device(hass, "base01")
    assert device.name == "Hall"
    assert device.model == "Canary Sense"


# ── the pin still comes first ────────────────────────────────────────────


class _TaskHass(HomeAssistant):
    """Runs what a callback schedules, so the TOFU pin is observable."""

    def async_create_task(self, coro):
        return run(coro)


def test_the_health_subscription_pins_the_key_and_records_the_firmware() -> None:
    hass = _TaskHass()
    hass.data = {DOMAIN: {}}
    store = TrustStore(hass, entry_id="e1")
    run(store.async_load())
    entry = make_entry(hass, runtime=make_runtime(hass, trust_store=store))
    health = FAMILIES["canary-vision"]["health"]

    package._async_device_health_received(hass, entry)(
        types.SimpleNamespace(topic="securacv/vision01/health", payload=json.dumps(health))
    )
    assert store.get("vision01").pubkey_hex == health["public_key"]
    assert entry.runtime_data.canary_firmware == {"vision01": "2.4.16"}
    assert _device(hass, "vision01").sw_version == "2.4.16"


def test_a_hostile_health_topic_mints_nothing() -> None:
    hass = _hass()
    entry = _mqtt_entry(hass)
    for topic in ("securacv/../health", "securacv/has space/health", f"securacv/{'x' * 70}/health"):
        _callbacks(hass)["securacv/+/health"](
            types.SimpleNamespace(topic=topic, payload=json.dumps({"firmware_version": "2.4.16"}))
        )
    _callbacks(hass)["securacv/+/health"](types.SimpleNamespace(topic="securacv/bad01/health", payload=b"\xff"))
    assert entry.runtime_data.canary_firmware == {}
    assert device_registry(hass).devices == []


# ── held to the firmware (monorepo only) ─────────────────────────────────

_REPO = Path(__file__).resolve().parents[3]
_FW = _REPO / "firmware"
_monorepo_only = pytest.mark.skipif(
    not (_FW / "configs").is_dir(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)


def _function_body(path: Path, name: str) -> str:
    """The text of C++ function `name` in `path`, from its definition to the
    first closing brace at column 0."""
    text = path.read_text(encoding="utf-8")
    m = re.search(rf"^[^\n;]*\b{re.escape(name)}\([^;{{]*\)\s*\{{(.*?)^\}}", text, re.S | re.M)
    assert m, f"{name}() not found in {path}; update the fixture's producer"
    return m.group(1)


def _keys(body: dict) -> set[str]:
    """Every key of `body`, nested objects' keys included."""
    keys: set[str] = set()
    for key, value in body.items():
        keys.add(key)
        if isinstance(value, dict):
            keys |= _keys(value)
    return keys


@_monorepo_only
@pytest.mark.parametrize("family", sorted(FAMILIES))
@pytest.mark.parametrize("topic", ["status", "health"])
def test_every_fixture_key_is_spelled_by_its_producer(family: str, topic: str) -> None:
    rel, func = FAMILIES[family][f"{topic}_producer"].split("#")
    body = _function_body(_REPO / rel, func)
    for key in sorted(_keys(FAMILIES[family][topic])):
        # snprintf bodies spell \"key\": ; ArduinoJson bodies spell doc["key"].
        assert f'\\"{key}\\":' in body or f'["{key}"]' in body, (
            f"{family} {topic}: {key!r} is not in {rel}#{func}"
        )
    if topic == "health":
        assert "firmware_version" in FAMILIES[family]["health"]


@_monorepo_only
def test_the_product_table_names_exactly_the_published_device_types() -> None:
    published: dict[str, str] = {}
    pattern = re.compile(r'^#define\s+(?:CD_|CS_|SENT_|CONFIG_)DEVICE_TYPE\s+"([^"]+)"', re.M)
    for config in sorted((_FW / "configs").glob("*/*/config.h")):
        for value in pattern.findall(config.read_text(encoding="utf-8")):
            published[value] = str(config.relative_to(_REPO))
    wap = (_FW / "projects/canary-wap/arduino/canary_wap/csi_mqtt.cpp").read_text(encoding="utf-8")
    for value in re.findall(r'\\"device_type\\":\\"([a-z0-9-]+)\\"', wap):
        published[value] = "csi_mqtt.cpp"
    assert published, "no published device type found; the patterns above moved"

    # The one key no current config publishes: the type display images sent
    # before the flavors had their own, which the displays' fleet model still
    # reads as a display.
    legacy = {"canary-display"}
    fleet_model = (_FW / "projects/canary-display/include/canary/fleet/fleet_model.h").read_text(
        encoding="utf-8"
    )
    for value in legacy:
        assert f'"{value}"' in fleet_model, value

    assert set(CANARY_PRODUCT_NAMES) == set(published) | legacy, (
        "CANARY_PRODUCT_NAMES must name every published device_type and no other "
        f"(published: {sorted(published)})"
    )
