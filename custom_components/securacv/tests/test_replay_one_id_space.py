"""One event-id space on the firmware, and no change in Home Assistant.

Backlog F46. A Canary's committed csi_events used to take ids from two
allocators: the chokepoint's (1 upward, persisted floor) and the CSI
bundler's (0x80000000 upward, restarted every boot), the second for every
row that went through a bundle (presence, the system.integrity tampers).
Home Assistant's replay gate (sensor.py ``_replay_gate``) refuses a verified
``events`` body whose ``event_id`` is below the last one it verified for the
device, so it refused the chokepoint rows after any bundle, and every
bundle after a reboot.

The firmware now gives every row its id at commit from one allocator that
starts at 0xC0000000 on every device (``kIdSpaceBase`` in
firmware/common/csi/src/csi_event_id_floor.h), above every id an older
firmware handed out. So the stored mark of an upgraded device, wherever the
old firmware left it, is below the device's next id, and the integration
needs no change and no reset. These tests drive the REAL gate through
``_verify_and_record``, with real Ed25519 signatures over the canonical the
firmware signs, a pinned key, and the integration's real setup and storage
across a Home Assistant restart.
"""

from __future__ import annotations

import base64
import copy
import re
import types
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run
from .test_mqtt_payload_hardening import _install_platform_stubs

_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import async_setup_entry  # noqa: E402
from .. import device_trust  # noqa: E402
from ..const import (  # noqa: E402
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    DOMAIN,
    SETUP_MODE_MQTT,
)
from ..sensor import _verify_and_record  # noqa: E402
from ..signature import build_event_canonical, verify_event  # noqa: E402

DEVICE = "canary_a1b2c3"
ENTRY = types.SimpleNamespace(
    entry_id="e1",
    data={CONF_SETUP_MODE: SETUP_MODE_MQTT, CONF_ENABLE_MQTT: False},
)

# The firmware's numbers (csi_event_id_floor.h; the monorepo-only test at the
# end holds these to the header).
ID_SPACE_BASE = 0xC0000000   # the first id of the one space
OLD_BUNDLER_BASE = 0x80000000  # where the old bundler's ids started each boot

_PRIV = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32)
_PUB_HEX = _PRIV.public_key().public_bytes_raw().hex()

_HEADER = (
    Path(__file__).resolve().parents[3]
    / "firmware" / "common" / "csi" / "src" / "csi_event_id_floor.h"
)


def _persistent_storage(monkeypatch) -> None:
    """A Store whose payloads outlive the instance that wrote them, so a
    Home Assistant restart is a real reload of the trust entries (and the
    replay marks they carry)."""
    saved: dict[str, dict] = {}

    class _Store:
        def __init__(self, hass, version, key) -> None:
            self._key = key

        async def async_load(self):
            return copy.deepcopy(saved.get(self._key))

        async def async_save(self, data) -> None:
            saved[self._key] = copy.deepcopy(data)

        def async_delay_save(self, data_func, delay: float = 0) -> None:
            saved[self._key] = copy.deepcopy(data_func())

    monkeypatch.setattr(device_trust, "Store", _Store)


def _boot() -> HomeAssistant:
    """One Home Assistant start, through the integration's real setup."""
    hass = HomeAssistant()
    hass.data = {}

    async def _forward(entry, platforms):
        return True

    hass.config_entries = types.SimpleNamespace(async_forward_entry_setups=_forward)
    assert run(async_setup_entry(hass, ENTRY)) is True
    return hass


def _store(hass):
    return hass.data[DOMAIN]["e1"]["trust_store"]


def _body(hass, event_id: int, state: str = "present") -> dict:
    """An events body as both firmware trees build it (csi_event_wire.h),
    signed over the `event` canonical with the device's witness key."""
    canonical = build_event_canonical(
        DEVICE, event_id, state, "event", "p1", 42, 17, 14,
    )
    sig = base64.urlsafe_b64encode(_PRIV.sign(canonical)).rstrip(b"=").decode("ascii")
    return {
        "event_id": event_id, "event_type": state, "timestamp": 98, "zone": "",
        "confidence": "likely", "signed": True, "module": "core.presence",
        "type": "presence", "category": "event", "privacy": "p1", "state": state,
        "motion": 42, "breathing": 17, "bpm": 14, "duration_sec": 120,
        "bundled": 3, "replay": False, "v": 1, "alg": "ed25519",
        "fp": _store(hass).get(DEVICE).fingerprint_hex, "sig": sig,
    }


def _send(hass, event_id: int):
    """Through the integration's own verify-and-gate path."""
    return _verify_and_record(hass, ENTRY, DEVICE, _body(hass, event_id), verify_event)


def _mark(hass) -> int | None:
    return hass.data[DOMAIN]["e1"]["replay"].get(DEVICE, {}).get("event_id")


def test_the_old_split_id_spaces_are_refused_by_the_real_gate(monkeypatch) -> None:
    """The bug, against the real gate: chokepoint rows after a bundled row,
    and a bundled row after a reboot, are refused as replays."""
    _persistent_storage(monkeypatch)
    hass = _boot()
    run(_store(hass).async_pin(DEVICE, _PUB_HEX, source="manual"))
    assert _send(hass, 40).trusted
    assert _send(hass, 41).trusted
    assert _send(hass, OLD_BUNDLER_BASE + 2).trusted     # one bundled row
    after_bundle = _send(hass, 42)                        # the next chokepoint row
    assert not after_bundle.trusted and after_bundle.reason == "replay"
    after_reboot = _send(hass, OLD_BUNDLER_BASE)          # the bundler restarted
    assert not after_reboot.trusted and after_reboot.reason == "replay"


def test_a_migrated_device_is_accepted_with_nothing_reset(monkeypatch) -> None:
    """The upgrade: the stored mark sits in the old bundler's space, and it
    survives a Home Assistant restart. Nothing is reset, not the mark and
    not the pin. The migrated device's next events, bundled or not, all
    take ids from 0xC0000000 up and are accepted; a replayed old message is
    still refused, and so is a replayed new one."""
    _persistent_storage(monkeypatch)
    hass = _boot()
    store = _store(hass)
    run(store.async_pin(DEVICE, _PUB_HEX, source="manual"))
    for old in (40, 41, OLD_BUNDLER_BASE + 2):
        assert _send(hass, old).trusted
    assert _mark(hass) == OLD_BUNDLER_BASE + 2

    hass = _boot()                                        # HA restarts, marks reload
    assert _mark(hass) == OLD_BUNDLER_BASE + 2
    pin_before = _store(hass).get(DEVICE).fingerprint_hex

    # The upgraded Canary's next rows: direct, bundled, direct.
    for new in (ID_SPACE_BASE, ID_SPACE_BASE + 1, ID_SPACE_BASE + 2):
        verdict = _send(hass, new)
        assert verdict.trusted, verdict
        assert verdict.reason != "replay"
    assert _mark(hass) == ID_SPACE_BASE + 2
    assert _store(hass).get(DEVICE).fingerprint_hex == pin_before

    # Replays: an old bundled id, an old chokepoint id, a new id.
    for stale in (OLD_BUNDLER_BASE + 2, 41, ID_SPACE_BASE):
        verdict = _send(hass, stale)
        assert not verdict.trusted and verdict.reason == "replay", stale
    assert _mark(hass) == ID_SPACE_BASE + 2               # never lowered

    hass = _boot()                                        # and across a restart
    stale = _send(hass, ID_SPACE_BASE + 1)
    assert not stale.trusted and stale.reason == "replay"
    assert _send(hass, ID_SPACE_BASE + 3).trusted


@pytest.mark.parametrize(
    "old_mark",
    [
        1,                                # a device that never bundled
        5_000_000,                        # years of chokepoint ids
        OLD_BUNDLER_BASE,                 # the first bundled row of a boot
        OLD_BUNDLER_BASE + 1_000_000,     # a very long boot's bundles
        ID_SPACE_BASE - 1,                # the old bundler's ceiling, in theory
    ],
)
def test_the_first_new_id_clears_any_mark_an_old_firmware_left(monkeypatch, old_mark) -> None:
    _persistent_storage(monkeypatch)
    hass = _boot()
    run(_store(hass).async_pin(DEVICE, _PUB_HEX, source="manual"))
    assert _send(hass, old_mark).trusted
    assert _send(hass, ID_SPACE_BASE).trusted


@pytest.mark.skipif(
    not _HEADER.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)
def test_the_numbers_are_the_firmware_header() -> None:
    text = _HEADER.read_text(encoding="utf-8")
    base = re.search(r"kIdSpaceBase\s*=\s*(0x[0-9A-Fa-f]+)u", text)
    handle = re.search(r"kHandleBase\s*=\s*(0x[0-9A-Fa-f]+)u", text)
    assert base and int(base.group(1), 16) == ID_SPACE_BASE
    assert handle and int(handle.group(1), 16) == OLD_BUNDLER_BASE
