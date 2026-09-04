"""Replay high-water marks must survive a restart, not just a session.

The gate in sensor.py refuses a validly signed publish whose counter runs
backwards — "a lower counter cannot be the device's present state". The
marks were kept only in ``entry_data["replay"]``, which
``async_setup_entry`` rebuilds empty, while the pins they belong to
persist. MQTT retains the last signed publish and anyone with publish
access can overwrite a retained topic with an OLDER validly signed message,
so on the next reload ``last is None``, the stale message verified as
current, moved entity state backwards, and re-seeded the floor at its own
low value — the replay window reopening on every restart.

Marks now ride along in the trust entry, which also gets the resets for
free: pin and rotate replace the entry, unpin deletes it.
"""

from __future__ import annotations

import copy
import types

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run

from .test_mqtt_payload_hardening import _install_platform_stubs  # noqa: E402

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
from ..device_trust import TrustVerdict  # noqa: E402
from ..sensor import _replay_gate  # noqa: E402
from ..signature import verify_chain  # noqa: E402

DEVICE = "canary-a1b2"
KEY_A = "11" * 32
KEY_B = "22" * 32

ENTRY = types.SimpleNamespace(
    entry_id="e1",
    data={CONF_SETUP_MODE: SETUP_MODE_MQTT, CONF_ENABLE_MQTT: False},
)

TRUSTED = TrustVerdict(trusted=True, reason="ok", pinned_fingerprint="ab" * 8,
                       received_fingerprint="ab" * 8)


def _persistent_storage(monkeypatch) -> dict:
    """A Store stub whose payloads outlive the instance that wrote them,
    keyed as HA's real one is — without that there is no restart to test."""
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

    # device_trust did `from ... import Store`, so rebind it there.
    monkeypatch.setattr(device_trust, "Store", _Store)
    return saved


def _boot() -> HomeAssistant:
    """One Home Assistant start, through the integration's real setup."""
    hass = HomeAssistant()
    hass.data = {}

    async def _forward(entry, platforms):
        return True

    hass.config_entries = types.SimpleNamespace(async_forward_entry_setups=_forward)
    assert run(async_setup_entry(hass, ENTRY)) is True
    return hass


def _entry_data(hass) -> dict:
    return hass.data[DOMAIN]["e1"]


def _chain(hass, length: int) -> TrustVerdict:
    return _replay_gate(hass, ENTRY, DEVICE, {"length": length}, verify_chain, TRUSTED)


def test_marks_survive_a_restart_and_still_refuse_the_stale_retained_publish(
    monkeypatch,
) -> None:
    _persistent_storage(monkeypatch)

    hass = _boot()
    run(_entry_data(hass)["trust_store"].async_pin(DEVICE, KEY_A))
    assert _chain(hass, 12).trusted is True
    assert _entry_data(hass)["replay"][DEVICE] == {"length": 12}

    # Restart: a brand-new hass and a brand-new entry_data over the same
    # storage. Before the fix the floor came back empty.
    hass2 = _boot()
    assert _entry_data(hass2)["replay"][DEVICE] == {"length": 12}

    # The retained stale-but-signed publish a hostile broker left behind.
    stale = _chain(hass2, 5)
    assert stale.trusted is False
    assert stale.reason == "replay"
    assert "older than the last verified" in stale.detail
    # ...and it did not lower the floor on its way through.
    assert _entry_data(hass2)["replay"][DEVICE] == {"length": 12}
    # The device's real, current publish still passes.
    assert _chain(hass2, 13).trusted is True


def test_a_re_pin_clears_the_persisted_marks_with_the_key(monkeypatch) -> None:
    """The counters belong to the key. A replacement Canary starts them
    over, and must not read as a replay of the device it replaced —
    across a restart as well as within the session."""
    _persistent_storage(monkeypatch)

    hass = _boot()
    store = _entry_data(hass)["trust_store"]
    run(store.async_pin(DEVICE, KEY_A))
    _chain(hass, 12)
    assert store.get(DEVICE).counters == {"length": 12}

    run(store.async_pin(DEVICE, KEY_B))
    assert store.get(DEVICE).counters == {}

    hass2 = _boot()
    assert DEVICE not in _entry_data(hass2)["replay"]
    assert _chain(hass2, 1).trusted is True, "a new key starts its counters over"


def test_unpinning_takes_the_marks_with_it(monkeypatch) -> None:
    _persistent_storage(monkeypatch)

    hass = _boot()
    store = _entry_data(hass)["trust_store"]
    run(store.async_pin(DEVICE, KEY_A))
    _chain(hass, 12)
    assert run(store.async_unpin(DEVICE)) is True

    hass2 = _boot()
    assert _entry_data(hass2)["replay"] == {}


def test_an_unpinned_device_records_nothing(monkeypatch) -> None:
    """No entry, no place to keep a mark — and no crash on the hot path."""
    _persistent_storage(monkeypatch)

    hass = _boot()
    assert _chain(hass, 12).trusted is True
    assert _entry_data(hass)["replay"][DEVICE] == {"length": 12}  # in-session only

    hass2 = _boot()
    assert _entry_data(hass2)["replay"] == {}


def test_a_corrupt_counter_in_storage_is_dropped_not_trusted(monkeypatch) -> None:
    """The gate compares the stored mark with `<`; a string or a bool there
    would raise inside a @callback. Load-time filtering keeps the floor an
    int or absent."""
    saved = _persistent_storage(monkeypatch)

    hass = _boot()
    run(_entry_data(hass)["trust_store"].async_pin(DEVICE, KEY_A))
    _chain(hass, 12)

    key = next(iter(saved))
    saved[key]["devices"][DEVICE]["counters"] = {
        "length": "12",
        "event_id": True,
        "total": 7,
    }

    hass2 = _boot()
    assert _entry_data(hass2)["replay"][DEVICE] == {"total": 7}
