"""SecuraCV - Privacy Witness Kernel integration for Home Assistant.

Connects to SecuraCV Canary devices via MQTT and/or the Privacy Witness
Kernel via MQTT (push) and HTTP (backstop). Surfaces semantic witness
events, hash chain integrity, and device health — never raw video or
identity data. Privacy by design.

Setup modes:
  - MQTT only: Auto-discovers Canary devices via MQTT
  - Kernel only: the witness kernel over HTTP (no MQTT at all)
  - Both: MQTT for Canary devices and the kernel's push topics, HTTP for
    the kernel backstop (the app and the sidecar both end up here)

Module-level imports here are deliberately limited to what the repo-root
test stubs provide; the kernel-side modules (coordinator, claim, repairs,
entity) are imported where they are used, after the stub lane has loaded
its richer surface. Everything else in the package imports normally.
"""
from __future__ import annotations

import logging
import re
from datetime import timedelta
from typing import Any, cast

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_TOKEN, CONF_URL, Platform
from homeassistant.core import HomeAssistant, callback
from homeassistant.components import mqtt
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr

from .api import (  # noqa: F401 - re-exported: the client moved here from api.py
    SecuraCVApi,
    SecuraCVApiAuthError,
    SecuraCVApiConnectionError,
    SecuraCVApiError,
    SecuraCVApiResponseError,
)
from .const import (
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
    CONF_ADAPTER_STATS_URL,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_DASHBOARD_NOTICE_SHOWN,
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_TOKEN_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MIGRATION_NOTICE_SHOWN,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_SETUP_MODE,
    CONF_TOKEN_FILE,
    DASHBOARDS_LINK,
    DEFAULT_MQTT_TOPIC_PREFIX,
    DOMAIN,
    MANUFACTURER,
    MODEL_CANARY,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    TOPIC_AVAILABILITY,
    TOPIC_HEALTH,
    TOPIC_STATUS,
)
from .data import SecuraCVConfigEntry, SecuraCVRuntimeData, runtime_for
from .device_trust import TrustStore, TrustVerdict, normalize_hex
from .kernel_trust import normalize_kernel_key, valid_kernel_device_id
from .mqtt_payload import (  # noqa: F401 - re-exported: every handler imports the gate from here
    DEVICE_ID_RE,
    MAX_MQTT_PAYLOAD_BYTES,
    mqtt_payload_text,
    mqtt_payload_within_cap,
    parse_mqtt_json,
    valid_device_id,
)
from .voice import status_online

_LOGGER = logging.getLogger(__name__)

# The four platforms (contract C11). Spelled through `_platform` so the thin
# repo-root test stubs — a `Platform` namespace that predates the event and
# button members — can still import this module; on a real core and in the
# integration's own stub lane every member resolves.
def _platform(name: str) -> Platform:
    """`Platform.<NAME>` by value, tolerant of a stub that lacks the member."""
    member = getattr(Platform, name.upper(), None)
    if member is not None:
        return cast(Platform, member)
    # A stub namespace without the member: the bare value (a StrEnum's value).
    return cast(Platform, name)


PLATFORMS: list[Platform] = [
    _platform("sensor"),
    _platform("binary_sensor"),
    _platform("event"),
    _platform("button"),
]

# Config-entry only: nothing is read from configuration.yaml. Declared
# because an integration with an async_setup must say so (hassfest).
CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

# The current config-entry schema (config_flow.SecuraCVConfigFlow.VERSION).
CONFIG_ENTRY_VERSION = 3

# Lovelace cards (custom_components/securacv/www/). Served and auto-loaded
# best-effort so `type: custom:securacv-timeline-card` /
# `custom:securacv-aim-card` and the `custom:securacv` dashboard strategy
# resolve without the user hand-adding a frontend resource. A file that is
# not on disk (the strategy ships separately) is skipped, not an error.
LOVELACE_CARD_FILENAMES = (
    "securacv-timeline-card.js",
    "securacv-aim-card.js",
    "securacv-strategy.js",
)
# The dashboard strategy module: when it was served, "Add dashboard →
# SecuraCV" exists in Settings → Dashboards and the one-time notice below
# may say so.
STRATEGY_CARD_FILENAME = "securacv-strategy.js"
# hass.data[DOMAIN] key listing the card files actually served this run.
DATA_FRONTEND_CARDS = "_frontend_cards"


def _card_digest(path: str) -> str:
    """A card file's cache key: the first 12 hex digits of its sha256.

    Keyed on the card's own bytes, not the integration version: the version
    sat at one number while the timeline card changed four times, and every
    browser kept the JS it already had. Blocking read — reached only through
    `_card_assets`, which runs in an executor.
    """
    import hashlib

    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()[:12]


def _card_assets() -> list[tuple[str, str, str]]:
    """The card files that exist, each with its cache key, in one hop.

    Both halves block — `Path.is_file()` per card and the `open()` that
    hashes it — and HA's blocking-call detector flags either one inside the
    event loop. Gathered here so `_async_register_frontend` pays a single
    `async_add_executor_job`, the same discipline `_read_token_file` follows.
    """
    from pathlib import Path

    www = Path(__file__).parent / "www"
    return [
        (filename, str(www / filename), _card_digest(str(www / filename)))
        for filename in LOVELACE_CARD_FILENAMES
        if (www / filename).is_file()
    ]


async def _async_register_frontend(hass: HomeAssistant) -> None:
    """Serve and auto-load the Lovelace cards. Never fatal.

    The frontend is optional — the integration is just as useful headless — so
    any registration hiccup is logged at debug and swallowed rather than failing
    setup. Runs once per HA instance (guarded in hass.data).
    """
    domain_data = hass.data.setdefault(DOMAIN, {})
    if domain_data.get("_frontend_registered"):
        return
    domain_data["_frontend_registered"] = True
    try:
        cards = await hass.async_add_executor_job(_card_assets)

        registered_any = False
        served: list[str] = domain_data.setdefault(DATA_FRONTEND_CARDS, [])
        for filename, card_path, digest in cards:
            card_url = f"/{DOMAIN}_www/{filename}"
            from homeassistant.components.http import StaticPathConfig

            await hass.http.async_register_static_paths(
                [StaticPathConfig(card_url, card_path, False)]
            )

            from homeassistant.components.frontend import add_extra_js_url

            # `?v=<digest of the card file>`: browsers and the HA frontend's
            # service worker cache extra_module_url scripts aggressively, so
            # without a query that moves every update left users on the old
            # card JS (cache_headers=False above only shapes the server
            # headers). It used to be the manifest version, which did not move
            # when only a card did; the file's own hash moves exactly when its
            # bytes do. Same convention HACS uses for every frontend resource;
            # the static path itself stays unversioned.
            add_extra_js_url(hass, f"{card_url}?v={digest}")
            registered_any = True
            if filename not in served:
                served.append(filename)
            _LOGGER.debug("SecuraCV card registered at %s", card_url)
        if not registered_any:
            domain_data["_frontend_registered"] = False
    except Exception:  # noqa: BLE001 - frontend is optional, never block setup
        domain_data["_frontend_registered"] = False
        _LOGGER.debug("SecuraCV Lovelace cards not registered", exc_info=True)


def _unique_id_in_use(hass: HomeAssistant, unique_id: str, entry: ConfigEntry) -> bool:
    """True when another securacv entry already holds `unique_id`."""
    for other in hass.config_entries.async_entries(DOMAIN):
        if other.entry_id != entry.entry_id and other.unique_id == unique_id:
            return True
    return False


def _migrated_entry_data(data: dict[str, Any]) -> dict[str, Any]:
    """The v3 shape of pre-v3 entry data (no network, pure)."""
    from .claim import new_claim_nonce

    new_data = dict(data)
    if new_data.get(CONF_URL):
        if not new_data.get(CONF_AUTH_MODE):
            new_data[CONF_AUTH_MODE] = (
                AUTH_MODE_TOKEN_FILE if new_data.get(CONF_TOKEN_FILE) else AUTH_MODE_STATIC
            )
        new_data.setdefault(CONF_MQTT_TOPIC_PREFIX, DEFAULT_MQTT_TOPIC_PREFIX)
        new_data.setdefault(CONF_CLAIM_NONCE, new_claim_nonce())
    return new_data


async def _async_identity_for_migration(hass: HomeAssistant, data: dict[str, Any]) -> dict[str, Any] | None:
    """Best-effort GET /api/identity during migration; None when unreachable."""
    try:
        api = SecuraCVApi(
            data[CONF_URL],
            data.get(CONF_TOKEN, ""),
            async_get_clientsession(hass),
            token_file=data.get(CONF_TOKEN_FILE),
        )
        return await api.async_get_identity()
    except Exception:  # noqa: BLE001 - the kernel may be down; setup retries later
        _LOGGER.debug("identity not readable during migration", exc_info=True)
        return None


async def async_migrate_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Migrate old config entries to VERSION 3.

    Version 1 predates this repository's history and used a compatible data
    schema; version 2 keyed kernel entries on the kernel URL, `hassio` or
    `mqtt_<prefix>`. Version 3 keys every kernel entry on the kernel's own
    device id (`kernel_<device_id>`, from GET /api/identity), records how it
    authenticates, the bridge's state prefix and the claim nonce, and pins
    the verifying key the identity served. A kernel that does not answer
    during migration keeps its old unique_id; async_setup_entry adopts the
    identity on the first backstop that reaches it.
    """
    if entry.version > CONFIG_ENTRY_VERSION:
        # Downgrade from a future version — refuse rather than guess.
        return False
    if entry.version == CONFIG_ENTRY_VERSION:
        return True

    new_data = _migrated_entry_data(dict(entry.data))
    unique_id = entry.unique_id
    if new_data.get(CONF_URL) and not new_data.get(CONF_KERNEL_DEVICE_ID):
        identity = await _async_identity_for_migration(hass, new_data)
        if identity is not None:
            device_id = identity.get("device_id")
            if valid_kernel_device_id(device_id):
                new_data[CONF_KERNEL_DEVICE_ID] = device_id
                candidate = f"kernel_{device_id}"
                if not _unique_id_in_use(hass, candidate, entry):
                    unique_id = candidate
            key = normalize_kernel_key(identity.get("verifying_key"))
            if key is not None and not new_data.get(CONF_KERNEL_VERIFYING_KEY):
                new_data[CONF_KERNEL_VERIFYING_KEY] = key
    hass.config_entries.async_update_entry(
        entry, data=new_data, unique_id=unique_id, version=CONFIG_ENTRY_VERSION
    )
    _LOGGER.info(
        "Migrated SecuraCV config entry from version %s to %s",
        entry.version,
        CONFIG_ENTRY_VERSION,
    )
    return True


async def async_setup(hass: HomeAssistant, config: dict[str, Any]) -> bool:
    """Register the securacv.* actions, once per Home Assistant instance.

    Here rather than in async_setup_entry (the action-setup rule): the
    actions exist whether or not a config entry is loaded, so an
    automation that calls one too early gets an explanatory error instead
    of "service not found". Never fatal — the actions are optional, the
    integration is not.
    """
    try:
        from .services import async_setup_services

        async_setup_services(hass)
    except Exception:  # noqa: BLE001 - the actions are optional, setup is not
        _LOGGER.warning("securacv actions not registered", exc_info=True)
    return True


async def _async_restore_watches(hass: HomeAssistant) -> None:
    """Restore persisted watches BEFORE anything can feed them.

    The first MQTT event after a restart must reach a restored watch, not a
    bucket that is still empty. Once per HA instance (the loader is
    idempotent); never fatal — watches are optional, setup is not.
    """
    try:
        from .watch_runtime import async_load_watches

        await async_load_watches(hass)
    except Exception:  # noqa: BLE001 - watches are optional, setup is not
        _LOGGER.debug("watches not restored", exc_info=True)


@callback
def _async_register_kernel_device(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> None:
    """Create or merge the kernel device with every identifier known now."""
    from .coordinator import DATA_IDENTITY
    from .entity import kernel_device_info

    runtime = runtime_for(entry)
    kernel_version = None
    if runtime is not None and runtime.coordinator is not None and runtime.coordinator.data:
        identity = runtime.coordinator.data.get(DATA_IDENTITY)
        if isinstance(identity, dict):
            kernel_version = identity.get("kernel_version")
    info = kernel_device_info(entry, kernel_version)
    dr.async_get(hass).async_get_or_create(config_entry_id=entry.entry_id, **info)


async def _async_adopt_identity(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> None:
    """Key the entry on the kernel's device id once /api/identity answered.

    An entry created before the identity endpoint existed (or while the
    kernel was down) carries a URL-, `hassio`- or prefix-keyed unique_id and
    no device id. The first backstop that reads the identity fixes both, so
    the claim (which needs the id) and the discovery flows (which dedupe on
    it) work without the person touching anything.
    """
    from .claim import new_claim_nonce
    from .coordinator import DATA_IDENTITY

    runtime = runtime_for(entry)
    if runtime is None or runtime.coordinator is None or not runtime.coordinator.data:
        return
    identity = runtime.coordinator.data.get(DATA_IDENTITY)
    if not isinstance(identity, dict):
        return
    device_id = identity.get("device_id")
    if not valid_kernel_device_id(device_id):
        return
    configured = entry.data.get(CONF_KERNEL_DEVICE_ID)
    if configured and configured != device_id:
        # A different kernel answers at this URL. The reconfigure flow is
        # the place to move an entry to another kernel; adopting silently
        # here would claim an id the person never paired with.
        _LOGGER.warning(
            "The kernel at %s identifies as %s but this entry was set up for %s; "
            "reconfigure the entry if the kernel was replaced",
            entry.data.get(CONF_URL),
            device_id,
            configured,
        )
        return
    changes: dict[str, Any] = {}
    if not configured:
        changes[CONF_KERNEL_DEVICE_ID] = device_id
    if not entry.data.get(CONF_CLAIM_NONCE):
        changes[CONF_CLAIM_NONCE] = new_claim_nonce()
    if not entry.data.get(CONF_MQTT_TOPIC_PREFIX):
        changes[CONF_MQTT_TOPIC_PREFIX] = DEFAULT_MQTT_TOPIC_PREFIX
    candidate = f"kernel_{device_id}"
    new_unique_id = (
        candidate
        if entry.unique_id != candidate and not _unique_id_in_use(hass, candidate, entry)
        else None
    )
    if not changes and new_unique_id is None:
        return
    kwargs: dict[str, Any] = {}
    if changes:
        kwargs["data"] = {**entry.data, **changes}
    if new_unique_id is not None:
        kwargs["unique_id"] = new_unique_id
    hass.config_entries.async_update_entry(entry, **kwargs)


@callback
def _bridge_entities_present(hass: HomeAssistant, kernel_device_id: str) -> bool:
    """Whether MQTT discovery ever created the bridge's own entities here.

    The bridge's discovery configs carry ``unique_id`` ``<device_id>_<object>``
    (``pwk_1a2b3c4d_last_event``) and default entity ids ``*.pwk_*``, on the
    ``mqtt`` platform. Read BEFORE the claim goes out: the claim is what
    makes the bridge withdraw them. When the registry cannot be read the
    answer is "yes" — the contract's default is to raise the notice.
    """
    from homeassistant.helpers import entity_registry as er

    try:
        rows: Any = er.async_get(hass).entities
        if hasattr(rows, "values"):
            rows = rows.values()
        for row in rows:
            if getattr(row, "platform", None) != "mqtt":
                continue
            unique_id = getattr(row, "unique_id", None)
            if isinstance(unique_id, str) and unique_id.startswith(f"{kernel_device_id}_"):
                return True
            entity_id = getattr(row, "entity_id", None)
            if isinstance(entity_id, str) and entity_id.partition(".")[2].startswith("pwk_"):
                return True
        return False
    except Exception:  # noqa: BLE001 - cannot tell: keep the contract's default
        _LOGGER.debug("entity registry unreadable; assuming bridge entities exist", exc_info=True)
        return True


async def _async_claim_kernel(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> None:
    """Publish the retained claim and keep it fresh; raise the migration notice once.

    The ``entities_migrated`` notice is for the people whose automations and
    dashboards named the bridge's ``pwk_*`` entities; a fresh install where
    MQTT discovery never created them is not told about a replacement that
    never happened. Either way the flag is set once, so the notice can
    never be raised twice.
    """
    from . import repairs
    from .claim import async_publish_claim, async_schedule_claim_republish

    runtime = runtime_for(entry)
    if runtime is None:
        return
    device_id = str(entry.data.get(CONF_KERNEL_DEVICE_ID) or "")
    show_migration = not entry.data.get(CONF_MIGRATION_NOTICE_SHOWN) and bool(device_id)
    bridge_entities = show_migration and _bridge_entities_present(hass, device_id)
    try:
        published = await async_publish_claim(hass, entry)
    except Exception as err:  # noqa: BLE001 - the claim is a courtesy to the bridge, not setup
        _LOGGER.warning("Could not publish the kernel claim: %s", err)
        return
    if not published:
        return
    runtime.claim_published = True
    runtime.unsub_mqtt.append(async_schedule_claim_republish(hass, entry))
    if show_migration:
        if bridge_entities:
            repairs.async_report_entities_migrated(hass, entry)
        hass.config_entries.async_update_entry(
            entry, data={**entry.data, CONF_MIGRATION_NOTICE_SHOWN: True}
        )


@callback
def _async_dashboard_notice(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> None:
    """Once per kernel entry: say where the dashboard is (J1).

    The strategy registers itself with the frontend, so the dashboard is
    one "Add dashboard" click away rather than created for the person; the
    notice says so, once, and only when the strategy module was really
    served this run — a sentence about a picker entry that does not exist
    would be a lie. Posted on the first load whether or not the kernel is
    reachable at that moment: the flow already probed it, and the dashboard
    is worth adding either way.
    """
    if entry.data.get(CONF_DASHBOARD_NOTICE_SHOWN):
        return
    served = hass.data.get(DOMAIN, {}).get(DATA_FRONTEND_CARDS) or []
    if STRATEGY_CARD_FILENAME not in served:
        return
    from homeassistant.components import persistent_notification

    persistent_notification.async_create(
        hass,
        "SecuraCV is connected. Add the dashboard: Settings → Dashboards → "
        "Add dashboard → SecuraCV. It builds itself from your cameras and the "
        f"witness chain.\n\n[Open Dashboards]({DASHBOARDS_LINK})",
        "SecuraCV: add the dashboard",
        f"securacv_dashboard_{entry.entry_id}",
    )
    hass.config_entries.async_update_entry(
        entry, data={**entry.data, CONF_DASHBOARD_NOTICE_SHOWN: True}
    )


async def async_setup_entry(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> bool:
    """Set up SecuraCV from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    # Serve + auto-load the Lovelace cards (best-effort, non-fatal).
    await _async_register_frontend(hass)
    await _async_restore_watches(hass)

    setup_mode = entry.data.get(CONF_SETUP_MODE, SETUP_MODE_KERNEL)
    has_kernel = setup_mode in (SETUP_MODE_KERNEL, SETUP_MODE_BOTH)
    enable_mqtt = bool(entry.data.get(CONF_ENABLE_MQTT, False))
    mqtt_prefix = entry.data.get(CONF_MQTT_PREFIX) if enable_mqtt else None
    topic_prefix = entry.data.get(CONF_MQTT_TOPIC_PREFIX) or DEFAULT_MQTT_TOPIC_PREFIX

    # test-before-setup: in mqtt/both modes the broker is a hard dependency.
    # A setup that goes on without it would subscribe nothing and show
    # every Canary as silent; ConfigEntryNotReady retries with backoff and
    # says why on the integration card.
    mqtt_ready = False
    if enable_mqtt:
        mqtt_ready = await mqtt.async_wait_for_mqtt_client(hass)
        if not mqtt_ready:
            from homeassistant.exceptions import ConfigEntryNotReady

            raise ConfigEntryNotReady(
                "The MQTT integration is not available yet; SecuraCV needs it "
                "for the Canary and witness topics"
            )

    # Trust store — persisted Ed25519 pubkey pins per device_id.
    # Created (and storage loaded) before the MQTT subscribe so the
    # first inbound message can already consult it for TOFU pinning.
    trust_store = TrustStore(hass, entry.entry_id)
    await trust_store.async_load()

    runtime = SecuraCVRuntimeData(
        trust_store=trust_store,
        setup_mode=setup_mode,
        mqtt_prefix=mqtt_prefix,
        # Per-device, per-kind high-water marks of the counters inside
        # VERIFIED publishes (event_id / seq / chain length / counts total).
        # A validly signed publish whose counter goes BACKWARDS is a replay
        # of an older message and must not move entity state — the
        # signatures alone cover no timestamp or nonce. Reset when a device
        # is (re)pinned via TOFU. Seeded from the pins, which persist them:
        # an empty mark accepts a retained stale-but-signed publish as
        # current, so starting every reload from zero would hand back the
        # replay window on each restart.
        replay={
            device_id: dict(pin.counters)
            for device_id, pin in trust_store.all_devices().items()
            if pin.counters
        },
    )
    entry.runtime_data = runtime

    if has_kernel:
        from . import repairs
        from .coordinator import SecuraCVKernelCoordinator

        api = SecuraCVApi(
            entry.data[CONF_URL],
            entry.data.get(CONF_TOKEN, ""),
            async_get_clientsession(hass),
            token_file=entry.data.get(CONF_TOKEN_FILE),
        )
        coordinator = SecuraCVKernelCoordinator(
            hass,
            entry,
            api,
            auth_mode=entry.data.get(CONF_AUTH_MODE, AUTH_MODE_TOKEN_FILE),
            topic_prefix=topic_prefix,
            pinned_key=entry.data.get(CONF_KERNEL_VERIFYING_KEY),
        )
        runtime.api = api
        runtime.coordinator = coordinator
        if mqtt_ready:
            try:
                await coordinator.async_subscribe_push()
            except Exception as err:  # noqa: BLE001 - the HTTP backstop keeps the kernel alive
                _LOGGER.warning("Kernel push subscriptions failed, HTTP backstop only: %s", err)
                repairs.async_report_mqtt_subscribe_failed(hass, entry, topic_prefix, err)
        if enable_mqtt:
            # "Both" mode: a kernel that is stopped, updating or still
            # booting must not take every Canary MQTT entity down with it,
            # so the first refresh is tolerant; the kernel entities report
            # unavailable and recover on the next backstop or push. Pure
            # kernel mode keeps the strict first refresh: there is nothing
            # else to serve.
            await coordinator.async_refresh()
        else:
            await coordinator.async_config_entry_first_refresh()
        await _async_adopt_identity(hass, entry)
        _async_register_kernel_device(hass, entry)
        if mqtt_ready:
            await _async_claim_kernel(hass, entry)
        repairs.async_clear_no_kernel_found(hass)

    # Optional adapter_host stats endpoint (read-only operational counters).
    # Setup must not fail if it is unreachable, so a tolerant refresh.
    adapter_stats_url = entry.data.get(CONF_ADAPTER_STATS_URL)
    if adapter_stats_url:
        from .coordinator import SecuraCVAdapterStatsCoordinator

        runtime.adapter_stats_coordinator = SecuraCVAdapterStatsCoordinator(
            hass, entry, adapter_stats_url, async_get_clientsession(hass)
        )
        await runtime.adapter_stats_coordinator.async_refresh()

    # Set up MQTT subscriptions for Canary devices
    if mqtt_ready and mqtt_prefix:
        from . import repairs

        try:
            status_callback = _async_device_status_received(hass, entry)
            for suffix in (TOPIC_STATUS, TOPIC_AVAILABILITY):
                runtime.unsub_mqtt.append(
                    await mqtt.async_subscribe(hass, f"{mqtt_prefix}/+/{suffix}", status_callback)
                )
            # Health-topic subscription: snags the device's pubkey_hex
            # the first time we see it and TOFU-pins via the trust
            # store. Sensor.py also subscribes to health for its own
            # state — that's fine, MQTT allows multiple subscribers
            # per topic and the two callbacks operate on independent
            # runtime slices.
            runtime.unsub_mqtt.append(
                await mqtt.async_subscribe(
                    hass, f"{mqtt_prefix}/+/{TOPIC_HEALTH}", _async_health_for_tofu(hass, entry)
                )
            )
            _LOGGER.info("SecuraCV MQTT subscriptions active (prefix: %s)", mqtt_prefix)
            repairs.async_clear_mqtt_subscribe_failed(hass, entry)
        except Exception as err:  # noqa: BLE001 - say what failed, keep the kernel side up
            _LOGGER.warning("MQTT setup failed, continuing without MQTT: %s", err)
            repairs.async_report_mqtt_subscribe_failed(hass, entry, mqtt_prefix, err)

    # The watch tick: evaluates every watch, delivers what fired, and
    # announces expiry. Without it a started watch would be recorded but
    # not actually watching, and its spoken promise would be untrue.
    try:
        from homeassistant.helpers.event import async_track_time_interval

        from .watch_runtime import TICK_INTERVAL_SECONDS, add_watch_host, async_tick

        @callback
        def _watch_tick(_now: Any) -> None:
            # Decorated, never a bare lambda: HassJob classifies a target
            # that is neither a coroutine function nor `@callback`-marked as
            # an Executor job and runs it in a worker thread, where the
            # tick's `hass.async_create_task` raises — and the tick's own
            # per-watch `except` swallows it, so a fired watch's
            # notification simply never arrives.
            async_tick(hass)

        runtime.unsub_mqtt.append(
            async_track_time_interval(
                hass,
                _watch_tick,
                timedelta(seconds=TICK_INTERVAL_SECONDS),
            )
        )
        add_watch_host(hass, entry.entry_id)
    except Exception:  # noqa: BLE001 - watches are optional, setup is not
        _LOGGER.debug("watch tick not scheduled", exc_info=True)

    # Forward to platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    if has_kernel and runtime.coordinator is not None and runtime.api is not None:
        # The integration's own chain walk (D10): after the platforms, so the
        # verification event entity hears the first verdict. Its timers and
        # listeners are released with the entry.
        from .evidence import async_register_view
        from .kernel_verify import KernelVerifier

        verifier = KernelVerifier(hass, entry, runtime.coordinator, runtime.api)
        runtime.verifier = verifier
        try:
            await verifier.async_start()
        except Exception:  # noqa: BLE001 - verification is retried on its timers, setup is not
            _LOGGER.warning("chain verifier did not start; it retries on its backstop", exc_info=True)
        runtime.unsub_mqtt.append(verifier.async_stop)
        # The authenticated download view for evidence packs, once per instance.
        async_register_view(hass)
        _async_dashboard_notice(hass, entry)

    _LOGGER.info(
        "SecuraCV integration loaded (mode: %s, mqtt: %s)",
        setup_mode,
        "enabled" if enable_mqtt else "disabled",
    )

    return True


async def async_unload_entry(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> bool:
    """Unload a SecuraCV config entry.

    Releases every subscription and timer this load registered. The
    retained claim stays: a reload or restart must not flip the bridge's
    entities out and in (it is withdrawn only in async_remove_entry).
    """
    runtime = runtime_for(entry)
    if runtime is not None:
        # Unwind and CLEAR, so a retried unload (HA retries when a platform
        # refused) never calls an MQTT unsubscribe twice.
        for unsub in runtime.unsub_mqtt:
            unsub()
        runtime.unsub_mqtt.clear()
        if runtime.coordinator is not None:
            runtime.coordinator.async_unsubscribe_push()
    from .watch_runtime import remove_watch_host

    remove_watch_host(hass, entry.entry_id)

    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        _LOGGER.info("SecuraCV integration unloaded")
    return unload_ok


async def async_remove_entry(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> None:
    """The entry is being deleted: withdraw the claim, revoke the paired token.

    Best-effort, every step: the entry is gone whatever happens here. The
    evidence exports on disk are the owner's and are left alone.
    """
    from . import repairs
    from .claim import async_withdraw_claim, claim_identity

    identity = claim_identity(entry)
    if identity is not None and entry.data.get(CONF_ENABLE_MQTT):
        prefix, device_id, _nonce = identity
        try:
            if await mqtt.async_wait_for_mqtt_client(hass):
                await async_withdraw_claim(hass, prefix, device_id)
        except Exception:  # noqa: BLE001 - the bridge's 24 h freshness window clears it anyway
            _LOGGER.debug("claim withdrawal failed", exc_info=True)

    token_id = entry.data.get(CONF_KERNEL_TOKEN_ID)
    if entry.data.get(CONF_AUTH_MODE) == AUTH_MODE_PAIRED and token_id and entry.data.get(CONF_URL):
        api = SecuraCVApi(
            entry.data[CONF_URL], entry.data.get(CONF_TOKEN, ""), async_get_clientsession(hass)
        )
        try:
            await api.async_revoke_paired_token(str(token_id))
        except Exception:  # noqa: BLE001 - revocation is a courtesy; `unpair` works too
            _LOGGER.debug("paired token revocation failed", exc_info=True)

    try:
        await TrustStore(hass, entry.entry_id).async_remove()
    except Exception:  # noqa: BLE001 - storage may already be gone
        _LOGGER.debug("trust store removal failed", exc_info=True)
    if entry.data.get(CONF_URL):
        from .kernel_verify import async_remove_verify_store

        try:
            await async_remove_verify_store(hass, entry.entry_id)
        except Exception:  # noqa: BLE001 - storage may already be gone
            _LOGGER.debug("verification store removal failed", exc_info=True)
    repairs.async_clear_entry_issues(hass, entry)
    # The entry's own notifications (the export link, the dashboard pointer)
    # would otherwise outlive it; a Canary key-mismatch notice is per device
    # and stays until the owner reads it.
    try:
        from homeassistant.components import persistent_notification

        for notification_id in (
            f"securacv_export_{entry.entry_id}",
            f"securacv_dashboard_{entry.entry_id}",
        ):
            persistent_notification.async_dismiss(hass, notification_id)
    except Exception:  # noqa: BLE001 - a notice left behind is not worth failing a removal
        _LOGGER.debug("notification dismissal failed", exc_info=True)


async def async_remove_config_entry_device(
    hass: HomeAssistant, entry: SecuraCVConfigEntry, device_entry: dr.DeviceEntry
) -> bool:
    """Allow deleting a Canary that left the fleet; never the kernel.

    A Canary still reporting on the status topic, or still pinned in the
    trust store, is part of the fleet and comes back on its next publish;
    the delete button is for the ones that are really gone.
    """
    runtime = runtime_for(entry)
    for domain, identifier in device_entry.identifiers:
        if domain != DOMAIN:
            continue
        if identifier.startswith("kernel_") or identifier == entry.data.get(CONF_URL):
            return False
        if identifier.startswith("canary_"):
            device_id = identifier[len("canary_"):]
            if runtime is not None and (
                device_id in runtime.devices or runtime.trust_store.is_pinned(device_id)
            ):
                return False
    return True


# The longest device name a status payload may set; the broker is untrusted.
DEVICE_NAME_MAX_LENGTH = 64


def _device_display_name(friendly_name: Any, device_id: str) -> str:
    """The Canary's device name: what the owner named it, else the id.

    The rule lives in ``entity.canary_display_name`` so every entity's
    ``device_info`` names the device exactly as the status handler does (a
    differing name in an entity's DeviceInfo replaces the owner's). Kept here
    as the name the status handler and its tests call.
    """
    from .entity import canary_display_name

    return canary_display_name(friendly_name, device_id)


def _safe_config_url(ap_ip: Any) -> str | None:
    """Build a configuration_url from a broker-supplied address, or None.

    The `ap_ip` field arrives over MQTT and is untrusted: it becomes a
    clickable link on the HA device page. Devices only ever report a LAN
    address (their AP/STA IP, e.g. 192.168.4.1) or their mDNS hostname
    (canary-<id>.local), so accept exactly those forms: a private or
    link-local IP address, or a `.local` hostname. Public IPs, arbitrary
    hostnames, and anything with URL syntax (scheme, port, path,
    credentials) are rejected — a hostile broker must not be able to plant
    an off-LAN phishing link.
    """
    if not isinstance(ap_ip, str) or not ap_ip or len(ap_ip) > 253:
        return None
    import ipaddress

    try:
        ip = ipaddress.ip_address(ap_ip)
    except ValueError:
        pass
    else:
        if not (ip.is_private or ip.is_link_local):
            return None
        # IPv6 literals need brackets in URLs.
        return f"http://[{ap_ip}]" if ip.version == 6 else f"http://{ap_ip}"
    if ap_ip.lower().endswith(".local") and re.fullmatch(
        r"[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)+",
        ap_ip,
    ):
        return f"http://{ap_ip}"
    return None


@callback
def _async_canary_online_changed(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str, online: bool | None
) -> None:
    """Tell the device's entities their availability moved."""
    from homeassistant.helpers.dispatcher import async_dispatcher_send

    from .entity import canary_status_signal

    runtime = runtime_for(entry)
    if runtime is None:
        return
    device = runtime.devices.setdefault(device_id, {"status": None})
    if device.get("online") == online:
        return
    was_online = device.get("online")
    device["online"] = online
    if online is False:
        _LOGGER.info("SecuraCV Canary %s reports offline", device_id)
    elif was_online is False:
        _LOGGER.info("SecuraCV Canary %s is back online", device_id)
    async_dispatcher_send(hass, canary_status_signal(entry.entry_id, device_id))


def _async_device_status_received(hass: HomeAssistant, entry: SecuraCVConfigEntry):
    """Return callback for Canary device status and availability messages."""

    @callback
    def _callback(msg: mqtt.ReceiveMessage) -> None:
        """Handle a status/availability message: register the Canary, track liveness."""
        # Topic format: {prefix}/{device_id}/status or /availability
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return
        device_id = parts[-2]
        if not valid_device_id(device_id):
            return
        runtime = runtime_for(entry)
        if runtime is None:
            return
        payload = mqtt_payload_text(msg.payload)
        if payload is None:
            return

        if parts[-1] == TOPIC_AVAILABILITY:
            # The canary base's last-will topic: a bare online/offline.
            word = payload.strip().lower()
            if word == "online":
                _async_canary_online_changed(hass, entry, device_id, True)
            elif word == "offline":
                _async_canary_online_changed(hass, entry, device_id, False)
            return

        devices = runtime.devices
        status_data = parse_mqtt_json(payload) or {}
        fw_version = status_data.get("firmware_version") or status_data.get("fw_version")
        hw_version = status_data.get("hardware") or status_data.get("board")
        config_url = _safe_config_url(status_data.get("ap_ip") or status_data.get("ip"))

        if device_id not in devices:
            _LOGGER.info("Discovered SecuraCV Canary device: %s", device_id)
            devices[device_id] = {"status": payload}
            display_name = _device_display_name(
                status_data.get("device_name") or status_data.get("name"), device_id
            )

            from .entity import kernel_via_device_kwargs

            dr.async_get(hass).async_get_or_create(
                config_entry_id=entry.entry_id,
                identifiers={(DOMAIN, f"canary_{device_id}")},
                manufacturer=MANUFACTURER,
                model=MODEL_CANARY,
                name=display_name,
                sw_version=str(fw_version) if fw_version else None,
                hw_version=str(hw_version) if hw_version else None,
                configuration_url=config_url,
                **kernel_via_device_kwargs(hass, entry),
            )
        else:
            devices[device_id]["status"] = payload
            if fw_version and fw_version != devices[device_id].get("_last_fw"):
                devices[device_id]["_last_fw"] = fw_version
                from .entity import kernel_via_device_kwargs

                dr.async_get(hass).async_get_or_create(
                    config_entry_id=entry.entry_id,
                    identifiers={(DOMAIN, f"canary_{device_id}")},
                    sw_version=str(fw_version),
                    hw_version=str(hw_version) if hw_version else None,
                    configuration_url=config_url,
                    **kernel_via_device_kwargs(hass, entry),
                )
        # The verdict comes from voice.py so the entities, the spoken answer
        # and the availability can never disagree about which Canaries are up.
        _async_canary_online_changed(hass, entry, device_id, status_online(payload))

    return _callback



# =============================================================================
# Trust / verification helpers
# =============================================================================
#
# The signed-MQTT path on the firmware side (PR adding per-device PKI)
# stamps every chain/events/counts publish with an Ed25519 signature
# the device produced over a canonical message. HA verifies that sig
# against the device's pinned pubkey from the TrustStore.
#
# The sensor handlers call `_verify_and_record` (sensor.py) inside
# their MQTT callbacks with the parsed payload and a verifier function.
# The path:
#   1. Never pins. TOFU happens in `_async_health_for_tofu` below, from
#      the `public_key` the health publish carries; no HTTP round-trip is
#      involved (nothing in the integration fetches /api/device/enroll).
#   2. Calls the kind-specific verifier (signature.verify_chain etc.).
#   3. Stamps the result into runtime.verify[device_id] so any
#      entity reading it can surface "verified: true/false".
#   4. Fires a one-shot persistent_notification on mismatch (dedup'd
#      via runtime.mismatch_notified).
#
# We deliberately *accept* the payload regardless of verdict — the
# scope decision is "warn loudly, accept" so a benign re-flash doesn't
# stall a live deployment.


def _async_health_for_tofu(hass: HomeAssistant, entry: SecuraCVConfigEntry):
    """Subscriber for `{prefix}/+/health` that TOFU-pins the device's
    pubkey the first time it appears.

    The health publish has always carried `public_key` (64-char hex)
    because the HA dashboard's health sensor already surfaces it as an
    attribute. We piggy-back on it for the trust store.

    Trust model — be honest about the limits of TOFU: first-sight pinning
    trusts whoever publishes to `{prefix}/{device_id}/health` FIRST. Any
    client with publish access to the broker (or a hostile broker) can
    pre-emptively pin its own key for a device_id before the genuine
    device connects, after which its spoofed publishes verify green.
    TOFU therefore upgrades an *honest* broker from "trust every payload"
    to "detect later tampering"; it does not defend against a broker (or
    co-tenant publisher) that is hostile from the start. Users whose
    threat model includes a hostile broker must pin keys manually
    (Options → Pin a device pubkey) with the key read off the device out
    of band — canary-wap's /enroll page, USB serial `j` on the
    firmware/canary build and canary-vision, the `Ed25519 pubkey` boot
    line on canary-sense and canary-sentinel from a firmware release
    after 2.4.15 (docs/device_trust.md, "Where each product shows its
    key") — and should use broker ACLs to restrict who may publish under
    the prefix.
    Subsequent publishes are verified against the pin; the warn-loudly-
    accept policy handles the "device legitimately re-flashed" case.
    """

    @callback
    def _callback(msg: mqtt.ReceiveMessage) -> None:
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return
        device_id = parts[-2]
        if not valid_device_id(device_id):
            return
        runtime = runtime_for(entry)
        if runtime is None:
            return
        trust_store = runtime.trust_store
        if trust_store.is_pinned(device_id):
            return
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        pubkey_hex = data.get("public_key")
        if not pubkey_hex or not isinstance(pubkey_hex, str) or len(pubkey_hex) != 64:
            return
        # A canary-wap on firmware 2.4.15 or older spells its key in
        # capitals; the store keeps one lowercase spelling
        # (device_trust.normalize_hex).
        pubkey_hex = normalize_hex(pubkey_hex)
        # Exactly 64 hex digits. bytes.fromhex alone is not enough: it skips
        # ASCII whitespace, so a 64-character string holding spaces decoded
        # to a short key and raised later, inside the pin task (HA22).
        if not re.fullmatch(r"[0-9a-f]{64}", pubkey_hex):
            return
        # async_pin needs the loop; schedule as a task so the @callback
        # context returns synchronously. A pin that actually lands is a new
        # identity for this device_id (an install or a full erase in either
        # flasher, whose factory image starts NVS over; an all-rounder
        # Canary's 30 s BOOT wipe; a replacement board; an OTA update and the
        # 10 s BOOT reset keep the key), whose counters start over — so its
        # replay high-water marks start over with it, or every valid publish from
        # the new key would read as a replay of the old one.
        async def _pin_and_reset_replay() -> None:
            pinned = await trust_store.async_tofu_pin_if_unknown(device_id, pubkey_hex)
            if pinned is not None:
                async_clear_replay_marks(hass, entry, device_id)

        hass.async_create_task(_pin_and_reset_replay())
        _LOGGER.info(
            "TOFU-pinning Canary %s with pubkey %s…", device_id, pubkey_hex[:16]
        )

    return _callback


@callback
def async_clear_replay_marks(hass: HomeAssistant, entry: ConfigEntry, device_id: str) -> None:
    """Forget a device's replay high-water marks.

    Called on every path that changes which key a device_id answers to —
    TOFU first sight, the operator's pin / rotate / unpin — because the
    counters the replay gate compares (event_id, seq, chain length, totals)
    belong to the key, and a new key starts them over. Without this the
    replacement device's first publishes all read as replays of the old one
    until its counters climb past the old high-water marks.
    """
    runtime = runtime_for(entry)
    if runtime is not None:
        runtime.replay.pop(device_id, None)


@callback
def async_record_verify(
    hass: HomeAssistant,
    entry: ConfigEntry,
    device_id: str,
    verdict: "TrustVerdict",
) -> None:
    """Stamp the verify outcome into the runtime and surface mismatches."""
    runtime = runtime_for(entry)
    if runtime is None:
        return
    runtime.verify[device_id] = {
        "trusted": verdict.trusted,
        "reason": verdict.reason,
        "pinned_fingerprint": verdict.pinned_fingerprint,
        "received_fingerprint": verdict.received_fingerprint,
        "detail": verdict.detail,
    }
    if verdict.reason == "tofu_pin":
        # A newly pinned device starts its counters over (an install or a full
        # erase in either flasher, an all-rounder Canary's 30 s BOOT wipe);
        # its replay high-water marks must start over with it. The verifiers
        # do not return this reason today — the health hook and the options
        # flow clear the marks themselves — but a future verifier that pins
        # inline gets the same behavior for free.
        async_clear_replay_marks(hass, entry, device_id)
    if verdict.reason == "mismatch":
        # Dedup by (device_id, received_fingerprint) so a steady stream
        # of mismatched publishes only notifies the user once. Cleared
        # when the operator either re-pins or unpins the device.
        # Lowercased so two spellings of one fingerprint are one notice.
        key = (device_id, normalize_hex(verdict.received_fingerprint or ""))
        notified = runtime.mismatch_notified
        if key in notified:
            return
        notified.add(key)
        # `persistent_notification.create` is awaitable but we're in a
        # @callback context; schedule it on the event loop. The
        # notification ID is stable per device so a follow-up create
        # replaces the previous payload rather than stacking. A Canary key
        # mismatch stays a notification: the kernel's key mismatch is the
        # Repairs issue (repairs.async_report_tofu_mismatch), because only
        # the kernel pin has a fix flow.
        hass.async_create_task(
            hass.services.async_call(
                "persistent_notification",
                "create",
                {
                    "title": f"SecuraCV: device {device_id} key mismatch",
                    "message": (
                        f"Canary `{device_id}` published with fingerprint "
                        f"`{verdict.received_fingerprint}` but the pinned "
                        f"fingerprint is `{verdict.pinned_fingerprint}`. "
                        "Entities are still updating, marked as unverified. "
                        "A Canary gets a new key when it is installed again "
                        "or fully erased in either flasher (an install starts "
                        "its settings over), from the all-rounder Canary's "
                        "30-second BOOT wipe, or with a replacement board, and "
                        "a Sense, Vision or Sentinel also makes one when it "
                        "cannot read its stored key at boot; an over-the-air "
                        "update and the 10-second BOOT reset keep it. Check "
                        "the new key on the device itself before you rotate "
                        "the pin from the integration's options menu: if none "
                        "of those explains it, something else may be "
                        "publishing as this device."
                    ),
                    "notification_id": f"securacv_mismatch_{device_id}",
                },
                blocking=False,
            )
        )


def unsigned_trust_attrs(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str
) -> dict[str, Any]:
    """Trust slice for an entity moved by a topic the firmware never signs.

    tamper / transport / mesh / chirp / health publishes carry no sig
    envelope, so nothing was verified and nothing here may say otherwise:
    `verified` is always False and `trust_reason` is "unsigned" — the same
    verdict the verifiers return for a signed-kind payload that arrives
    without its envelope, so a dashboard reads both the same way. The keys
    match sensor.py's `_trust_attrs` exactly: an unsigned publish and a
    verified one are told apart by the attribute's value, never by its
    absence. `pinned_fingerprint` is the identity the device is enrolled
    under (from its signed topics) when a pin exists, so the row still says
    which Canary this is; there is no received fingerprint to show.
    """
    pinned: str | None = None
    runtime = runtime_for(entry)
    if runtime is not None:
        pin = runtime.trust_store.get(device_id)
        if pin is not None:
            pinned = pin.fingerprint_hex
    return {
        "verified": False,
        "trust_reason": "unsigned",
        "pinned_fingerprint": pinned,
        "received_fingerprint": None,
    }
