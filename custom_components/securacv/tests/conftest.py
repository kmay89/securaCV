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
    place with the richer surface the integration's modules need: a working
    ConfigFlow base (reauth/reconfigure helpers, `_set_confirm_only`,
    `async_update_reload_and_abort`), a recording config-entries registry,
    the issue registry, the repairs flow bases, the dispatcher, the device
    registry, `async_redact_data`, a coordinator base that refreshes and
    pushes, MQTT publish/subscribe recorders, and HA's real `@callback`
    marking semantics. It is additive/idempotent so it can layer on top of
    either installer without breaking the existing tests.

The package `__init__` is imported BEFORE this file runs (it is this
conftest's parent), so it keeps its module-level imports to what the base
stubs provide; the kernel-side modules (coordinator, claim, repairs, entity,
config_flow, diagnostics) are imported later and see the full surface.
"""

from __future__ import annotations

import asyncio
import importlib
import json
import os
import sys
import types
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# The HA constant `UNDEFINED` stands in for "argument not given".
_UNDEFINED = object()


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
        instance methods on hass; it just hands it to Store(). The
        services registry and ServiceCall come from _augment_ha_stubs."""

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
        ("homeassistant.helpers.config_validation", {
            "config_entry_only_config_schema": lambda domain: (lambda config: config),
        }),
    ]:
        mod = types.ModuleType(name)
        for k, v in attrs.items():
            setattr(mod, k, v)
        sys.modules[name] = mod


def _module(name: str) -> types.ModuleType:
    """The stub module `name`, created (and attached to its parent) if absent."""
    mod = sys.modules.get(name)
    if mod is None:
        mod = types.ModuleType(name)
        sys.modules[name] = mod
    parent_name, _, child = name.rpartition(".")
    if parent_name:
        parent = _module(parent_name)
        if not hasattr(parent, child):
            setattr(parent, child, mod)
    return mod


def _augment_core(core_mod: types.ModuleType) -> None:
    """homeassistant.core: real @callback semantics, executor, services, config_entries."""
    if not getattr(core_mod, "_securacv_rich", False):

        def _callback(func):
            """HA's real decorator MARKS the target rather than wrapping it,
            and HassJob reads that mark to decide whether to run it on the
            event loop or hand it to a worker thread. An identity stub can't
            tell a decorated target from a bare lambda — which is precisely
            the bug test_watch_tick_schedule.py pins down."""
            func._hass_callback = True
            return func

        def _is_callback(func) -> bool:
            return getattr(func, "_hass_callback", False) is True

        core_mod.callback = _callback
        core_mod.is_callback = _is_callback

        if not hasattr(core_mod.HomeAssistant, "async_add_executor_job"):

            async def _async_add_executor_job(self, target, *args):
                """Runs inline: the tests care that the integration
                DISPATCHED the blocking call, not which thread ran it."""
                return target(*args)

            core_mod.HomeAssistant.async_add_executor_job = _async_add_executor_job
        core_mod._securacv_rich = True

        # The four platforms the package forwards to: the thin repo-root
        # stub knows two. Added before the reload below so PLATFORMS reads
        # every member.
        const_mod = _module("homeassistant.const")
        platform = getattr(const_mod, "Platform", None)
        if platform is None:
            platform = const_mod.Platform = types.SimpleNamespace(
                SENSOR="sensor", BINARY_SENSOR="binary_sensor"
            )
        for member, value in (("EVENT", "event"), ("BUTTON", "button")):
            if not hasattr(platform, member):
                setattr(platform, member, value)

        # The integration package is this conftest's own parent, so Python
        # imported it BEFORE any line here ran — its module-level
        # `@callback` decorations were applied by the identity stub and
        # carry no mark. Re-execute it against the real decorator, or
        # test_watch_tick_schedule.py cannot tell a decorated tick from the
        # bare lambda that shipped the bug (an identity decorator makes the
        # two indistinguishable). reload() re-executes in place, so the
        # module object every other importer holds stays the same one.
        package = sys.modules.get(__name__.rsplit(".", 2)[0])
        if package is not None:
            importlib.reload(package)

    # ── the services surface ─────────────────────────────────────────
    # What services.py registers against and what its handlers receive.
    # Additive: the repo-root conftest installs the same shapes; this
    # layer only fills in whatever base is missing them.
    if not getattr(core_mod, "_securacv_services_rich", False):

        class _ServiceRegistry:
            """Records registrations so a test can find a handler by
            (domain, service); async_call is a no-op (watch_runtime._notify
            schedules one, and tests that care patch _notify)."""

            def __init__(self) -> None:
                self.registered: dict[tuple[str, str], Any] = {}

            def async_register(
                self, domain, service, service_func, schema=None,
                supports_response=None, job_type=None,
            ) -> None:
                self.registered[(domain, service)] = types.SimpleNamespace(
                    func=service_func, schema=schema, supports_response=supports_response
                )

            def has_service(self, domain, service) -> bool:
                return (domain, service) in self.registered

            def async_remove(self, domain, service) -> None:
                self.registered.pop((domain, service), None)

            async def async_call(self, *a, **kw):
                return None

        class _ServiceCall:
            """The pre-2025.1 shape: slotted, and no ``.hass`` — a handler
            binds hass at registration rather than reading it off the
            call, so this leaner shape is the one the stub provides."""

            __slots__ = ("domain", "service", "data", "context", "return_response")

            def __init__(
                self, domain, service, data=None, context=None, return_response=False
            ) -> None:
                self.domain = domain
                self.service = service
                self.data = dict(data or {})
                self.context = context
                self.return_response = return_response

        ha_cls = core_mod.HomeAssistant
        probe = ha_cls()
        if not hasattr(getattr(probe, "services", None), "async_register"):

            def _services(self):
                # Cached per instance, and a property rather than an
                # attribute set in __init__, so a test subclass can still
                # override it with its own (test_tofu_health_hook.py).
                registry = self.__dict__.get("_services")
                if registry is None:
                    registry = self.__dict__["_services"] = _ServiceRegistry()
                return registry

            ha_cls.services = property(_services)
        if not hasattr(ha_cls, "async_create_task"):

            def _async_create_task(self, coro):
                try:
                    coro.close()
                except Exception:
                    pass
                return None

            ha_cls.async_create_task = _async_create_task
        if not hasattr(core_mod, "ServiceCall"):
            core_mod.ServiceCall = _ServiceCall
        if not hasattr(core_mod, "SupportsResponse"):
            core_mod.SupportsResponse = types.SimpleNamespace(
                NONE="none", OPTIONAL="optional", ONLY="only"
            )
        core_mod._securacv_services_rich = True

    # ── hass.config_entries: a recording registry ─────────────────────
    # Tests that build their own `hass.config_entries = SimpleNamespace(...)`
    # keep working (an instance attribute shadows the property).
    ha_cls = core_mod.HomeAssistant
    if not getattr(ha_cls, "_securacv_config_entries_rich", False):

        def _config_entries(self):
            registry = self.__dict__.get("_config_entries")
            if registry is None:
                registry = self.__dict__["_config_entries"] = ConfigEntriesStub(self)
            return registry

        def _set_config_entries(self, value):
            self.__dict__["_config_entries"] = value

        ha_cls.config_entries = property(_config_entries, _set_config_entries)
        ha_cls._securacv_config_entries_rich = True
    if not hasattr(core_mod, "CALLBACK_TYPE"):
        core_mod.CALLBACK_TYPE = object

    # ── hass.config: a configuration directory per instance ────────────
    # `hass.config.path(*parts)` is where the evidence packs land; the stub
    # hands each HomeAssistant its own temporary directory and lets a test
    # assign `hass.config` outright.
    if not getattr(ha_cls, "_securacv_config_rich", False):

        def _config(self):
            config = self.__dict__.get("_config")
            if config is None:
                import tempfile

                root = tempfile.mkdtemp(prefix="securacv-ha-config-")
                config = self.__dict__["_config"] = types.SimpleNamespace(
                    config_dir=root,
                    path=lambda *parts: os.path.join(root, *parts),
                    time_zone="UTC",
                    language="en",
                )
            return config

        def _set_config(self, value):
            self.__dict__["_config"] = value

        ha_cls.config = property(_config, _set_config)
        ha_cls._securacv_config_rich = True


class ConfigEntriesStub:
    """What `hass.config_entries` offers the integration, recorded.

    `entries` holds every entry a test adds (``add``); loaded entries are
    the ones carrying a ``runtime_data``. Updates apply to the entry object
    and are recorded in ``updates``; reload requests land in ``reloads``.
    """

    def __init__(self, hass) -> None:
        self.hass = hass
        self.entries: list[Any] = []
        self.updates: list[tuple[Any, dict[str, Any]]] = []
        self.reloads: list[str] = []
        self.forwarded: list[tuple[Any, list[Any]]] = []
        self.unloaded: list[tuple[Any, list[Any]]] = []

    def add(self, *entries: Any) -> None:
        for entry in entries:
            if entry not in self.entries:
                self.entries.append(entry)

    def async_entries(self, domain: str | None = None, **_kw: Any) -> list[Any]:
        return [
            entry
            for entry in self.entries
            if domain is None or getattr(entry, "domain", "securacv") == domain
        ]

    def async_loaded_entries(self, domain: str) -> list[Any]:
        return [e for e in self.async_entries(domain) if hasattr(e, "runtime_data")]

    def async_get_entry(self, entry_id: str) -> Any | None:
        for entry in self.entries:
            if getattr(entry, "entry_id", None) == entry_id:
                return entry
        return None

    def async_get_known_entry(self, entry_id: str) -> Any:
        entry = self.async_get_entry(entry_id)
        if entry is None:
            raise KeyError(entry_id)
        return entry

    def async_entry_for_domain_unique_id(self, domain: str, unique_id: str) -> Any | None:
        for entry in self.async_entries(domain):
            if getattr(entry, "unique_id", None) == unique_id:
                return entry
        return None

    def async_update_entry(self, entry: Any, **kwargs: Any) -> bool:
        changed = False
        self.updates.append((entry, kwargs))
        for attr in ("data", "options", "unique_id", "version", "title", "minor_version"):
            if attr in kwargs:
                value = kwargs[attr]
                if attr in ("data", "options") and value is not None:
                    value = dict(value)
                if getattr(entry, attr, _UNDEFINED) != value:
                    setattr(entry, attr, value)
                    changed = True
        return changed

    def async_schedule_reload(self, entry_id: str) -> None:
        self.reloads.append(entry_id)

    async def async_reload(self, entry_id: str) -> bool:
        self.reloads.append(entry_id)
        return True

    async def async_forward_entry_setups(self, entry: Any, platforms: Iterable[Any]) -> None:
        self.forwarded.append((entry, list(platforms)))

    async def async_unload_platforms(self, entry: Any, platforms: Iterable[Any]) -> bool:
        self.unloaded.append((entry, list(platforms)))
        return True


def _augment_exceptions() -> None:
    exc_mod = _module("homeassistant.exceptions")
    if not hasattr(exc_mod, "HomeAssistantError"):
        class _HomeAssistantError(Exception):
            """HA's constructor: a positional message plus the translation
            triple (the repo-root conftest's stub has the same shape)."""

            def __init__(
                self,
                *args,
                translation_domain=None,
                translation_key=None,
                translation_placeholders=None,
            ) -> None:
                super().__init__(*args)
                self.translation_domain = translation_domain
                self.translation_key = translation_key
                self.translation_placeholders = translation_placeholders
        exc_mod.HomeAssistantError = _HomeAssistantError
    if not hasattr(exc_mod, "ServiceValidationError"):
        class _ServiceValidationError(exc_mod.HomeAssistantError):
            pass
        exc_mod.ServiceValidationError = _ServiceValidationError
    for name in ("ConfigEntryError", "ConfigEntryNotReady", "ConfigEntryAuthFailed"):
        if not hasattr(exc_mod, name):
            setattr(exc_mod, name, type(name, (exc_mod.HomeAssistantError,), {}))


def _augment_config_validation() -> None:
    cv_mod = _module("homeassistant.helpers.config_validation")
    for name, validator in (
        ("string", str),
        ("boolean", bool),
        ("positive_float", float),
        ("positive_int", int),
        ("config_entry_only_config_schema", lambda domain: (lambda config: config)),
    ):
        if not hasattr(cv_mod, name):
            setattr(cv_mod, name, validator)


def _augment_storage() -> None:
    store_cls = sys.modules["homeassistant.helpers.storage"].Store
    if not hasattr(store_cls, "async_delay_save"):

        def _async_delay_save(self, data_func, delay: float = 0) -> None:
            """The real one is a @callback that schedules a coalesced write.
            The stub writes through immediately: tests assert what LANDED in
            the store, not when the loop got round to it."""
            self._payload = json.loads(json.dumps(data_func()))

        store_cls.async_delay_save = _async_delay_save
    if not hasattr(store_cls, "async_remove"):

        async def _async_remove(self) -> None:
            self._payload = None

        store_cls.async_remove = _async_remove


def _augment_event() -> None:
    event_mod = _module("homeassistant.helpers.event")
    if not hasattr(event_mod, "async_track_time_interval"):
        # Returns the unsubscribe callable the real helper does; tests that
        # inspect the scheduled action monkeypatch this.
        event_mod.async_track_time_interval = (
            lambda hass, action, interval, **kw: (lambda: None)
        )
    if not hasattr(event_mod, "async_call_later"):
        event_mod.async_call_later = lambda hass, delay, action: (lambda: None)


def _augment_aiohttp() -> None:
    aiohttp_mod = sys.modules["aiohttp"]
    if not hasattr(aiohttp_mod, "ClientTimeout"):
        class _StubClientTimeout:
            def __init__(self, total=None, **kwargs) -> None:
                self.total = total
        aiohttp_mod.ClientTimeout = _StubClientTimeout
    if not hasattr(aiohttp_mod, "ContentTypeError"):
        aiohttp_mod.ContentTypeError = aiohttp_mod.ClientError
    # aiohttp.web: what the evidence view returns and raises.
    web_mod = sys.modules.get("aiohttp.web")
    if web_mod is None or not hasattr(web_mod, "FileResponse"):
        web_mod = web_mod or types.ModuleType("aiohttp.web")

        class _HTTPException(Exception):
            status = 500

        class _HTTPNotFound(_HTTPException):
            status = 404

        class _StreamResponse:
            def __init__(self, *, status=200, headers=None) -> None:
                self.status = status
                self.headers = dict(headers or {})

        class _FileResponse(_StreamResponse):
            """Records the path and headers the view served."""

            def __init__(self, path, *, headers=None, status=200, **kwargs) -> None:
                super().__init__(status=status, headers=headers)
                self.path = path

        web_mod.Request = object
        web_mod.StreamResponse = _StreamResponse
        web_mod.FileResponse = _FileResponse
        web_mod.HTTPException = _HTTPException
        web_mod.HTTPNotFound = _HTTPNotFound
        sys.modules["aiohttp.web"] = web_mod
        aiohttp_mod.web = web_mod


def _augment_voluptuous() -> None:
    vol_mod = sys.modules["voluptuous"]
    if getattr(vol_mod, "_securacv_rich", False):
        return
    _MISSING = object()

    class _Marker:
        """Hashable stand-in for vol.Required/vol.Optional. Records
        the key and the default so tests can assert that a rebuilt
        form preserved what the user typed."""

        def __init__(self, key, default=_MISSING, **kwargs) -> None:
            self.key = key
            if default is _MISSING:
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
    vol_mod.All = lambda *validators: validators
    vol_mod.Range = lambda **kw: kw
    vol_mod.Match = lambda pattern, **kw: pattern
    vol_mod.Coerce = lambda type_: type_
    vol_mod._securacv_rich = True


def _augment_data_entry_flow() -> None:
    flow_mod = _module("homeassistant.data_entry_flow")
    if not hasattr(flow_mod, "AbortFlow"):
        class _AbortFlow(Exception):
            def __init__(self, reason: str, description_placeholders=None) -> None:
                super().__init__(reason)
                self.reason = reason
                self.description_placeholders = description_placeholders
        flow_mod.AbortFlow = _AbortFlow
    if not hasattr(flow_mod, "FlowResult"):
        flow_mod.FlowResult = dict
    if not hasattr(flow_mod, "FlowResultType"):
        flow_mod.FlowResultType = types.SimpleNamespace(
            FORM="form", CREATE_ENTRY="create_entry", ABORT="abort", MENU="menu"
        )


class _FlowHandlerBase:
    """The flow-result builders every flow kind shares."""

    def __init__(self) -> None:
        self.hass = None
        self.context: dict[str, Any] = {}
        self.confirm_only = False

    def async_show_form(
        self,
        *,
        step_id: str | None = None,
        data_schema=None,
        errors=None,
        description_placeholders=None,
        last_step=None,
        preview=None,
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
        self,
        *,
        title: str | None = None,
        data,
        description=None,
        description_placeholders=None,
        options=None,
        **kwargs,
    ) -> dict[str, Any]:
        return {
            "type": "create_entry",
            "title": title,
            "data": data,
            "options": options or {},
            "description": description,
            "description_placeholders": description_placeholders or {},
        }

    def async_abort(
        self, *, reason: str, description_placeholders=None
    ) -> dict[str, Any]:
        return {
            "type": "abort",
            "reason": reason,
            "description_placeholders": description_placeholders or {},
        }

    def _set_confirm_only(self) -> None:
        self.confirm_only = True


def _augment_config_entries() -> None:
    ce_mod = _module("homeassistant.config_entries")
    for name, value in (
        ("SOURCE_USER", "user"),
        ("SOURCE_MQTT", "mqtt"),
        ("SOURCE_HASSIO", "hassio"),
        ("SOURCE_REAUTH", "reauth"),
        ("SOURCE_RECONFIGURE", "reconfigure"),
    ):
        if not hasattr(ce_mod, name):
            setattr(ce_mod, name, value)
    if not hasattr(ce_mod, "ConfigEntryState"):
        ce_mod.ConfigEntryState = types.SimpleNamespace(
            LOADED="loaded", NOT_LOADED="not_loaded", SETUP_RETRY="setup_retry"
        )
    if getattr(ce_mod, "_securacv_rich", False):
        return
    abort_flow_cls = sys.modules["homeassistant.data_entry_flow"].AbortFlow

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
            # Entry ids whose data changed under reload_on_update=True.
            self._test_reloads: list[str] = []

        @property
        def source(self):
            return self.context.get("source")

        async def async_set_unique_id(
            self, unique_id, *, raise_on_progress=True
        ):
            self.unique_id = unique_id
            for entry in self._async_current_entries():
                if getattr(entry, "unique_id", None) == unique_id:
                    return entry
            return None

        def _abort_if_unique_id_configured(
            self,
            updates=None,
            reload_on_update=True,
            *,
            error="already_configured",
            description_placeholders=None,
        ) -> None:
            for entry in self._async_current_entries():
                if (
                    self.unique_id is not None
                    and getattr(entry, "unique_id", None) == self.unique_id
                ):
                    changed = False
                    if updates:
                        new_data = dict(getattr(entry, "data", None) or {})
                        changed = any(new_data.get(k) != v for k, v in updates.items())
                        new_data.update(updates)
                        entry.data = new_data
                    if changed and reload_on_update:
                        self._test_reloads.append(getattr(entry, "entry_id", None))
                        registry = getattr(self.hass, "config_entries", None)
                        schedule = getattr(registry, "async_schedule_reload", None)
                        if schedule is not None:
                            schedule(getattr(entry, "entry_id", None))
                    raise abort_flow_cls(error, description_placeholders)

        def _abort_if_unique_id_mismatch(self, *, reason="unique_id_mismatch", description_placeholders=None):
            entry = self._reauth_or_reconfigure_entry()
            if getattr(entry, "unique_id", None) != self.unique_id:
                raise abort_flow_cls(reason, description_placeholders)

        def _async_current_entries(self, include_ignore=False):
            return list(self._test_entries)

        def _reauth_or_reconfigure_entry(self):
            entry_id = self.context.get("entry_id")
            for entry in self._async_current_entries():
                if getattr(entry, "entry_id", None) == entry_id:
                    return entry
            raise ValueError("Source is not reauth/reconfigure or no entry in context")

        def _get_reauth_entry(self):
            return self._reauth_or_reconfigure_entry()

        def _get_reconfigure_entry(self):
            return self._reauth_or_reconfigure_entry()

        def async_update_reload_and_abort(
            self,
            entry,
            *,
            unique_id=_UNDEFINED,
            title=_UNDEFINED,
            data=_UNDEFINED,
            data_updates=_UNDEFINED,
            options=_UNDEFINED,
            reason=_UNDEFINED,
            reload_even_if_entry_is_unchanged=True,
        ):
            if data_updates is not _UNDEFINED:
                if data is not _UNDEFINED:
                    raise ValueError("Cannot set both data and data_updates")
                data = {**(getattr(entry, "data", None) or {}), **data_updates}
            kwargs: dict[str, Any] = {}
            if data is not _UNDEFINED:
                kwargs["data"] = data
            if unique_id is not _UNDEFINED:
                kwargs["unique_id"] = unique_id
            if title is not _UNDEFINED:
                kwargs["title"] = title
            if options is not _UNDEFINED:
                kwargs["options"] = options
            registry = getattr(self.hass, "config_entries", None)
            update = getattr(registry, "async_update_entry", None)
            if update is not None:
                update(entry, **kwargs)
            else:
                for attr, value in kwargs.items():
                    setattr(entry, attr, value)
            self._test_reloads.append(getattr(entry, "entry_id", None))
            schedule = getattr(registry, "async_schedule_reload", None)
            if schedule is not None:
                schedule(getattr(entry, "entry_id", None))
            if reason is _UNDEFINED:
                reason = (
                    "reauth_successful"
                    if self.source == ce_mod.SOURCE_REAUTH
                    else "reconfigure_successful"
                )
            return self.async_abort(reason=reason)

    class _OptionsFlow(_FlowHandlerBase):
        """The core's shape since 2024.11: no entry in __init__; the
        `config_entry` property resolves the flow's `handler` (the entry
        id the options-flow manager assigns) through hass.config_entries."""

        handler: str | None = None

        @property
        def config_entry(self):
            if self.hass is None or self.handler is None:
                raise ValueError("The config entry is not available during initialization")
            return self.hass.config_entries.async_get_known_entry(self.handler)

    class _ConfigEntry:
        """Attribute-bag entry stand-in; tests may also use SimpleNamespace."""

        domain = "securacv"

        def __init__(
            self,
            *,
            data=None,
            unique_id=None,
            entry_id="test-entry",
            options=None,
            version=3,
            title="",
            source="user",
        ) -> None:
            self.data = dict(data or {})
            self.unique_id = unique_id
            self.entry_id = entry_id
            self.options = dict(options or {})
            self.version = version
            self.title = title
            self.source = source
            self.minor_version = 1

    ce_mod.ConfigFlow = _ConfigFlow
    ce_mod.OptionsFlow = _OptionsFlow
    ce_mod.ConfigEntry = _ConfigEntry
    ce_mod.ConfigFlowResult = dict
    ce_mod._securacv_rich = True


def _augment_issue_registry() -> None:
    ir_mod = _module("homeassistant.helpers.issue_registry")
    if getattr(ir_mod, "_securacv_rich", False):
        return

    class _IssueSeverity:
        CRITICAL = "critical"
        ERROR = "error"
        WARNING = "warning"

    def _issues(hass) -> dict[tuple[str, str], dict[str, Any]]:
        return hass.data.setdefault("_securacv_test_issues", {})

    def async_create_issue(hass, domain, issue_id, **kwargs) -> None:
        _issues(hass)[(domain, issue_id)] = dict(kwargs)

    def async_delete_issue(hass, domain, issue_id) -> None:
        _issues(hass).pop((domain, issue_id), None)

    class _Registry:
        def __init__(self, hass) -> None:
            self._hass = hass

        def async_get_issue(self, domain, issue_id):
            issue = _issues(self._hass).get((domain, issue_id))
            if issue is None:
                return None
            return types.SimpleNamespace(
                domain=domain,
                issue_id=issue_id,
                translation_placeholders=issue.get("translation_placeholders"),
                data=issue.get("data"),
                **{k: v for k, v in issue.items() if k not in ("translation_placeholders", "data")},
            )

    ir_mod.IssueSeverity = _IssueSeverity
    ir_mod.async_create_issue = async_create_issue
    ir_mod.async_delete_issue = async_delete_issue
    ir_mod.async_get = lambda hass: _Registry(hass)
    ir_mod._securacv_rich = True


def _augment_repairs() -> None:
    repairs_mod = _module("homeassistant.components.repairs")
    if getattr(repairs_mod, "_securacv_rich", False):
        return
    ir_mod = sys.modules["homeassistant.helpers.issue_registry"]

    class _RepairsFlow(_FlowHandlerBase):
        def __init__(self) -> None:
            super().__init__()
            self.issue_id = ""
            self.handler = ""
            self.data = None

    class _ConfirmRepairFlow(_RepairsFlow):
        async def async_step_init(self, user_input=None):
            return await self.async_step_confirm()

        async def async_step_confirm(self, user_input=None):
            if user_input is not None:
                return self.async_create_entry(data={})
            placeholders = None
            issue = ir_mod.async_get(self.hass).async_get_issue(self.handler, self.issue_id)
            if issue is not None:
                placeholders = issue.translation_placeholders
            return self.async_show_form(
                step_id="confirm", data_schema=None, description_placeholders=placeholders
            )

    repairs_mod.RepairsFlow = _RepairsFlow
    repairs_mod.ConfirmRepairFlow = _ConfirmRepairFlow
    repairs_mod._securacv_rich = True


def _augment_diagnostics() -> None:
    diag_mod = _module("homeassistant.components.diagnostics")
    if hasattr(diag_mod, "async_redact_data"):
        return
    REDACTED = "**REDACTED**"

    def async_redact_data(data, to_redact):
        """Byte-for-byte the algorithm in homeassistant/components/diagnostics/util.py."""
        if not isinstance(data, (Mapping, list)):
            return data
        if isinstance(data, list):
            return [async_redact_data(val, to_redact) for val in data]
        redacted = {**data}
        for key, value in redacted.items():
            if value is None:
                continue
            if isinstance(value, str) and not value:
                continue
            if key in to_redact:
                redacted[key] = REDACTED
            elif isinstance(value, Mapping):
                redacted[key] = async_redact_data(value, to_redact)
            elif isinstance(value, list):
                redacted[key] = [async_redact_data(item, to_redact) for item in value]
        return redacted

    diag_mod.REDACTED = REDACTED
    diag_mod.async_redact_data = async_redact_data


def _augment_dispatcher() -> None:
    disp_mod = _module("homeassistant.helpers.dispatcher")
    if hasattr(disp_mod, "async_dispatcher_connect"):
        return

    def _targets(hass) -> dict[str, list[Any]]:
        return hass.data.setdefault("_securacv_test_dispatcher", {})

    def async_dispatcher_connect(hass, signal, target):
        _targets(hass).setdefault(signal, []).append(target)

        def _unsub() -> None:
            listeners = _targets(hass).get(signal, [])
            if target in listeners:
                listeners.remove(target)

        return _unsub

    def async_dispatcher_send(hass, signal, *args) -> None:
        for target in list(_targets(hass).get(signal, [])):
            target(*args)

    disp_mod.async_dispatcher_connect = async_dispatcher_connect
    disp_mod.async_dispatcher_send = async_dispatcher_send
    disp_mod.dispatcher_send = async_dispatcher_send


def _augment_service_info() -> None:
    mqtt_info = _module("homeassistant.helpers.service_info.mqtt")
    if not hasattr(mqtt_info, "MqttServiceInfo"):

        @dataclass(slots=True)
        class MqttServiceInfo:
            """Mirror of the real slots dataclass: attribute access only."""

            topic: str
            payload: Any
            qos: int
            retain: bool
            subscribed_topic: str
            timestamp: float

        mqtt_info.MqttServiceInfo = MqttServiceInfo
    hassio_info = _module("homeassistant.helpers.service_info.hassio")
    if not hasattr(hassio_info, "HassioServiceInfo"):

        @dataclass(slots=True)
        class HassioServiceInfo:
            config: dict[str, Any]
            name: str
            slug: str
            uuid: str

        hassio_info.HassioServiceInfo = HassioServiceInfo


class DeviceRegistryStub:
    """Records async_get_or_create calls and answers identifier lookups."""

    def __init__(self) -> None:
        self.devices: list[types.SimpleNamespace] = []
        self.calls: list[dict[str, Any]] = []
        self._next = 1

    def async_get_or_create(self, **kwargs):
        self.calls.append(dict(kwargs))
        identifiers = set(kwargs.get("identifiers") or ())
        for device in self.devices:
            if identifiers & device.identifiers:
                device.identifiers |= identifiers
                for key, value in kwargs.items():
                    if key not in ("identifiers", "config_entry_id") and value is not None:
                        setattr(device, key, value)
                return device
        device = types.SimpleNamespace(
            id=f"dev{self._next}",
            identifiers=identifiers,
            config_entry_id=kwargs.get("config_entry_id"),
            **{k: v for k, v in kwargs.items() if k not in ("identifiers", "config_entry_id")},
        )
        self._next += 1
        self.devices.append(device)
        return device

    def async_get_device(self, identifiers=None, connections=None):
        wanted = set(identifiers or ())
        for device in self.devices:
            if wanted & device.identifiers:
                return device
        return None

    def async_get_device_by_identifier(self, identifier, config_entry_id):
        for device in self.devices:
            if identifier in device.identifiers and device.config_entry_id == config_entry_id:
                return device
        return None


def _augment_device_registry() -> None:
    dr_mod = _module("homeassistant.helpers.device_registry")
    if getattr(dr_mod, "_securacv_rich", False):
        return

    class DeviceInfo(dict):
        """The pre-2026.8 shape by default (`via_device`); a test flips the
        annotations to `via_device_id` to drive the other branch of the shim."""

        __annotations__ = {
            "identifiers": set,
            "manufacturer": str,
            "model": str,
            "name": str,
            "configuration_url": str,
            "serial_number": str,
            "sw_version": str,
            "hw_version": str,
            "via_device": tuple,
        }

    def async_get(hass):
        registry = hass.data.get("_securacv_test_device_registry")
        if registry is None:
            registry = hass.data["_securacv_test_device_registry"] = DeviceRegistryStub()
        return registry

    def async_get_device_id_by_identifier(hass, identifier, *, config_entry_id):
        device = async_get(hass).async_get_device_by_identifier(identifier, config_entry_id)
        if device is None:
            raise ValueError(f"no device {identifier} in {config_entry_id}")
        return device.id

    dr_mod.DeviceInfo = DeviceInfo
    dr_mod.DeviceEntry = types.SimpleNamespace
    dr_mod.async_get = async_get
    dr_mod.async_get_device_id_by_identifier = async_get_device_id_by_identifier
    dr_mod._securacv_rich = True
    entity_mod = _module("homeassistant.helpers.entity")
    if not hasattr(entity_mod, "DeviceInfo"):
        entity_mod.DeviceInfo = DeviceInfo


def _augment_mqtt() -> None:
    mqtt_mod = _module("homeassistant.components.mqtt")
    if getattr(mqtt_mod, "_securacv_rich", False):
        return
    mqtt_mod.DOMAIN = "mqtt"

    async def async_wait_for_mqtt_client(hass) -> bool:
        return bool(hass.data.get("_securacv_test_mqtt_available", True))

    async def async_subscribe(hass, topic, msg_callback, qos=0, encoding="utf-8"):
        hass.data.setdefault("_securacv_test_mqtt_subscriptions", []).append((topic, msg_callback))
        return lambda: None

    async def async_publish(hass, topic, payload, qos=0, retain=False, encoding="utf-8", **kw):
        hass.data.setdefault("_securacv_test_mqtt_published", []).append(
            {"topic": topic, "payload": payload, "qos": qos, "retain": retain}
        )

    @dataclass(slots=True)
    class ReceiveMessage:
        topic: str
        payload: Any
        qos: int = 0
        retain: bool = False
        subscribed_topic: str = ""
        timestamp: float = 0.0

    if getattr(mqtt_mod, "async_wait_for_mqtt_client", None) is None:
        mqtt_mod.async_wait_for_mqtt_client = async_wait_for_mqtt_client
    if getattr(mqtt_mod, "async_subscribe", None) is None:
        mqtt_mod.async_subscribe = async_subscribe
    if not hasattr(mqtt_mod, "async_publish"):
        mqtt_mod.async_publish = async_publish
    if getattr(mqtt_mod, "ReceiveMessage", None) is object:
        mqtt_mod.ReceiveMessage = ReceiveMessage
    mqtt_mod._securacv_rich = True


class _EntityBase:
    """What Entity gives every platform class: the lifecycle hooks and state writes."""

    hass: Any = None

    @property
    def unique_id(self) -> str | None:
        """HA's Entity reads the `_attr_` field; so does the stub."""
        return getattr(self, "_attr_unique_id", None)

    async def async_added_to_hass(self) -> None:
        return None

    async def async_will_remove_from_hass(self) -> None:
        return None

    def async_on_remove(self, func) -> None:
        self.__dict__.setdefault("_on_remove", []).append(func)

    def async_write_ha_state(self) -> None:
        return None

    @property
    def available(self) -> bool:
        return True


def _augment_update_coordinator() -> None:
    uc_mod = _module("homeassistant.helpers.update_coordinator")
    exc_mod = sys.modules["homeassistant.exceptions"]
    duc = uc_mod.DataUpdateCoordinator
    if not getattr(duc, "_securacv_rich", False):
        # The base conftest's class is bare; give it a faithful surface
        # by attribute so every subclass already imported picks it up.
        def __init__(self, hass, logger=None, *, name="", update_interval=None, config_entry=None, **kw):
            self.hass = hass
            self.logger = logger
            self.name = name
            self.update_interval = update_interval
            self.config_entry = config_entry
            self.data = None
            self.last_update_success = True
            self.last_exception = None
            self.auth_failed = None
            self._listeners: list[Any] = []

        async def _async_update_data(self):
            raise NotImplementedError

        async def async_refresh(self):
            try:
                self.data = await self._async_update_data()
            except uc_mod.UpdateFailed as err:
                self.last_update_success = False
                self.last_exception = err
            except exc_mod.ConfigEntryAuthFailed as err:
                self.last_update_success = False
                self.last_exception = err
                self.auth_failed = err
            else:
                self.last_update_success = True
                self.last_exception = None
            self.async_update_listeners()

        async def async_config_entry_first_refresh(self):
            try:
                self.data = await self._async_update_data()
            except uc_mod.UpdateFailed as err:
                self.last_update_success = False
                raise exc_mod.ConfigEntryNotReady(str(err)) from err
            self.last_update_success = True
            self.async_update_listeners()

        async def async_request_refresh(self):
            await self.async_refresh()

        def async_set_updated_data(self, data):
            self.data = data
            self.last_update_success = True
            self.async_update_listeners()

        def async_set_update_error(self, err):
            self.last_update_success = False
            self.last_exception = err
            self.async_update_listeners()

        def async_add_listener(self, update_callback, context=None):
            self._listeners.append(update_callback)

            def _remove() -> None:
                if update_callback in self._listeners:
                    self._listeners.remove(update_callback)

            return _remove

        def async_update_listeners(self):
            for listener in list(getattr(self, "_listeners", [])):
                listener()

        for name, func in (
            ("__init__", __init__),
            ("_async_update_data", _async_update_data),
            ("async_refresh", async_refresh),
            ("async_config_entry_first_refresh", async_config_entry_first_refresh),
            ("async_request_refresh", async_request_refresh),
            ("async_set_updated_data", async_set_updated_data),
            ("async_set_update_error", async_set_update_error),
            ("async_add_listener", async_add_listener),
            ("async_update_listeners", async_update_listeners),
        ):
            setattr(duc, name, func)
        if not hasattr(duc, "__class_getitem__"):
            duc.__class_getitem__ = classmethod(lambda cls, item: cls)
        duc._securacv_rich = True

    if getattr(uc_mod, "CoordinatorEntity", object) is object:

        class CoordinatorEntity(_EntityBase):
            def __init__(self, coordinator, context=None) -> None:
                self.coordinator = coordinator

            def __class_getitem__(cls, item):
                return cls

            @property
            def available(self) -> bool:
                return bool(getattr(self.coordinator, "last_update_success", True))

            async def async_added_to_hass(self) -> None:
                await super().async_added_to_hass()
                add = getattr(self.coordinator, "async_add_listener", None)
                if add is not None:
                    self.async_on_remove(add(self._handle_coordinator_update))

            def _handle_coordinator_update(self) -> None:
                self.async_write_ha_state()

        uc_mod.CoordinatorEntity = CoordinatorEntity


def _augment_platform_bases() -> None:
    """sensor / binary_sensor entity bases with the Entity lifecycle hooks."""
    enum_ns = types.SimpleNamespace
    sensor_mod = _module("homeassistant.components.sensor")
    if not hasattr(sensor_mod, "SensorEntity"):
        sensor_mod.SensorEntity = type("SensorEntity", (_EntityBase,), {})
    if not hasattr(sensor_mod, "SensorDeviceClass"):
        sensor_mod.SensorDeviceClass = enum_ns(TEMPERATURE="temperature")
    if not hasattr(sensor_mod, "SensorStateClass"):
        sensor_mod.SensorStateClass = enum_ns(
            MEASUREMENT="measurement", TOTAL_INCREASING="total_increasing"
        )
    bs_mod = _module("homeassistant.components.binary_sensor")
    if not hasattr(bs_mod, "BinarySensorEntity"):
        bs_mod.BinarySensorEntity = type("BinarySensorEntity", (_EntityBase,), {})
    if not hasattr(bs_mod, "BinarySensorDeviceClass"):
        bs_mod.BinarySensorDeviceClass = enum_ns(
            CONNECTIVITY="connectivity",
            PROBLEM="problem",
            TAMPER="tamper",
            MOTION="motion",
            OCCUPANCY="occupancy",
        )
    const_mod = _module("homeassistant.const")
    for name, value in (
        ("CONF_URL", "url"),
        ("CONF_TOKEN", "token"),
        ("PERCENTAGE", "%"),
        ("EntityCategory", enum_ns(DIAGNOSTIC="diagnostic", CONFIG="config")),
        ("UnitOfTemperature", enum_ns(CELSIUS="°C")),
    ):
        if not hasattr(const_mod, name):
            setattr(const_mod, name, value)
    plat_mod = _module("homeassistant.helpers.entity_platform")
    if not hasattr(plat_mod, "AddEntitiesCallback"):
        plat_mod.AddEntitiesCallback = object
    aiohttp_client = _module("homeassistant.helpers.aiohttp_client")
    if getattr(aiohttp_client, "async_get_clientsession", None) is None:
        aiohttp_client.async_get_clientsession = lambda hass, *a, **kw: None

    # The event platform: a recording EventEntity with HA's own guard (an
    # event type outside `event_types` is a ValueError, never a state).
    event_comp = _module("homeassistant.components.event")
    if not hasattr(event_comp, "EventEntity"):

        class EventEntity(_EntityBase):
            _attr_event_types: list[str] = []

            @property
            def event_types(self) -> list[str]:
                return list(getattr(self, "_attr_event_types", []))

            def _trigger_event(self, event_type, event_attributes=None) -> None:
                if event_type not in self.event_types:
                    raise ValueError(f"Invalid event type {event_type}")
                self.__dict__.setdefault("fired", []).append((event_type, event_attributes))
                self.__dict__["last_event_type"] = event_type
                self.__dict__["last_event_attributes"] = event_attributes

            @property
            def state(self):
                return self.__dict__.get("last_event_type")

        event_comp.EventEntity = EventEntity
        event_comp.EventDeviceClass = enum_ns(DOORBELL="doorbell", MOTION="motion")
    button_comp = _module("homeassistant.components.button")
    if not hasattr(button_comp, "ButtonEntity"):

        class ButtonEntity(_EntityBase):
            async def async_press(self) -> None:
                raise NotImplementedError

        button_comp.ButtonEntity = ButtonEntity


def _augment_http() -> None:
    """HomeAssistantView and async_sign_path (the evidence view's surface)."""
    http_mod = _module("homeassistant.components.http")
    if not hasattr(http_mod, "HomeAssistantView"):

        class HomeAssistantView:
            url: str | None = None
            name: str | None = None
            requires_auth = True

        http_mod.HomeAssistantView = HomeAssistantView
    if not hasattr(http_mod, "StaticPathConfig"):

        class StaticPathConfig:
            def __init__(self, url, path, cache_headers) -> None:
                self.url = url
                self.path = path
                self.cache_headers = cache_headers

        http_mod.StaticPathConfig = StaticPathConfig
    auth_mod = _module("homeassistant.components.http.auth")
    if not hasattr(auth_mod, "async_sign_path"):

        def async_sign_path(hass, path, expiration, **kwargs) -> str:
            """Records every signing; the URL carries the lifetime in seconds."""
            hass.data.setdefault("_securacv_test_signed_paths", []).append(
                (path, expiration)
            )
            return f"{path}?authSig=test-{int(expiration.total_seconds())}"

        auth_mod.async_sign_path = async_sign_path


def _augment_media() -> None:
    """media_player classes and the media_source bases."""
    enum_ns = types.SimpleNamespace
    player_mod = _module("homeassistant.components.media_player")
    if not hasattr(player_mod, "MediaClass"):
        player_mod.MediaClass = enum_ns(DIRECTORY="directory", URL="url", APP="app")
    if not hasattr(player_mod, "BrowseError"):
        player_mod.BrowseError = type("BrowseError", (Exception,), {})
    ms_mod = _module("homeassistant.components.media_source")
    if getattr(ms_mod, "_securacv_rich", False):
        return

    class BrowseMediaSource:
        """Records every keyword the integration builds a node with."""

        def __init__(self, *, domain, identifier, **kwargs) -> None:
            self.domain = domain
            self.identifier = identifier
            self.media_content_id = f"media-source://{domain}" + (
                f"/{identifier}" if identifier else ""
            )
            for key, value in kwargs.items():
                setattr(self, key, value)
            self.children = kwargs.get("children")

    class MediaSource:
        name: str | None = None

        def __init__(self, domain: str) -> None:
            self.domain = domain

    @dataclass
    class MediaSourceItem:
        hass: Any
        domain: str | None
        identifier: str
        target_media_player: str | None = None

    @dataclass
    class PlayMedia:
        url: str
        mime_type: str

    ms_mod.BrowseMediaSource = BrowseMediaSource
    ms_mod.MediaSource = MediaSource
    ms_mod.MediaSourceItem = MediaSourceItem
    ms_mod.PlayMedia = PlayMedia
    ms_mod.Unresolvable = type("Unresolvable", (Exception,), {})
    ms_mod._securacv_rich = True


def _augment_notifications_registry_dt() -> None:
    """persistent_notification, the entity registry, and util.dt."""
    pn_mod = _module("homeassistant.components.persistent_notification")
    if not hasattr(pn_mod, "async_create"):

        def async_create(hass, message, title=None, notification_id=None) -> None:
            hass.data.setdefault("_securacv_test_notifications", []).append(
                {"message": message, "title": title, "notification_id": notification_id}
            )

        def async_dismiss(hass, notification_id) -> None:
            notes = hass.data.setdefault("_securacv_test_notifications", [])
            notes[:] = [n for n in notes if n["notification_id"] != notification_id]

        pn_mod.async_create = async_create
        pn_mod.async_dismiss = async_dismiss

    er_mod = _module("homeassistant.helpers.entity_registry")
    if not hasattr(er_mod, "async_entries_for_config_entry"):

        class _EntityRegistryStub:
            """Rows a test adds to `hass.data["_securacv_test_entity_registry"]`:
            anything with .domain, .unique_id, .config_entry_id, .original_name."""

            def __init__(self, hass) -> None:
                self._hass = hass

            @property
            def entities(self) -> list[Any]:
                return self._hass.data.setdefault("_securacv_test_entity_registry", [])

        er_mod.async_get = lambda hass: _EntityRegistryStub(hass)
        er_mod.async_entries_for_config_entry = lambda registry, entry_id: [
            row for row in registry.entities if getattr(row, "config_entry_id", None) == entry_id
        ]
        er_mod.RegistryEntry = types.SimpleNamespace

    dt_mod = _module("homeassistant.util.dt")
    if not hasattr(dt_mod, "now"):
        from datetime import datetime, timezone

        dt_mod.utcnow = lambda: datetime.now(timezone.utc)
        dt_mod.now = lambda: datetime.now(timezone.utc)
    util_mod = _module("homeassistant.util")
    if not hasattr(util_mod, "dt"):
        util_mod.dt = dt_mod


def _augment_ha_stubs() -> None:
    """Upgrade the installed stubs with the full surface the package needs."""
    _augment_core(sys.modules["homeassistant.core"])
    _augment_exceptions()
    _augment_config_validation()
    _augment_storage()
    _augment_event()
    _augment_aiohttp()
    _augment_voluptuous()
    _augment_data_entry_flow()
    _augment_config_entries()
    _augment_issue_registry()
    _augment_repairs()
    _augment_diagnostics()
    _augment_dispatcher()
    _augment_service_info()
    _augment_device_registry()
    _augment_mqtt()
    _augment_update_coordinator()
    _augment_platform_bases()
    _augment_http()
    _augment_media()
    _augment_notifications_registry_dt()


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


def make_runtime(hass, entry_id: str = "e1", **overrides):
    """A loaded TrustStore inside a SecuraCVRuntimeData, fields overridable."""
    from ..data import SecuraCVRuntimeData
    from ..device_trust import TrustStore

    store = overrides.pop("trust_store", None)
    if store is None:
        store = TrustStore(hass, entry_id=entry_id)
        run(store.async_load())
    fields = {"trust_store": store, "setup_mode": overrides.pop("setup_mode", "mqtt")}
    fields.update(overrides)
    return SecuraCVRuntimeData(**fields)


def make_entry(hass=None, entry_id: str = "e1", *, data=None, runtime=None, **kwargs):
    """A ConfigEntry stand-in; with `hass` it also gets a runtime and is registered."""
    from homeassistant.config_entries import ConfigEntry

    entry = ConfigEntry(entry_id=entry_id, data=data or {}, **kwargs)
    if hass is not None:
        entry.runtime_data = runtime if runtime is not None else make_runtime(hass, entry_id)
        hass.config_entries.add(entry)
    return entry


def issues(hass) -> dict[tuple[str, str], dict[str, Any]]:
    """Every Repairs issue currently open, keyed (domain, issue_id)."""
    return hass.data.get("_securacv_test_issues", {})


def published(hass) -> list[dict[str, Any]]:
    """Every mqtt.async_publish the integration made, in order."""
    return hass.data.get("_securacv_test_mqtt_published", [])


def device_registry(hass) -> DeviceRegistryStub:
    """The recording device registry for `hass`."""
    return sys.modules["homeassistant.helpers.device_registry"].async_get(hass)


def notifications(hass) -> list[dict[str, Any]]:
    """Every persistent notification the integration created, in order."""
    return hass.data.get("_securacv_test_notifications", [])


def signed_paths(hass) -> list[tuple[str, Any]]:
    """Every (path, lifetime) the integration asked async_sign_path to sign."""
    return hass.data.get("_securacv_test_signed_paths", [])


def dispatched(hass, signal: str) -> list[Any]:
    """The listeners connected to a dispatcher signal (to fire one by hand)."""
    return hass.data.get("_securacv_test_dispatcher", {}).get(signal, [])
