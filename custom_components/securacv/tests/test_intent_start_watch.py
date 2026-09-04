"""StartWatch must bind to the name people actually say.

``voice.match_device`` takes a third argument — the friendly names a Canary
advertises in its status payload — and the DeviceCheck intent passes it, so
"how's the gate Canary?" resolves a serial-like ``cv-a1b2c3``. StartWatch
did not, even though its ``brief`` already carried ``device_names``. The
consequence was silent and total: "keep an eye on the gate canary" created
an UNBOUND watch, ``watch_runtime.async_observe_event`` only ever feeds
subjects of kind "event", and so the watch could not fire for its entire
fortnight — while its spoken confirmation promised it would.

The stubs here follow the convention in test_mqtt_payload_hardening.py:
just enough of ``homeassistant.helpers.intent`` and ``homeassistant.util.dt``
for intent.py to import and its handlers to run.
"""

from __future__ import annotations

import sys
import types
from datetime import datetime

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run


def _install_intent_stubs() -> None:
    """The intent-platform surface intent.py imports at module top."""
    intent_mod = sys.modules.get("homeassistant.helpers.intent") or types.ModuleType(
        "homeassistant.helpers.intent"
    )
    intent_mod.IntentHandler = getattr(intent_mod, "IntentHandler", None) or type(
        "IntentHandler", (), {}
    )
    intent_mod.Intent = getattr(intent_mod, "Intent", None) or type("Intent", (), {})
    intent_mod.async_register = lambda hass, handler: None
    sys.modules["homeassistant.helpers.intent"] = intent_mod

    util_mod = sys.modules.get("homeassistant.util") or types.ModuleType(
        "homeassistant.util"
    )
    dt_mod = sys.modules.get("homeassistant.util.dt") or types.ModuleType(
        "homeassistant.util.dt"
    )
    dt_mod.now = getattr(dt_mod, "now", None) or (lambda: datetime(2024, 1, 1, 12, 0))
    util_mod.dt = dt_mod
    sys.modules["homeassistant.util"] = util_mod
    sys.modules["homeassistant.util.dt"] = dt_mod


_install_intent_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import intent as intent_platform  # noqa: E402
from .. import watch_runtime  # noqa: E402
from ..const import DOMAIN  # noqa: E402

# A serial-like id — the case that fails without friendly names, and the
# shape the firmware's device_pseudonym actually produces.
GATE = "cv-a1b2c3"
BACK = "cv-d4e5f6"

DEVICES = {
    GATE: {"status": '{"online": true, "device_name": "Gate"}'},
    BACK: {"status": '{"online": true, "device_name": "Back Door"}'},
}


class _Response:
    def __init__(self) -> None:
        self.speech = ""

    def async_set_speech(self, speech: str) -> None:
        self.speech = speech


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {DOMAIN: {"e1": {"devices": DEVICES, "verify": {}}}}
    return hass


def _start(hass, subject: str, duration: str = "two weeks") -> _Response:
    response = _Response()
    intent_obj = types.SimpleNamespace(
        hass=hass,
        slots={
            "watch_subject": {"value": subject},
            "watch_duration": {"value": duration},
        },
        create_response=lambda: response,
    )
    run(intent_platform.StartWatchIntentHandler().async_handle(intent_obj))
    return response


def _watches(hass) -> list:
    return hass.data[DOMAIN]["watches"]


def test_start_watch_binds_to_the_friendly_name_people_say() -> None:
    hass = _hass()
    response = _start(hass, "the gate canary")

    assert len(_watches(hass)) == 1
    assert _watches(hass)[0]["subject"] == {"kind": "event", "ref": GATE}
    # A bound watch does not get the "nothing is feeding this" caveat.
    assert "nothing in the fleet is" not in response.speech

    # ...and the second name resolves to the second device, not the first.
    hass2 = _hass()
    _start(hass2, "back door")
    assert _watches(hass2)[0]["subject"] == {"kind": "event", "ref": BACK}


def test_the_bound_watch_actually_receives_that_device_s_events() -> None:
    """The consequence the binding exists for: before the fix nothing ever
    reached the watch, so it could not fire for its whole lifetime."""
    hass = _hass()
    _start(hass, "the gate canary")
    watch = _watches(hass)[0]

    watch_runtime.async_observe_event(hass, GATE, watch["started_at"] + 60)
    watch_runtime.async_observe_event(hass, BACK, watch["started_at"] + 120)

    assert len(watch["observations"]) == 1, "only the bound device may feed it"


def test_the_raw_device_id_still_binds() -> None:
    hass = _hass()
    _start(hass, GATE)
    assert _watches(hass)[0]["subject"] == {"kind": "event", "ref": GATE}


def test_a_subject_that_names_no_canary_is_still_honestly_unbound() -> None:
    """Binding harder must not start inventing matches: an unrecognized
    subject keeps the unbound shape AND says so out loud."""
    hass = _hass()
    response = _start(hass, "the litter box")

    assert _watches(hass)[0]["subject"] == {"kind": "unbound", "ref": "the litter box"}
    assert "nothing in the fleet is" in response.speech
