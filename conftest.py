"""Repo-root conftest.

Installs lightweight homeassistant.* + aiohttp + voluptuous stubs in
sys.modules BEFORE pytest's import machinery traverses into the
`custom_components.securacv` package. The integration's __init__.py
imports aiohttp at module top, so without the stub pytest's
collection step crashes before any test conftest gets a chance to
patch.

Tests under `custom_components/securacv/tests/` rely on these stubs
being in place; we deliberately keep them small (only the surface
TrustStore + signature touch) so they can't drift away from the real
HA API without us noticing.
"""

from __future__ import annotations

import sys
import types
import json
from typing import Any


def _install_minimum_stubs() -> None:
    if "homeassistant" in sys.modules:
        return

    # ── aiohttp ──────────────────────────────────────────────────────
    aiohttp_stub = types.ModuleType("aiohttp")

    class _StubError(Exception):
        pass

    aiohttp_stub.ClientError = _StubError
    aiohttp_stub.ClientResponseError = _StubError
    aiohttp_stub.ClientSession = object
    sys.modules["aiohttp"] = aiohttp_stub

    # ── voluptuous ───────────────────────────────────────────────────
    vol_stub = types.ModuleType("voluptuous")
    vol_stub.Schema = lambda *a, **kw: None
    vol_stub.Required = lambda *a, **kw: None
    vol_stub.Optional = lambda *a, **kw: None
    vol_stub.In = lambda *a, **kw: None
    sys.modules["voluptuous"] = vol_stub

    # ── homeassistant.core ───────────────────────────────────────────
    ha = types.ModuleType("homeassistant")
    ha_core = types.ModuleType("homeassistant.core")

    class _ServiceRegistry:
        """Records what the integration registers (services.py) so a test
        can find a handler by (domain, service) and call it with a
        ServiceCall; async_call itself is a no-op — watch_runtime._notify
        schedules one and the tests that care patch _notify."""

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

    class _HomeAssistant:
        def __init__(self) -> None:
            self.data: dict[str, Any] = {}

        @property
        def services(self):
            # A property, cached per instance, so a test subclass can still
            # override it with its own property (test_tofu_health_hook.py).
            registry = self.__dict__.get("_services")
            if registry is None:
                registry = self.__dict__["_services"] = _ServiceRegistry()
            return registry

        def async_create_task(self, coro):
            # Drop the coroutine on the floor — tests that need to
            # observe scheduled tasks should patch this. We close the
            # coro to suppress "coroutine was never awaited" warnings.
            try:
                coro.close()
            except Exception:
                pass
            return None

    class _ServiceCall:
        """What a registered handler receives, in the OLDEST shape the
        integration supports (hacs.json: 2024.4.1). HA added ``.hass`` to
        ServiceCall only in 2025.1, so the stub is slotted without it: a
        handler that reads ``call.hass`` fails here as it would there."""

        __slots__ = ("domain", "service", "data", "context", "return_response")

        def __init__(
            self, domain, service, data=None, context=None, return_response=False
        ) -> None:
            self.domain = domain
            self.service = service
            self.data = dict(data or {})
            self.context = context
            self.return_response = return_response

    ha_core.HomeAssistant = _HomeAssistant
    ha_core.callback = lambda fn: fn
    ha_core.ServiceCall = _ServiceCall
    ha_core.SupportsResponse = types.SimpleNamespace(
        NONE="none", OPTIONAL="optional", ONLY="only"
    )

    # ── homeassistant.exceptions ─────────────────────────────────────
    ha_exceptions = types.ModuleType("homeassistant.exceptions")

    class _HomeAssistantError(Exception):
        """HA's constructor: a positional message plus the translation
        triple the frontend localizes from (services.py raises with both)."""

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

    class _ServiceValidationError(_HomeAssistantError):
        pass

    ha_exceptions.HomeAssistantError = _HomeAssistantError
    ha_exceptions.ServiceValidationError = _ServiceValidationError
    sys.modules["homeassistant.exceptions"] = ha_exceptions

    # ── homeassistant.helpers.storage.Store ──────────────────────────
    ha_helpers = types.ModuleType("homeassistant.helpers")
    ha_storage = types.ModuleType("homeassistant.helpers.storage")

    class _Store:
        """In-memory Store stub. Survives async_load/async_save inside
        a single TrustStore instance; tests that need cross-instance
        persistence reach into ._payload directly (see
        test_storage_roundtrip)."""

        def __init__(self, hass, version, key) -> None:
            self._payload: dict[str, Any] | None = None
            self._key = key
            self._version = version

        async def async_load(self):
            return self._payload

        async def async_save(self, data) -> None:
            self._payload = json.loads(json.dumps(data))

    ha_storage.Store = _Store

    sys.modules["homeassistant"] = ha
    sys.modules["homeassistant.core"] = ha_core
    sys.modules["homeassistant.helpers"] = ha_helpers
    sys.modules["homeassistant.helpers.storage"] = ha_storage

    # ── homeassistant submodules __init__.py imports ─────────────────
    for name, attrs in [
        ("homeassistant.config_entries", {
            "ConfigEntry": object,
            "ConfigFlowResult": dict,
            "OptionsFlow": object,
        }),
        ("homeassistant.const", {
            "CONF_TOKEN": "token",
            "CONF_URL": "url",
            "Platform": types.SimpleNamespace(
                SENSOR="sensor", BINARY_SENSOR="binary_sensor"
            ),
        }),
        ("homeassistant.components", {}),
        ("homeassistant.components.mqtt", {
            "async_wait_for_mqtt_client": None,
            "async_subscribe": None,
            "ReceiveMessage": object,
        }),
        ("homeassistant.helpers.aiohttp_client", {
            "async_get_clientsession": lambda *a, **kw: None,
        }),
        # DataUpdateCoordinator is subscripted as a generic
        # (DataUpdateCoordinator[dict]) at module top, so we need a
        # class with __class_getitem__. The metaclass approach lets
        # it stand in for both the type and the subscriptable form.
        ("homeassistant.helpers.update_coordinator", {
            "DataUpdateCoordinator": type(
                "DataUpdateCoordinator",
                (object,),
                {"__class_getitem__": classmethod(lambda cls, item: cls)},
            ),
            "UpdateFailed": Exception,
            "CoordinatorEntity": object,
        }),
        ("homeassistant.helpers.device_registry", {
            "async_get": lambda *a, **kw: None,
        }),
        # The validators services.py's schemas and __init__.py's
        # CONFIG_SCHEMA name; the stub voluptuous never runs them.
        ("homeassistant.helpers.config_validation", {
            "string": str,
            "boolean": bool,
            "positive_float": float,
            "config_entry_only_config_schema": lambda domain: (lambda config: config),
        }),
        ("homeassistant.config_entries", {
            "ConfigEntry": object,
            "ConfigFlowResult": dict,
            "OptionsFlow": object,
            "ConfigFlow": object,
        }),
        ("homeassistant", {}),  # touched again; idempotent
    ]:
        mod = sys.modules.get(name) or types.ModuleType(name)
        for k, v in attrs.items():
            setattr(mod, k, v)
        sys.modules[name] = mod


_install_minimum_stubs()
