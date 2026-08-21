"""Diagnostics — what a publicly shared dump actually contains.

The regression: the trust section sliced the fingerprint with ``[:16]``
and appended an ellipsis, but the fingerprint IS exactly 16 hex chars
(device_trust: sha256 digest[:8].hex()) — a no-op truncation whose "…"
falsely implied something had been held back. The slice is now half the
fingerprint, so the truncation is real.
"""

from __future__ import annotations

import types

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from ..diagnostics import async_get_config_entry_diagnostics  # noqa: E402
from ..const import DOMAIN  # noqa: E402

FULL_FP = "aabbccdd11223344"  # 16 hex chars, like the real fingerprint


def _entry() -> types.SimpleNamespace:
    return types.SimpleNamespace(entry_id="e1", data={})


def _hass_with_trust(fp: str | None) -> HomeAssistant:
    trust_entry = types.SimpleNamespace(fingerprint_hex=fp, pin_source="tofu")
    trust_store = types.SimpleNamespace(get=lambda device_id: trust_entry)
    hass = HomeAssistant()
    hass.data = {
        DOMAIN: {
            "e1": {
                "devices": {"canary01": {"status": "online"}},
                "trust_store": trust_store,
                "verify": {},
                "unsub_mqtt": [],
            }
        }
    }
    return hass


def test_fingerprint_is_actually_truncated() -> None:
    diag = run(async_get_config_entry_diagnostics(_hass_with_trust(FULL_FP), _entry()))
    shown = diag["trust"]["canary01"]["fingerprint"]
    assert shown.endswith("…")
    # Really shorter than the identifier it stands for — not the old no-op.
    assert len(shown.rstrip("…")) < len(FULL_FP)
    assert shown == FULL_FP[:8] + "…"
    # And the full fingerprint appears nowhere in the dump.
    assert FULL_FP not in repr(diag)


def test_diagnostics_survive_an_unpinned_device() -> None:
    diag = run(async_get_config_entry_diagnostics(_hass_with_trust(""), _entry()))
    assert diag["trust"]["canary01"]["fingerprint"] is None
    assert diag["trust"]["canary01"]["pinned"] is False
