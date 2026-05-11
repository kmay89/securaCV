"""Pytest fixtures for SecuraCV HA tests.

The HA integration imports `homeassistant.*` at module load time. To
run these tests without spinning up a real HA core (and without the
heavyweight `pytest-homeassistant-custom-component` dep), we install
minimal stub modules in sys.modules before the integration is imported.
The stubs implement just enough surface to let TrustStore + signature
verify under test.
"""

from __future__ import annotations

import asyncio
import json
import sys
import types
from pathlib import Path
from typing import Any


def _install_ha_stubs() -> None:
    """Install minimal homeassistant.* + aiohttp stubs in sys.modules.

    Pytest's discovery imports `custom_components.securacv` (the
    package), which runs __init__.py — that pulls in aiohttp and a
    handful of homeassistant.* submodules we don't actually exercise
    in these tests. The stubs are just enough to make those imports
    succeed; the modules under test (device_trust, signature) only
    touch homeassistant.core.HomeAssistant + helpers.storage.Store.
    """
    if "homeassistant" in sys.modules:
        return

    # aiohttp — __init__.py imports `aiohttp` at module top.
    if "aiohttp" not in sys.modules:
        aiohttp_stub = types.ModuleType("aiohttp")
        # Pin a couple of symbols __init__.py references so a Python
        # AttributeError doesn't surface during the test collection.
        class _StubError(Exception):
            pass
        aiohttp_stub.ClientError = _StubError
        aiohttp_stub.ClientResponseError = _StubError
        aiohttp_stub.ClientSession = object
        sys.modules["aiohttp"] = aiohttp_stub

    # voluptuous — used by config_flow but not by the tests we run.
    if "voluptuous" not in sys.modules:
        vol_stub = types.ModuleType("voluptuous")
        vol_stub.Schema = lambda *a, **kw: None
        vol_stub.Required = lambda *a, **kw: None
        vol_stub.Optional = lambda *a, **kw: None
        vol_stub.In = lambda *a, **kw: None
        sys.modules["voluptuous"] = vol_stub

    ha = types.ModuleType("homeassistant")
    ha_core = types.ModuleType("homeassistant.core")
    ha_helpers = types.ModuleType("homeassistant.helpers")
    ha_storage = types.ModuleType("homeassistant.helpers.storage")

    class _HomeAssistant:
        """Just enough for type annotations — TrustStore never calls
        instance methods on hass; it just hands it to Store()."""

        def __init__(self) -> None:
            self.data: dict[str, Any] = {}

    class _Store:
        """In-memory Store stub that survives within a single test
        instance. Writes are reflected on subsequent reads. Matches
        the real Store's `async_load` / `async_save` signatures."""

        def __init__(self, hass: _HomeAssistant, version: int, key: str) -> None:
            self._hass = hass
            self._key = key
            self._version = version
            self._payload: dict[str, Any] | None = None

        async def async_load(self):
            return self._payload

        async def async_save(self, data: dict[str, Any]) -> None:
            # Deep-copy via json to mirror real-store behavior (the
            # real Store serializes to disk, so identity is broken).
            self._payload = json.loads(json.dumps(data))

    ha_core.HomeAssistant = _HomeAssistant
    # `callback` is just an identity decorator at runtime — we mirror
    # that so any `@callback` in the integration is harmless to import.
    ha_core.callback = lambda fn: fn
    ha_storage.Store = _Store

    sys.modules["homeassistant"] = ha
    sys.modules["homeassistant.core"] = ha_core
    sys.modules["homeassistant.helpers"] = ha_helpers
    sys.modules["homeassistant.helpers.storage"] = ha_storage

    # Submodules __init__.py reaches into but the tests don't exercise.
    # We add the minimum surface so import doesn't crash on collection.
    for name, attrs in [
        ("homeassistant.config_entries", {"ConfigEntry": object}),
        ("homeassistant.const", {"CONF_TOKEN": "token", "CONF_URL": "url",
                                 "Platform": types.SimpleNamespace(SENSOR="sensor",
                                                                   BINARY_SENSOR="binary_sensor")}),
        ("homeassistant.components", {}),
        ("homeassistant.components.mqtt", {
            "async_wait_for_mqtt_client": None,
            "async_subscribe": None,
            "ReceiveMessage": object,
        }),
        ("homeassistant.helpers.aiohttp_client", {"async_get_clientsession": None}),
        ("homeassistant.helpers.update_coordinator", {
            "DataUpdateCoordinator": object,
            "UpdateFailed": Exception,
        }),
        ("homeassistant.helpers.device_registry", {
            "async_get": lambda *a, **kw: None,
        }),
    ]:
        mod = types.ModuleType(name)
        for k, v in attrs.items():
            setattr(mod, k, v)
        sys.modules[name] = mod


def _add_repo_root_to_path() -> None:
    """Make `custom_components.securacv` importable from any cwd."""
    repo_root = Path(__file__).resolve().parents[3]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))


_install_ha_stubs()
_add_repo_root_to_path()


# Helpers re-exported for tests ---------------------------------------------

def run(coro):
    """Synchronously drive a coroutine. asyncio.run() is too heavy
    when each test only awaits a few load/save calls; we keep one
    loop per call and tear it down explicitly."""
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()
