"""Pytest fixtures for SecuraCV HA tests.

The HA integration imports `homeassistant.*` at module load time. To
run these tests without spinning up a real HA core (and without the
heavyweight `pytest-homeassistant-custom-component` dep), we install
minimal stub modules in sys.modules before the integration is imported.
The stubs implement just enough surface to let TrustStore + signature
verify under test.

Two layers:
  - `_install_ha_stubs()` installs the base stubs, but only when nothing
    installed them first — the repo-root conftest.py usually wins that
    race (pytest loads it before this file).
  - `_augment_ha_stubs()` ALWAYS runs and upgrades whichever stubs are in
    place with the richer surface the config-flow tests need (a working
    ConfigFlow base, inspectable voluptuous markers, aiohttp.ClientTimeout,
    data_entry_flow.AbortFlow). It is additive/idempotent so it can layer
    on top of either installer without breaking the existing tests.
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


def _augment_ha_stubs() -> None:
    """Upgrade the installed stubs with the config-flow test surface.

    Runs unconditionally (idempotent, additive) because either this
    conftest OR the repo-root conftest may have installed the base
    stubs first, and both bases are too thin for the flow tests:
      - aiohttp gains ClientTimeout/ContentTypeError (the client passes
        aiohttp.ClientTimeout(total=...) instead of deprecated ints);
      - voluptuous gains real marker classes whose key/default a test
        can inspect (to assert an error rebuild kept the typed input);
      - homeassistant.config_entries gains a WORKING ConfigFlow base
        (async_show_form/async_create_entry/async_abort return
        data_entry_flow-ish dicts; unique-id helpers consult a
        `_test_entries` list the test populates);
      - homeassistant.data_entry_flow gains AbortFlow.
    """
    # ── aiohttp extras ───────────────────────────────────────────────
    aiohttp_mod = sys.modules["aiohttp"]
    if not hasattr(aiohttp_mod, "ClientTimeout"):
        class _StubClientTimeout:
            def __init__(self, total=None, **kwargs) -> None:
                self.total = total
        aiohttp_mod.ClientTimeout = _StubClientTimeout
    if not hasattr(aiohttp_mod, "ContentTypeError"):
        aiohttp_mod.ContentTypeError = aiohttp_mod.ClientError

    # ── voluptuous, rich enough to inspect ───────────────────────────
    vol_mod = sys.modules["voluptuous"]
    if not getattr(vol_mod, "_securacv_rich", False):
        _UNDEFINED = object()

        class _Marker:
            """Hashable stand-in for vol.Required/vol.Optional. Records
            the key and the default so tests can assert that a rebuilt
            form preserved what the user typed."""

            def __init__(self, key, default=_UNDEFINED, **kwargs) -> None:
                self.key = key
                if default is _UNDEFINED:
                    self.default = None
                    self.has_default = False
                else:
                    self.default = default() if callable(default) else default
                    self.has_default = True

            def __hash__(self) -> int:
                return hash((type(self).__name__, self.key))

            def __eq__(self, other) -> bool:
                return (
                    type(self) is type(other)
                    and self.key == getattr(other, "key", None)
                )

            def __str__(self) -> str:
                return str(self.key)

            def __repr__(self) -> str:
                return f"{type(self).__name__}({self.key!r})"

        class _Required(_Marker):
            pass

        class _Optional(_Marker):
            pass

        class _Schema:
            """Records the schema verbatim (real vol exposes `.schema`
            too); no validation — the flows only build these."""

            def __init__(self, schema, **kwargs) -> None:
                self.schema = schema

        class _In:
            def __init__(self, container, **kwargs) -> None:
                self.container = container

        vol_mod.Schema = _Schema
        vol_mod.Required = _Required
        vol_mod.Optional = _Optional
        vol_mod.In = _In
        vol_mod._securacv_rich = True

    # ── data_entry_flow.AbortFlow ────────────────────────────────────
    flow_mod = sys.modules.get("homeassistant.data_entry_flow")
    if flow_mod is None:
        flow_mod = types.ModuleType("homeassistant.data_entry_flow")
        sys.modules["homeassistant.data_entry_flow"] = flow_mod
    if not hasattr(flow_mod, "AbortFlow"):
        class _AbortFlow(Exception):
            def __init__(self, reason: str, description_placeholders=None) -> None:
                super().__init__(reason)
                self.reason = reason
                self.description_placeholders = description_placeholders
        flow_mod.AbortFlow = _AbortFlow
    abort_flow_cls = flow_mod.AbortFlow

    # ── a working ConfigFlow base ────────────────────────────────────
    ce_mod = sys.modules["homeassistant.config_entries"]
    if getattr(ce_mod, "_securacv_rich", False):
        return

    class _FlowHandlerBase:
        def __init__(self) -> None:
            self.hass = None
            self.context: dict[str, Any] = {}

        def async_show_form(
            self,
            *,
            step_id: str,
            data_schema=None,
            errors=None,
            description_placeholders=None,
            last_step=None,
        ) -> dict[str, Any]:
            return {
                "type": "form",
                "step_id": step_id,
                "data_schema": data_schema,
                "errors": errors or {},
                "description_placeholders": description_placeholders or {},
            }

        def async_show_menu(
            self, *, step_id: str, menu_options, description_placeholders=None
        ) -> dict[str, Any]:
            return {
                "type": "menu",
                "step_id": step_id,
                "menu_options": menu_options,
            }

        def async_create_entry(
            self, *, title: str, data, options=None, **kwargs
        ) -> dict[str, Any]:
            return {
                "type": "create_entry",
                "title": title,
                "data": data,
                "options": options or {},
            }

        def async_abort(
            self, *, reason: str, description_placeholders=None
        ) -> dict[str, Any]:
            return {
                "type": "abort",
                "reason": reason,
                "description_placeholders": description_placeholders or {},
            }

    class _ConfigFlow(_FlowHandlerBase):
        def __init_subclass__(cls, *, domain=None, **kwargs) -> None:
            super().__init_subclass__(**kwargs)
            cls._domain = domain

        def __init__(self) -> None:
            super().__init__()
            self.unique_id = None
            # Tests append entry stand-ins here (anything with
            # .unique_id and .data) to simulate the entry registry.
            self._test_entries: list[Any] = []

        async def async_set_unique_id(
            self, unique_id, *, raise_on_progress=True
        ):
            self.unique_id = unique_id
            for entry in self._async_current_entries():
                if getattr(entry, "unique_id", None) == unique_id:
                    return entry
            return None

        def _abort_if_unique_id_configured(self, updates=None, **kwargs) -> None:
            for entry in self._async_current_entries():
                if (
                    self.unique_id is not None
                    and getattr(entry, "unique_id", None) == self.unique_id
                ):
                    if updates:
                        new_data = dict(getattr(entry, "data", None) or {})
                        new_data.update(updates)
                        entry.data = new_data
                    raise abort_flow_cls("already_configured")

        def _async_current_entries(self, include_ignore=False):
            return list(self._test_entries)

    class _OptionsFlow(_FlowHandlerBase):
        pass

    class _ConfigEntry:
        """Attribute-bag entry stand-in; tests may also use SimpleNamespace."""

        def __init__(
            self,
            *,
            data=None,
            unique_id=None,
            entry_id="test-entry",
            options=None,
            version=2,
            title="",
        ) -> None:
            self.data = dict(data or {})
            self.unique_id = unique_id
            self.entry_id = entry_id
            self.options = dict(options or {})
            self.version = version
            self.title = title

    ce_mod.ConfigFlow = _ConfigFlow
    ce_mod.OptionsFlow = _OptionsFlow
    ce_mod.ConfigEntry = _ConfigEntry
    ce_mod.ConfigFlowResult = dict
    ce_mod._securacv_rich = True


def _add_repo_root_to_path() -> None:
    """Make `custom_components.securacv` importable from any cwd."""
    repo_root = Path(__file__).resolve().parents[3]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))


_install_ha_stubs()
_augment_ha_stubs()
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
