"""The two buttons run the actions' coroutines and follow the kernel's reach.

A press on Verify now is `securacv.verify` for this entry (trigger
`button`); a press on Export evidence is `securacv.export_evidence` with the
default window. Neither registers anything; both are unavailable while the
kernel is out of reach, so a button never promises what it cannot do.
"""

from __future__ import annotations

import json
import types
from pathlib import Path

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import button as button_platform  # noqa: E402
from .. import services  # noqa: E402
from ..const import EXPORT_WINDOW_DEFAULT_HOURS, VERIFY_TRIGGER_BUTTON  # noqa: E402

PACKAGE_DIR = Path(button_platform.__file__).resolve().parent


def _setup(with_kernel: bool = True):
    hass = HomeAssistant()
    hass.data = {}
    coordinator = types.SimpleNamespace(kernel_available=True, data={}, pinned_key="ab" * 32)
    runtime = make_runtime(hass, coordinator=coordinator if with_kernel else None)
    entry = make_entry(hass, data={"url": "http://kernel:8799"}, runtime=runtime)
    added: list = []
    run(button_platform.async_setup_entry(hass, entry, added.extend))
    return hass, entry, coordinator, added


def test_two_buttons_per_kernel_and_none_without_one():
    hass, entry, _c, added = _setup()
    assert [type(b).__name__ for b in added] == [
        "SecuraCVVerifyNowButton", "SecuraCVExportEvidenceButton"
    ]
    assert [b.unique_id for b in added] == [
        f"securacv_{entry.entry_id}_verify_now", f"securacv_{entry.entry_id}_export_evidence"
    ]
    assert [b._attr_translation_key for b in added] == ["verify_now", "export_evidence"]
    # The icons live in icons.json only (the icon-translations rule).
    icons = json.loads((PACKAGE_DIR / "icons.json").read_text(encoding="utf-8"))["entity"]["button"]
    assert [icons[b._attr_translation_key]["default"] for b in added] == [
        "mdi:shield-search", "mdi:file-certificate"
    ]
    assert not any("_attr_icon" in vars(type(b)) for b in added)
    assert all(b._attr_has_entity_name for b in added)
    assert getattr(added[0], "_attr_entity_category", None) is None
    assert button_platform.PARALLEL_UPDATES == 1
    _hass, _entry, _c, none = _setup(with_kernel=False)
    assert none == []


def test_availability_follows_the_kernel():
    hass, entry, coordinator, added = _setup()
    assert all(b.available for b in added)
    coordinator.kernel_available = False
    assert not any(b.available for b in added)


def test_verify_now_runs_the_action_s_coroutine_with_the_button_trigger(monkeypatch):
    hass, entry, _c, (verify, _export) = _setup()
    calls: list = []

    async def _verify(hass_, entry_, *, trigger):
        calls.append((entry_, trigger))
        return {"verdict": "3 new entries verified against the pinned key"}

    monkeypatch.setattr(button_platform, "async_verify_for_entry", _verify)
    verify.hass = hass
    run(verify.async_press())
    assert calls == [(entry, VERIFY_TRIGGER_BUTTON)]


def test_export_evidence_runs_the_action_s_coroutine_with_the_default_window(monkeypatch):
    hass, entry, _c, (_verify, export) = _setup()
    calls: list = []

    async def _export(hass_, entry_, *, window_hours):
        calls.append((entry_, window_hours))
        return {"file": "securacv-evidence-2026-10-12-deadbeef.zip", "events": 2}

    monkeypatch.setattr(button_platform, "async_export_for_entry", _export)
    export.hass = hass
    run(export.async_press())
    assert calls == [(entry, EXPORT_WINDOW_DEFAULT_HOURS)]


def test_the_buttons_call_the_very_coroutines_the_actions_call():
    assert button_platform.async_verify_for_entry is services.async_verify_for_entry
    assert button_platform.async_export_for_entry is services.async_export_for_entry
