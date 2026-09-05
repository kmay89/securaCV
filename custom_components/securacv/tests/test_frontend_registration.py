"""Card registration must not read files on the event loop.

``_async_register_frontend`` is awaited from ``async_setup_entry``, so
everything it does runs on the loop. It used to ``open()`` manifest.json
(via ``_manifest_version``) and ``stat()`` each card (``Path.is_file``)
right there, which HA's blocking-call detector reports as "Detected
blocking call to open ... inside the event loop by custom integration
securacv" — while the same file already dispatched ``_read_token_file``
through an executor and documented why.

The guard below is the point of this file: ``open`` and ``Path.is_file``
raise a **BaseException** unless the executor is running, because
``_async_register_frontend`` swallows every ``Exception`` by design and
would hide an ordinary assertion failure.
"""

from __future__ import annotations

import builtins
import pathlib
import sys
import types

from . import conftest  # noqa: F401  (installs/augments ha stubs at import time)
from .conftest import run

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import _async_register_frontend, _card_assets, _manifest_version  # noqa: E402
from ..const import DOMAIN  # noqa: E402


def _install_frontend_stubs() -> None:
    http_mod = sys.modules.get("homeassistant.components.http") or types.ModuleType(
        "homeassistant.components.http"
    )

    class _StaticPathConfig:
        def __init__(self, url, path, cache_headers) -> None:
            self.url = url
            self.path = path
            self.cache_headers = cache_headers

    http_mod.StaticPathConfig = _StaticPathConfig
    sys.modules["homeassistant.components.http"] = http_mod

    frontend_mod = sys.modules.get(
        "homeassistant.components.frontend"
    ) or types.ModuleType("homeassistant.components.frontend")
    frontend_mod.add_extra_js_url = lambda hass, url: JS_URLS.append(url)
    sys.modules["homeassistant.components.frontend"] = frontend_mod


JS_URLS: list[str] = []
STATIC_PATHS: list = []

_install_frontend_stubs()


class _LoopIO(BaseException):
    """Deliberately not an Exception — the function under test swallows those."""


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {}

    async def _register_static_paths(configs):
        STATIC_PATHS.extend(configs)

    hass.http = types.SimpleNamespace(async_register_static_paths=_register_static_paths)
    return hass


def _run_guarded(monkeypatch, hass) -> list:
    """Run the registration with every blocking call fenced to the executor."""
    dispatched: list = []
    in_executor = {"now": False}

    real_open = builtins.open
    real_is_file = pathlib.Path.is_file

    def _guarded_open(*args, **kwargs):
        if not in_executor["now"]:
            raise _LoopIO(f"open() on the event loop: {args[:1]}")
        return real_open(*args, **kwargs)

    def _guarded_is_file(self, *args, **kwargs):
        if not in_executor["now"]:
            raise _LoopIO(f"Path.is_file() on the event loop: {self}")
        return real_is_file(self, *args, **kwargs)

    async def _executor(target, *args):
        dispatched.append(target)
        in_executor["now"] = True
        try:
            return target(*args)
        finally:
            in_executor["now"] = False

    monkeypatch.setattr(builtins, "open", _guarded_open)
    monkeypatch.setattr(pathlib.Path, "is_file", _guarded_is_file)
    monkeypatch.setattr(hass, "async_add_executor_job", _executor, raising=False)

    run(_async_register_frontend(hass))
    return dispatched


def test_card_registration_does_every_blocking_read_in_the_executor(monkeypatch) -> None:
    JS_URLS.clear()
    STATIC_PATHS.clear()
    hass = _hass()
    # Read before the guards go up — inside the test they would trip it.
    version = _manifest_version()
    assert version != "0", "manifest.json should carry a version"

    dispatched = _run_guarded(monkeypatch, hass)

    assert dispatched == [_card_assets], "the card probe was not sent to an executor"
    assert hass.data[DOMAIN]["_frontend_registered"] is True
    # The cards really were served and auto-loaded, at the manifest version.
    assert JS_URLS == [
        f"/{DOMAIN}_www/securacv-timeline-card.js?v={version}",
        f"/{DOMAIN}_www/securacv-aim-card.js?v={version}",
    ]
    assert [c.url for c in STATIC_PATHS] == [u.split("?")[0] for u in JS_URLS]


def test_registration_runs_once_per_hass(monkeypatch) -> None:
    """The guard in hass.data still holds — a second entry must not re-serve
    the cards (nor pay a second executor hop)."""
    JS_URLS.clear()
    STATIC_PATHS.clear()
    hass = _hass()

    _run_guarded(monkeypatch, hass)
    assert len(JS_URLS) == 2
    assert _run_guarded(monkeypatch, hass) == []
    assert len(JS_URLS) == 2


def test_card_assets_reports_the_version_and_the_files_on_disk() -> None:
    """What the executor hop actually returns — both halves in one call, so
    there is no second blocking probe left on the loop."""
    version, cards = _card_assets()

    assert version == _manifest_version()
    assert [name for name, _path in cards] == [
        "securacv-timeline-card.js",
        "securacv-aim-card.js",
    ]
    assert all(pathlib.Path(path).is_file() for _name, path in cards)
