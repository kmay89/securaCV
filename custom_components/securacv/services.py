"""Actions — the automation surface for watches.

Three ``securacv.*`` actions, registered once per Home Assistant instance
from ``async_setup`` (the action-setup rule: they exist whether or not a
config entry is loaded, and say so when none is):

  - ``securacv.start_watch``  subject, duration?, concern?  -> the watch
  - ``securacv.end_watch``    watch (id or label)           -> the watch
  - ``securacv.list_watches`` (response only)               -> {"watches": [...]}

They go through ``watch_runtime``'s start/end path together with the
voice intents, so a watch is the same object however it began and is
persisted the same way. The field vocabulary is the spoken one on purpose
("two weeks", "if it stops"): an automation says what a person would say.

Why only watches. Pinning, rotating and unpinning a device key are trust
decisions the options flow deliberately asks a human to make: the key
mismatch path (``__init__.py``) tells the owner and asks them to rotate by
hand precisely so a re-flashed or impersonating device cannot be laundered
into "trusted" by a rule reacting to its own notification. Watches are the
opposite direction — starting one only adds attention, and ending one
removes only attention, never trust — and ending is the authenticated
surface the design already says ending belongs on (docs/design/watches.md;
docs/device_trust.md, "Why pin, rotate and unpin are not actions").
"""
from __future__ import annotations

import logging
import math
import time
from typing import Any

import voluptuous as vol

from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse, callback
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers import config_validation as cv

from . import watch_runtime, watches
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

SERVICE_START_WATCH = "start_watch"
SERVICE_END_WATCH = "end_watch"
SERVICE_LIST_WATCHES = "list_watches"
# The whole vocabulary. Action names are a compatibility promise; anything
# added here needs a written decision (docs/device_trust.md for trust).
SERVICES = (SERVICE_START_WATCH, SERVICE_END_WATCH, SERVICE_LIST_WATCHES)

ATTR_SUBJECT = "subject"
ATTR_DURATION = "duration"
ATTR_CONCERN = "concern"
ATTR_WATCH = "watch"

START_WATCH_SCHEMA = vol.Schema(
    {
        vol.Required(ATTR_SUBJECT): cv.string,
        vol.Optional(ATTR_DURATION): cv.string,
        vol.Optional(ATTR_CONCERN): vol.In(watches.CONCERNS),
    }
)
END_WATCH_SCHEMA = vol.Schema({vol.Required(ATTR_WATCH): cv.string})
LIST_WATCHES_SCHEMA = vol.Schema({})

# Every refusal the actions raise, by the key strings.json's ``exceptions``
# section (and translations/en.json, its byte-identical copy) declares it
# under. The English is the message as raised — ``str(err)``, what the log
# and an API client read. The frontend renders the refusal from the
# translation key and placeholders instead, in the user's language once a
# translations/<language>.json carries it (today only en.json exists).
# Placeholder values carry their own quotes (``repr``), so no template
# quotes a placeholder: hassfest refuses ``'{name}'`` in a translation.
# tests/test_exception_translations.py holds this table and strings.json
# word for word, pins every message as a user reads it, and fails on a
# key that is raised but undeclared or declared but never raised.
REFUSALS: dict[str, str] = {
    "no_loaded_entry": (
        "SecuraCV has no loaded entry, so nothing is running its watches. "
        "Enable or reload the integration to use them."
    ),
    "watches_unreadable": (
        "SecuraCV could not read its stored watches (the log has the reason), "
        "so its watches are not available. Reload the integration to try again."
    ),
    "not_loaded_yet": (
        "SecuraCV is not loaded yet, so its watches are not available. "
        "Try again once the integration has started."
    ),
    "duration_unreadable": (
        "Can't tell how long {duration} is. Give it in days, weeks, months, "
        'seasons or years ("two weeks", "10 days"), or leave it out for '
        "{default_days} days."
    ),
    "subject_empty": "Say what to keep an eye on: subject is empty.",
    "watch_limit_reached": (
        "Already running {count} watches, which is as many as the hub keeps. "
        "End one with securacv.end_watch first."
    ),
    "watch_ref_empty": "Say which watch to end: its id or its label.",
    "watch_not_found": (
        "No watch is called {watch}. securacv.list_watches names the ones running."
    ),
    "watch_ambiguous": "{watch} names {count} watches ({ids}); end it by id.",
}


def _refusal(key: str, **placeholders: str) -> ServiceValidationError:
    """The refusal ``key`` names: its English message, translatable.

    The message is passed as well as the key, so ``str(err)`` is the
    English above whether or not Home Assistant has this integration's
    translations cached; the key, domain and placeholders are what the
    frontend localizes from.
    """
    return ServiceValidationError(
        REFUSALS[key].format(**placeholders),
        translation_domain=DOMAIN,
        translation_key=key,
        translation_placeholders=placeholders or None,
    )


def watch_row(watch: dict[str, Any], now: float) -> dict[str, Any]:
    """One watch as an automation sees it: the model's own fields plus
    ``days_left``. Observations stay out — a diary is not a response."""
    return {
        "id": watch.get("id"),
        "label": watch.get("label"),
        "subject": dict(watch.get("subject") or {}),
        "concern": watch.get("concern"),
        "sensitivity": watch.get("sensitivity"),
        "state": watch.get("state"),
        "started_at": watch.get("started_at"),
        "ends_at": watch.get("ends_at"),
        "days_left": watches.days_left(watch, now),
        "fired": int(watch.get("fired", 0)),
    }


def _require_restored(hass: HomeAssistant) -> None:
    """Refuse, with the reason, until the persisted roster is in memory
    and a loaded entry is running the watch tick.

    Before the restore the bucket is not the truth (a start would be
    written over by the rows about to be read back, a list would answer
    "nothing" for watches that exist), so an early call is an error the
    automation can see rather than a quietly wrong answer. After the last
    entry unloads the restore flag stays set but nothing evaluates,
    delivers or expires a watch any more, so a start would record a
    promise nobody keeps; that is refused by name too.
    """
    if watch_runtime.watches_restored(hass):
        if watch_runtime.watches_hosted(hass):
            return
        raise _refusal("no_loaded_entry")
    if watch_runtime.watches_unreadable(hass):
        # The rows are still on disk, unread: the bucket is not the truth.
        raise _refusal("watches_unreadable")
    raise _refusal("not_loaded_yet")


def _duration_or_refuse(value: Any) -> str | None:
    """The duration text, or a refusal when it names no unit.

    Voice is forgiving on purpose: an unparseable phrase becomes the
    default rather than a question back. An automation is typed, and a
    silent 14 days for "48 hours" or "until Sunday" would be a watch
    nobody asked for, so the action refuses what the parser cannot read.
    Left out or blank still means the default.
    """
    text = str(value or "").strip()
    if not text:
        return None
    if math.isnan(watches.parse_duration_days(text, default=math.nan)):
        raise _refusal(
            "duration_unreadable",
            duration=repr(text),
            default_days=str(watches.DEFAULT_DAYS),
        )
    return text


def _async_start_watch(hass: HomeAssistant, call: ServiceCall) -> dict[str, Any]:
    _require_restored(hass)
    subject = str(call.data.get(ATTR_SUBJECT) or "").strip()
    if not subject:
        raise _refusal("subject_empty")
    duration = _duration_or_refuse(call.data.get(ATTR_DURATION))
    now = time.time()
    try:
        watch, device_id = watch_runtime.async_start_watch(
            hass,
            subject,
            duration,
            now,
            concern=call.data.get(ATTR_CONCERN),
        )
    except watch_runtime.WatchLimitReached as err:
        raise _refusal("watch_limit_reached", count=str(err.count)) from err
    if device_id is None:
        # The voice path says this out loud; an automation has no ear, so
        # the log carries it — a watch nothing feeds can never fire.
        _LOGGER.warning(
            "watch %s started on %r, which nothing in the fleet reports yet; "
            "it cannot fire until something does",
            watch["id"],
            subject,
        )
    return watch_row(watch, now)


def _async_end_watch(hass: HomeAssistant, call: ServiceCall) -> dict[str, Any]:
    _require_restored(hass)
    ref = str(call.data.get(ATTR_WATCH) or "").strip()
    if not ref:
        raise _refusal("watch_ref_empty")
    now = time.time()
    try:
        watch = watch_runtime.async_end_watch(hass, ref, now)
    except watch_runtime.WatchNotFound as err:
        raise _refusal("watch_not_found", watch=repr(ref)) from err
    except watch_runtime.WatchAmbiguous as err:
        raise _refusal(
            "watch_ambiguous", watch=repr(ref), count=str(len(err.ids)), ids=", ".join(err.ids)
        ) from err
    return watch_row(watch, now)


def _async_list_watches(hass: HomeAssistant, call: ServiceCall) -> dict[str, Any]:
    _require_restored(hass)
    now = time.time()
    rows: list[dict[str, Any]] = []
    # With ``now`` the bucket first announces and evicts anything past its
    # end, exactly as the spoken roster does, so what is listed is running.
    for watch in watch_runtime.async_watch_bucket(hass, now):
        watches.refresh_state(watch, now)
        rows.append(watch_row(watch, now))
    return {"watches": rows}


@callback
def async_setup_services(hass: HomeAssistant) -> None:
    """Register the actions, once; a second call is a no-op.

    ``hass`` is bound here, never read off the call: ``ServiceCall`` has
    no ``hass`` before Home Assistant 2025.1, and the integration supports
    2024.4.1 and newer (hacs.json). A handler that read ``call.hass`` would
    raise AttributeError on every call there.
    """
    if hass.services.has_service(DOMAIN, SERVICE_START_WATCH):
        return

    @callback
    def _start_watch(call: ServiceCall) -> dict[str, Any]:
        return _async_start_watch(hass, call)

    @callback
    def _end_watch(call: ServiceCall) -> dict[str, Any]:
        return _async_end_watch(hass, call)

    @callback
    def _list_watches(call: ServiceCall) -> dict[str, Any]:
        return _async_list_watches(hass, call)

    hass.services.async_register(
        DOMAIN,
        SERVICE_START_WATCH,
        _start_watch,
        schema=START_WATCH_SCHEMA,
        supports_response=SupportsResponse.OPTIONAL,
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_END_WATCH,
        _end_watch,
        schema=END_WATCH_SCHEMA,
        supports_response=SupportsResponse.OPTIONAL,
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_LIST_WATCHES,
        _list_watches,
        schema=LIST_WATCHES_SCHEMA,
        supports_response=SupportsResponse.ONLY,
    )
