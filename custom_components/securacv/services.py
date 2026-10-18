"""Actions — the automation surface: watches, the chain walk, the export.

Five ``securacv.*`` actions, registered once per Home Assistant instance
from ``async_setup`` (the action-setup rule: they exist whether or not a
config entry is loaded, and say so when none is):

  - ``securacv.start_watch``     subject, duration?, concern?  -> the watch
  - ``securacv.end_watch``       watch (id or label)           -> the watch
  - ``securacv.list_watches``    (response only)               -> {"watches": [...]}
  - ``securacv.verify``          entry_id?                     -> the walk's verdict
  - ``securacv.export_evidence`` window_hours?, entry_id?      -> the pack

The watch actions go through ``watch_runtime``'s start/end path together
with the voice intents, so a watch is the same object however it began and
is persisted the same way. The field vocabulary is the spoken one on
purpose ("two weeks", "if it stops"): an automation says what a person
would say. ``verify`` runs the integration's own chain walk against the
pinned kernel key (``kernel_verify``) and ``export_evidence`` composes the
signed evidence pack (``evidence``); the Verify now and Export evidence
buttons call the same two coroutines, so a press and an automation do one
thing. ``entry_id`` (a config_entry selector) says which kernel when more
than one entry has one; left out, the only one.

What is NOT here. Pinning, rotating and unpinning a device key are trust
decisions the options flow and the Repairs fix flow deliberately ask a
human to make: the key mismatch path tells the owner and asks them to
confirm the fingerprint by hand precisely so a re-flashed or impersonating
device cannot be laundered into "trusted" by a rule reacting to its own
notification. Services never mutate trust (docs/device_trust.md, "Why pin,
rotate and unpin are not actions"); verifying and exporting only read.
"""
from __future__ import annotations

import logging
import math
import time
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse, callback
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import config_validation as cv

from . import rollout, watch_runtime, watches
from .api import SecuraCVApiAuthError, SecuraCVApiConnectionError, SecuraCVApiError
from .const import (
    DOMAIN,
    EXPORT_WINDOW_DEFAULT_HOURS,
    EXPORT_WINDOW_MAX_HOURS,
    EXPORT_WINDOW_MIN_HOURS,
    VERIFY_TRIGGER_SERVICE,
)
from .data import runtime_for

_LOGGER = logging.getLogger(__name__)

SERVICE_START_WATCH = "start_watch"
SERVICE_END_WATCH = "end_watch"
SERVICE_LIST_WATCHES = "list_watches"
SERVICE_VERIFY = "verify"
SERVICE_EXPORT_EVIDENCE = "export_evidence"
SERVICE_STAGED_UPDATE = "staged_update"
# The whole vocabulary. Action names are a compatibility promise; anything
# added here needs a written decision (docs/device_trust.md for trust;
# docs/OTA.md §5 for staged_update, which mutates firmware, never trust).
SERVICES = (
    SERVICE_START_WATCH,
    SERVICE_END_WATCH,
    SERVICE_LIST_WATCHES,
    SERVICE_VERIFY,
    SERVICE_EXPORT_EVIDENCE,
    SERVICE_STAGED_UPDATE,
)

ATTR_SUBJECT = "subject"
ATTR_DURATION = "duration"
ATTR_CONCERN = "concern"
ATTR_WATCH = "watch"
ATTR_ENTRY_ID = "entry_id"
ATTR_WINDOW_HOURS = "window_hours"
ATTR_ENTITY_ID = "entity_id"
ATTR_NODE_TIMEOUT = "node_timeout"

# `Any`: the schema class is voluptuous on cores before 2026.9 and probatio
# (through its `voluptuous` alias) after; mypy must not pin either.
START_WATCH_SCHEMA: Any = vol.Schema(
    {
        vol.Required(ATTR_SUBJECT): cv.string,
        vol.Optional(ATTR_DURATION): cv.string,
        vol.Optional(ATTR_CONCERN): vol.In(watches.CONCERNS),
    }
)
END_WATCH_SCHEMA: Any = vol.Schema({vol.Required(ATTR_WATCH): cv.string})
LIST_WATCHES_SCHEMA: Any = vol.Schema({})
VERIFY_SCHEMA: Any = vol.Schema({vol.Optional(ATTR_ENTRY_ID): cv.string})
def _entity_ids(value: Any) -> list[str]:
    """One entity id or a list of them, always a list out. A validator
    function rather than vol.Any(cv.string, [cv.string]) so the stub
    voluptuous the unit tests run against can hold it too."""
    items = [value] if isinstance(value, str) else list(value)
    if not items or not all(isinstance(item, str) and item for item in items):
        raise ValueError("entity_id must be an entity id or a list of them")
    return items


STAGED_UPDATE_SCHEMA: Any = vol.Schema(
    {
        vol.Optional(ATTR_ENTITY_ID): _entity_ids,
        vol.Optional(ATTR_NODE_TIMEOUT, default=rollout.DEFAULT_NODE_TIMEOUT_S): vol.All(
            vol.Coerce(int),
            vol.Range(min=rollout.MIN_NODE_TIMEOUT_S, max=rollout.MAX_NODE_TIMEOUT_S),
        ),
    }
)
EXPORT_EVIDENCE_SCHEMA: Any = vol.Schema(
    {
        vol.Optional(ATTR_WINDOW_HOURS, default=EXPORT_WINDOW_DEFAULT_HOURS): vol.All(
            vol.Coerce(int),
            vol.Range(min=EXPORT_WINDOW_MIN_HOURS, max=EXPORT_WINDOW_MAX_HOURS),
        ),
        vol.Optional(ATTR_ENTRY_ID): cv.string,
    }
)

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
    # The kernel actions (verify, export_evidence).
    "no_kernel_entry": (
        "No SecuraCV entry with a witness kernel is loaded, so there is no sealed "
        "log to verify or export. Add the SecuraCV app or sidecar first."
    ),
    "kernel_entry_ambiguous": (
        "{count} SecuraCV entries have a witness kernel; say which one with entry_id."
    ),
    "entry_not_loaded": (
        "SecuraCV entry {entry_id} is not loaded or has no witness kernel."
    ),
    "window_out_of_range": (
        "window_hours is {window_hours}; give a whole number of hours from "
        "{min_hours} to {max_hours}."
    ),
    "kernel_unreachable": (
        "The witness kernel did not answer. Check that the SecuraCV app or sidecar "
        "is running, then try again."
    ),
    "kernel_auth_failed": (
        "The witness kernel rejected Home Assistant's credential. Re-authenticate "
        "the SecuraCV entry under Settings → Devices & services."
    ),
    "kernel_bad_response": (
        "The witness kernel answered with something other than the sealed log or "
        "the evidence bundle: {detail}."
    ),
    "export_write_failed": (
        "The evidence pack could not be written under the configuration directory: "
        "{detail}."
    ),
    # The staged firmware rollout (docs/OTA.md §5).
    "rollout_no_update_entities": (
        "No Canary has a firmware update entity yet. A Canary announces one over "
        "MQTT once it is on the broker; check that the fleet is online."
    ),
    "rollout_not_update_entities": (
        "{entity_ids} are not update entities; name the Canaries' firmware "
        "update entities (the update domain) or leave entity_id out."
    ),
    "rollout_aborted": (
        "The staged rollout stopped: {reason}. {installed} installed, "
        "{not_attempted} not attempted. Fix that node, then run it again."
    ),
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


def _failure(key: str, **placeholders: str) -> HomeAssistantError:
    """A failure in doing what was asked (the kernel did not answer, the pack
    could not be written) — HomeAssistantError, not a validation refusal,
    so an automation's error handling tells the two apart."""
    return HomeAssistantError(
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


# ── the kernel actions ──────────────────────────────────────────────────


def _kernel_entries(hass: HomeAssistant) -> list[ConfigEntry]:
    """Every loaded entry that has a kernel (a coordinator)."""
    found: list[ConfigEntry] = []
    for entry in hass.config_entries.async_entries(DOMAIN):
        runtime = runtime_for(entry)
        if runtime is not None and runtime.coordinator is not None:
            found.append(entry)
    return found


def _kernel_entry_or_refuse(hass: HomeAssistant, entry_id: Any) -> ConfigEntry:
    """The entry an action targets: the named one, else the only one."""
    entries = _kernel_entries(hass)
    if entry_id:
        wanted = str(entry_id)
        for entry in entries:
            if entry.entry_id == wanted:
                return entry
        raise _refusal("entry_not_loaded", entry_id=wanted)
    if not entries:
        raise _refusal("no_kernel_entry")
    if len(entries) > 1:
        raise _refusal("kernel_entry_ambiguous", count=str(len(entries)))
    return entries[0]


def _window_hours_or_refuse(value: Any) -> int:
    """`window_hours` as a bounded int (the schema bounds it on a real core;
    the handler bounds it again so a stub lane or a direct call cannot ask
    the kernel for a window it does not serve)."""
    if value is None:
        return EXPORT_WINDOW_DEFAULT_HOURS
    try:
        if isinstance(value, bool):
            raise ValueError(value)
        hours = int(value)
    except (TypeError, ValueError):
        hours = -1
    if not EXPORT_WINDOW_MIN_HOURS <= hours <= EXPORT_WINDOW_MAX_HOURS:
        raise _refusal(
            "window_out_of_range",
            window_hours=repr(value),
            min_hours=str(EXPORT_WINDOW_MIN_HOURS),
            max_hours=str(EXPORT_WINDOW_MAX_HOURS),
        )
    return hours


async def async_verify_for_entry(
    hass: HomeAssistant, entry: ConfigEntry, *, trigger: str
) -> dict[str, Any]:
    """Run the entry's chain walk now and shape the response (action + button)."""
    from .kernel_verify import async_verify_entry, kernel_device_id_of, verification_response

    try:
        state = await async_verify_entry(hass, entry, trigger=trigger)
    except ValueError as err:
        raise _refusal("entry_not_loaded", entry_id=entry.entry_id) from err
    except SecuraCVApiAuthError as err:
        raise _failure("kernel_auth_failed") from err
    except SecuraCVApiConnectionError as err:
        raise _failure("kernel_unreachable") from err
    except SecuraCVApiError as err:
        raise _failure("kernel_bad_response", detail=str(err)) from err
    return verification_response(state, kernel_device_id_of(entry))


async def async_export_for_entry(
    hass: HomeAssistant, entry: ConfigEntry, *, window_hours: int
) -> dict[str, Any]:
    """Compose the entry's evidence pack (action + button)."""
    from .evidence import async_export_evidence

    try:
        return await async_export_evidence(hass, entry, window_hours=window_hours)
    except ValueError as err:
        raise _refusal("entry_not_loaded", entry_id=entry.entry_id) from err
    except SecuraCVApiAuthError as err:
        raise _failure("kernel_auth_failed") from err
    except SecuraCVApiConnectionError as err:
        raise _failure("kernel_unreachable") from err
    except SecuraCVApiError as err:
        raise _failure("kernel_bad_response", detail=str(err)) from err
    except OSError as err:
        raise _failure("export_write_failed", detail=str(err)) from err


async def _async_verify(hass: HomeAssistant, call: ServiceCall) -> dict[str, Any]:
    entry = _kernel_entry_or_refuse(hass, call.data.get(ATTR_ENTRY_ID))
    return await async_verify_for_entry(hass, entry, trigger=VERIFY_TRIGGER_SERVICE)


async def _async_staged_update(hass: HomeAssistant, call: ServiceCall) -> dict[str, Any]:
    """Install the fleet's pending firmware updates one node at a time."""
    named = call.data.get(ATTR_ENTITY_ID)
    if isinstance(named, str):
        named = [named]
    if named:
        wrong = [e for e in named if not str(e).startswith(f"{rollout.UPDATE_DOMAIN}.")]
        if wrong:
            raise _refusal("rollout_not_update_entities", entity_ids=repr(sorted(wrong)))
        entity_ids = [str(e) for e in named]
    else:
        entity_ids = rollout.candidate_entity_ids(hass)
        if not entity_ids:
            raise _refusal("rollout_no_update_entities")
    result = await rollout.async_run(
        hass, entity_ids, node_timeout_s=float(call.data[ATTR_NODE_TIMEOUT])
    )
    if result.aborted:
        raise _failure(
            "rollout_aborted",
            reason=str(result.reason),
            installed=str(len(result.installed)),
            not_attempted=str(len(result.not_attempted)),
        )
    return result.as_dict()


async def _async_export_evidence(hass: HomeAssistant, call: ServiceCall) -> dict[str, Any]:
    hours = _window_hours_or_refuse(call.data.get(ATTR_WINDOW_HOURS))
    entry = _kernel_entry_or_refuse(hass, call.data.get(ATTR_ENTRY_ID))
    return await async_export_for_entry(hass, entry, window_hours=hours)


@callback
def async_setup_services(hass: HomeAssistant) -> None:
    """Register the actions, once; a second call is a no-op.

    ``hass`` is bound here, never read off the call: the handlers then
    work with any ``ServiceCall`` shape, including the slotted one without
    ``hass`` that cores before 2025.1 passed and the tests still drive
    (hacs.json's floor is 2026.5.0 now, but the binding costs nothing and
    keeps the handlers free of the call object's layout).
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

    async def _verify(call: ServiceCall) -> dict[str, Any]:
        return await _async_verify(hass, call)

    async def _export_evidence(call: ServiceCall) -> dict[str, Any]:
        return await _async_export_evidence(hass, call)

    async def _staged_update(call: ServiceCall) -> dict[str, Any]:
        return await _async_staged_update(hass, call)

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
    hass.services.async_register(
        DOMAIN,
        SERVICE_VERIFY,
        _verify,
        schema=VERIFY_SCHEMA,
        supports_response=SupportsResponse.OPTIONAL,
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_EXPORT_EVIDENCE,
        _export_evidence,
        schema=EXPORT_EVIDENCE_SCHEMA,
        supports_response=SupportsResponse.OPTIONAL,
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_STAGED_UPDATE,
        _staged_update,
        schema=STAGED_UPDATE_SCHEMA,
        supports_response=SupportsResponse.OPTIONAL,
    )
