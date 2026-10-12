"""Setup, unload, removal, migration, the kernel device and Canary liveness.

Driven through the integration's real async_setup_entry / async_unload_entry
/ async_remove_entry / async_migrate_entry with a fake kernel API and the
recording MQTT, device-registry and issue-registry stubs.
"""

from __future__ import annotations

import json
import sys
import types

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import (
    device_registry,
    issues,
    make_entry,
    make_runtime,
    notifications,
    published,
    run,
)

from homeassistant.core import HomeAssistant  # noqa: E402
from homeassistant.exceptions import ConfigEntryNotReady  # noqa: E402

import securacv as package  # noqa: E402
from securacv import (  # noqa: E402
    async_migrate_entry,
    async_remove_config_entry_device,
    async_remove_entry,
    async_setup_entry,
    async_unload_entry,
)
from securacv import entity as entity_mod  # noqa: E402
from securacv.const import (  # noqa: E402
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
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
    DOMAIN,
    ISSUE_ENTITIES_MIGRATED,
    ISSUE_NO_KERNEL_FOUND,
    ISSUE_TOFU_MISMATCH,
    MODEL_KERNEL,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    SETUP_MODE_MQTT,
)

KEY = "ab" * 32
DEVICE_ID = "pwk_1a2b3c4d"
IDENTITY = {"device_id": DEVICE_ID, "verifying_key": KEY, "kernel_version": "2.4.16"}


class FakeApi:
    """Stands in for SecuraCVApi inside the package: canned kernel answers."""

    identity: dict | None = dict(IDENTITY)
    instances: list["FakeApi"] = []

    def __init__(self, base_url, token, session, token_file=None) -> None:
        self.base_url = base_url
        self.token = token
        self.token_file = token_file
        self.token_file_unreadable = False
        self.revoked: list[str] = []
        FakeApi.instances.append(self)

    async def async_get_health(self):
        return {"ok": True}

    async def async_get_identity(self):
        return dict(self.identity) if self.identity is not None else None

    async def async_get_status(self):
        return None

    async def async_get_latest_event(self):
        return None

    async def async_get_digest(self):
        return None

    async def async_revoke_paired_token(self, token_id):
        self.revoked.append(token_id)


@pytest.fixture(autouse=True)
def _fake_api(monkeypatch):
    FakeApi.instances.clear()
    FakeApi.identity = dict(IDENTITY)
    monkeypatch.setattr(package, "SecuraCVApi", FakeApi)


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {}
    return hass


def _bridge_entity(hass, object_id: str = "last_event", domain: str = "sensor") -> None:
    """A row MQTT discovery made from the bridge's config: the `mqtt`
    platform, unique_id `<device_id>_<object>`, entity id `*.pwk_<object>`."""
    hass.data.setdefault("_securacv_test_entity_registry", []).append(
        types.SimpleNamespace(
            domain=domain,
            platform="mqtt",
            unique_id=f"{DEVICE_ID}_{object_id}",
            entity_id=f"{domain}.pwk_{object_id}",
            config_entry_id="mqtt-entry",
            original_name=None,
        )
    )


def _both_entry(hass, **extra):
    data = {
        "url": "http://kernel:8799",
        "token": "",
        CONF_TOKEN_FILE: "/config/api_token",
        CONF_AUTH_MODE: AUTH_MODE_TOKEN_FILE,
        CONF_KERNEL_DEVICE_ID: DEVICE_ID,
        CONF_KERNEL_VERIFYING_KEY: KEY,
        CONF_MQTT_TOPIC_PREFIX: "witness",
        CONF_CLAIM_NONCE: "feedfacefeedface",
        CONF_ENABLE_MQTT: True,
        CONF_MQTT_PREFIX: "securacv",
        CONF_SETUP_MODE: SETUP_MODE_BOTH,
    }
    data.update(extra)
    entry = make_entry(data=data, unique_id=f"kernel_{DEVICE_ID}")
    hass.config_entries.add(entry)
    return entry


# ── setup ───────────────────────────────────────────────────────────────


def test_both_mode_setup_subscribes_claims_and_registers_the_kernel_device():
    hass = _hass()
    issues(hass)[(DOMAIN, ISSUE_NO_KERNEL_FOUND)] = {"translation_key": ISSUE_NO_KERNEL_FOUND}
    _bridge_entity(hass)
    entry = _both_entry(hass)
    assert run(async_setup_entry(hass, entry)) is True
    runtime = entry.runtime_data
    assert runtime.setup_mode == SETUP_MODE_BOTH and runtime.mqtt_prefix == "securacv"
    assert runtime.api is not None and runtime.coordinator is not None
    assert runtime.coordinator.pinned_key == KEY
    assert runtime.coordinator.backstop_ok is True

    topics = [t for t, _cb in hass.data["_securacv_test_mqtt_subscriptions"]]
    assert topics[:8] == [f"witness/{s}" for s in (
        "last_event", "events", "digest", "chain_problem", "chain_problem/attrs", "status",
        "claim_state", "frigate_link")]
    assert topics[8:] == ["securacv/+/status", "securacv/+/availability", "securacv/+/health"]

    # The claim went out retained, and will be refreshed hourly.
    (claim,) = [m for m in published(hass) if m["topic"] == f"witness/claim/{DEVICE_ID}"]
    assert claim["qos"] == 1 and claim["retain"] is True
    assert json.loads(claim["payload"])["nonce"] == "feedfacefeedface"
    assert runtime.claim_published is True
    # The one-time migration notice, flagged so it is never raised again.
    assert (DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_{entry.entry_id}") in issues(hass)
    assert entry.data[CONF_MIGRATION_NOTICE_SHOWN] is True
    # A loaded kernel clears the J2 pointer.
    assert (DOMAIN, ISSUE_NO_KERNEL_FOUND) not in issues(hass)

    (device,) = device_registry(hass).devices
    assert device.identifiers == {(DOMAIN, "http://kernel:8799"), (DOMAIN, f"kernel_{DEVICE_ID}")}
    assert device.name == "SecuraCV" and device.model == MODEL_KERNEL
    assert device.serial_number == DEVICE_ID and device.sw_version == "2.4.16"
    assert device.configuration_url == "http://kernel:8799"
    assert all(isinstance(getattr(device, k), str) for k in ("name", "model", "sw_version"))
    assert hass.config_entries.forwarded[0][1] == ["sensor", "binary_sensor", "event", "button"]
    # The chain verifier is running for the kernel and is released with the entry.
    assert runtime.verifier is not None
    assert runtime.verifier.async_stop in runtime.unsub_mqtt


def test_the_migration_notice_is_raised_once():
    hass = _hass()
    _bridge_entity(hass)
    entry = _both_entry(hass, **{CONF_MIGRATION_NOTICE_SHOWN: True})
    run(async_setup_entry(hass, entry))
    assert (DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_{entry.entry_id}") not in issues(hass)


def test_a_fresh_install_without_the_bridge_entities_gets_no_migration_notice():
    """Nothing was replaced on an instance where MQTT discovery never made
    the bridge's pwk_* entities; the flag is still set, so the notice can
    never appear later either."""
    hass = _hass()
    hass.data["_securacv_test_entity_registry"] = [
        # Other mqtt entities, and a securacv one, are not the bridge's.
        types.SimpleNamespace(domain="sensor", platform="mqtt", unique_id="tasmota_1_power",
                              entity_id="sensor.kitchen_power", config_entry_id="mqtt-entry"),
        types.SimpleNamespace(domain="sensor", platform="securacv", unique_id="securacv_e1_latest_event",
                              entity_id="sensor.securacv_last_event", config_entry_id="e1"),
    ]
    entry = _both_entry(hass)
    assert run(async_setup_entry(hass, entry)) is True
    assert entry.runtime_data.claim_published is True
    assert (DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_{entry.entry_id}") not in issues(hass)
    assert entry.data[CONF_MIGRATION_NOTICE_SHOWN] is True


def test_the_migration_notice_recognizes_the_bridge_by_entity_id_too():
    """An entity the person renamed keeps its unique_id; one MQTT discovery
    re-keyed keeps its pwk_ object id. Either is proof the bridge's entities
    were here."""
    hass = _hass()
    hass.data["_securacv_test_entity_registry"] = [
        types.SimpleNamespace(domain="binary_sensor", platform="mqtt", unique_id="legacy-id",
                              entity_id="binary_sensor.pwk_chain_problem", config_entry_id="mqtt-entry"),
    ]
    entry = _both_entry(hass)
    run(async_setup_entry(hass, entry))
    assert (DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_{entry.entry_id}") in issues(hass)


def test_an_unreadable_registry_keeps_the_contracts_default(monkeypatch):
    er_mod = sys.modules["homeassistant.helpers.entity_registry"]

    def _boom(hass):
        raise RuntimeError("registry not loaded")

    monkeypatch.setattr(er_mod, "async_get", _boom)
    hass = _hass()
    entry = _both_entry(hass)
    run(async_setup_entry(hass, entry))
    assert (DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_{entry.entry_id}") in issues(hass)


# ── the one-time "add the dashboard" notice (J1) ─────────────────────────


def _strategy_served(hass, served: bool = True) -> None:
    hass.data.setdefault(DOMAIN, {})[package.DATA_FRONTEND_CARDS] = (
        ["securacv-timeline-card.js", "securacv-aim-card.js", "securacv-strategy.js"]
        if served
        else ["securacv-timeline-card.js", "securacv-aim-card.js"]
    )


def test_the_dashboard_notice_is_posted_once_when_the_strategy_is_served():
    hass = _hass()
    _strategy_served(hass)
    entry = _both_entry(hass)
    assert run(async_setup_entry(hass, entry)) is True
    (note,) = [n for n in notifications(hass) if n["notification_id"] == f"securacv_dashboard_{entry.entry_id}"]
    assert note["title"] == "SecuraCV: add the dashboard"
    assert "Settings → Dashboards → Add dashboard → SecuraCV" in note["message"]
    assert "https://my.home-assistant.io/redirect/lovelace_dashboards/" in note["message"]
    assert entry.data[CONF_DASHBOARD_NOTICE_SHOWN] is True
    # A reload never repeats it.
    run(async_unload_entry(hass, entry))
    hass.data["_securacv_test_notifications"] = []
    assert run(async_setup_entry(hass, entry)) is True
    assert notifications(hass) == []


def test_the_dashboard_notice_waits_for_the_strategy_module():
    """Without the strategy served, "Add dashboard → SecuraCV" does not exist
    on this instance, so the notice is not posted and the flag stays clear
    (a later load with the module posts it)."""
    hass = _hass()
    _strategy_served(hass, served=False)
    entry = _both_entry(hass)
    assert run(async_setup_entry(hass, entry)) is True
    assert not [n for n in notifications(hass) if "dashboard" in n["notification_id"]]
    assert CONF_DASHBOARD_NOTICE_SHOWN not in entry.data


def test_an_mqtt_only_entry_posts_no_dashboard_notice():
    hass = _hass()
    _strategy_served(hass)
    entry = make_entry(data={CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv",
                             CONF_SETUP_MODE: SETUP_MODE_MQTT})
    hass.config_entries.add(entry)
    assert run(async_setup_entry(hass, entry)) is True
    assert notifications(hass) == []


def test_mqtt_unavailable_is_not_ready():
    hass = _hass()
    hass.data["_securacv_test_mqtt_available"] = False
    entry = _both_entry(hass)
    with pytest.raises(ConfigEntryNotReady):
        run(async_setup_entry(hass, entry))
    assert published(hass) == []


def test_kernel_only_mode_never_touches_mqtt():
    hass = _hass()
    hass.data["_securacv_test_mqtt_available"] = False
    entry = make_entry(data={
        "url": "http://kernel:8799", "token": "tok", CONF_AUTH_MODE: AUTH_MODE_STATIC,
        CONF_KERNEL_DEVICE_ID: DEVICE_ID, CONF_KERNEL_VERIFYING_KEY: KEY,
        CONF_ENABLE_MQTT: False, CONF_SETUP_MODE: SETUP_MODE_KERNEL,
    }, unique_id=f"kernel_{DEVICE_ID}")
    hass.config_entries.add(entry)
    assert run(async_setup_entry(hass, entry)) is True
    assert "_securacv_test_mqtt_subscriptions" not in hass.data
    assert published(hass) == []
    assert entry.runtime_data.claim_published is False


def test_setup_adopts_the_identity_of_a_legacy_entry():
    """An entry from before v3 (URL-keyed, no device id, no pin) learns all
    three from the first backstop and claims the kernel on the spot."""
    hass = _hass()
    entry = make_entry(data={
        "url": "http://kernel:8799", "token": "", CONF_TOKEN_FILE: "/config/api_token",
        CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_BOTH,
    }, unique_id="http://kernel:8799")
    hass.config_entries.add(entry)
    assert run(async_setup_entry(hass, entry)) is True
    assert entry.unique_id == f"kernel_{DEVICE_ID}"
    assert entry.data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert len(entry.data[CONF_CLAIM_NONCE]) == 16
    assert any(m["topic"] == f"witness/claim/{DEVICE_ID}" for m in published(hass))


def test_setup_refuses_to_adopt_a_different_kernel(caplog):
    hass = _hass()
    entry = _both_entry(hass, **{CONF_KERNEL_DEVICE_ID: "pwk_ffffffff"})
    FakeApi.identity = dict(IDENTITY)
    with caplog.at_level("WARNING"):
        run(async_setup_entry(hass, entry))
    assert entry.data[CONF_KERNEL_DEVICE_ID] == "pwk_ffffffff"
    assert any("reconfigure" in r.getMessage() for r in caplog.records)
    # The claim is for the configured id, never the one that answered.
    assert [m["topic"] for m in published(hass)] == ["witness/claim/pwk_ffffffff"]


def test_a_served_key_that_differs_from_the_pin_is_reported_at_setup():
    hass = _hass()
    FakeApi.identity = {**IDENTITY, "verifying_key": "cd" * 32}
    entry = _both_entry(hass)
    run(async_setup_entry(hass, entry))
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}") in issues(hass)
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == KEY


# ── unload and removal ──────────────────────────────────────────────────


def test_unload_releases_everything_but_keeps_the_claim():
    hass = _hass()
    entry = _both_entry(hass)
    run(async_setup_entry(hass, entry))
    runtime = entry.runtime_data
    assert len(runtime.unsub_mqtt) >= 4
    before = list(published(hass))
    assert run(async_unload_entry(hass, entry)) is True
    assert runtime.unsub_mqtt == []
    assert published(hass) == before, "unload never withdraws the claim"
    assert hass.config_entries.unloaded[0][1] == ["sensor", "binary_sensor", "event", "button"]


def test_remove_entry_withdraws_the_claim_revokes_the_token_and_clears_issues(monkeypatch):
    from securacv import kernel_verify

    removed_stores: list[str] = []
    real_store = kernel_verify.Store

    class _RecordingStore(real_store):  # type: ignore[misc,valid-type]
        async def async_remove(self):
            removed_stores.append(self._key)
            await super().async_remove()

    monkeypatch.setattr(kernel_verify, "Store", _RecordingStore)
    hass = _hass()
    _strategy_served(hass)
    entry = _both_entry(hass, **{CONF_AUTH_MODE: AUTH_MODE_PAIRED, "token": "f1" * 32,
                                 CONF_KERNEL_TOKEN_ID: "0a1b2c3d"})
    run(async_setup_entry(hass, entry))
    # An export's and the dashboard notice outlive nothing: both go with the entry.
    hass.data["_securacv_test_notifications"].append(
        {"message": "m", "title": "t", "notification_id": f"securacv_export_{entry.entry_id}"}
    )
    hass.data["_securacv_test_notifications"].append(
        {"message": "m", "title": "t", "notification_id": "securacv_mismatch_canary01"}
    )
    assert any(n["notification_id"] == f"securacv_dashboard_{entry.entry_id}" for n in notifications(hass))
    run(async_unload_entry(hass, entry))
    run(async_remove_entry(hass, entry))
    withdrawal = published(hass)[-1]
    assert withdrawal == {"topic": f"witness/claim/{DEVICE_ID}", "payload": b"", "qos": 1, "retain": True}
    assert FakeApi.instances[-1].revoked == ["0a1b2c3d"]
    assert FakeApi.instances[-1].token == "f1" * 32
    assert not any(key[1].endswith(entry.entry_id) for key in issues(hass))
    # The verifier's cursor and covered index are deleted with the entry.
    assert removed_stores == [f"securacv.verify_{entry.entry_id}"]
    # The entry's notifications are dismissed; a Canary's key notice is per device and stays.
    assert [n["notification_id"] for n in notifications(hass)] == ["securacv_mismatch_canary01"]


def test_remove_entry_without_mqtt_or_pairing_is_quiet():
    hass = _hass()
    entry = make_entry(data={"url": "http://kernel:8799", "token": "tok",
                             CONF_AUTH_MODE: AUTH_MODE_STATIC, CONF_SETUP_MODE: SETUP_MODE_KERNEL})
    run(async_remove_entry(hass, entry))
    assert published(hass) == []


def test_remove_config_entry_device_rules():
    hass = _hass()
    entry = _both_entry(hass)
    runtime = make_runtime(hass, devices={"canary-live": {"status": "online"}})
    run(runtime.trust_store.async_pin("canary-pinned", "11" * 32))
    entry.runtime_data = runtime

    def device(*identifiers):
        return types.SimpleNamespace(identifiers=set(identifiers))

    assert run(async_remove_config_entry_device(hass, entry, device((DOMAIN, f"kernel_{DEVICE_ID}")))) is False
    assert run(async_remove_config_entry_device(hass, entry, device((DOMAIN, "http://kernel:8799")))) is False
    assert run(async_remove_config_entry_device(hass, entry, device((DOMAIN, "canary_canary-live")))) is False
    assert run(async_remove_config_entry_device(hass, entry, device((DOMAIN, "canary_canary-pinned")))) is False
    assert run(async_remove_config_entry_device(hass, entry, device((DOMAIN, "canary_canary-gone")))) is True
    assert run(async_remove_config_entry_device(hass, entry, device(("other", "x")))) is True


# ── migration ───────────────────────────────────────────────────────────


def test_v2_kernel_entry_migrates_to_v3_with_the_identity():
    hass = _hass()
    entry = make_entry(version=2, unique_id="http://kernel:8799", data={
        "url": "http://kernel:8799", "token": "", CONF_TOKEN_FILE: "/config/api_token",
        CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_BOTH,
    })
    hass.config_entries.add(entry)
    assert run(async_migrate_entry(hass, entry)) is True
    assert entry.version == 3
    assert entry.unique_id == f"kernel_{DEVICE_ID}"
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert entry.data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert len(entry.data[CONF_CLAIM_NONCE]) == 16
    assert entry.data[CONF_MQTT_PREFIX] == "securacv", "existing data survives"


def test_v2_static_token_entry_migrates_without_a_kernel_answer():
    hass = _hass()
    FakeApi.identity = None
    entry = make_entry(version=1, unique_id="http://kernel:8799", data={
        "url": "http://kernel:8799", "token": "tok", CONF_ENABLE_MQTT: False,
        CONF_SETUP_MODE: SETUP_MODE_KERNEL,
    })
    hass.config_entries.add(entry)
    assert run(async_migrate_entry(hass, entry)) is True
    assert entry.version == 3
    assert entry.unique_id == "http://kernel:8799", "adopted later, by setup, when the kernel answers"
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_STATIC
    assert CONF_KERNEL_DEVICE_ID not in entry.data


def test_migration_never_steals_a_unique_id_another_entry_holds():
    hass = _hass()
    hass.config_entries.add(make_entry(entry_id="other", unique_id=f"kernel_{DEVICE_ID}", data={}))
    entry = make_entry(version=2, unique_id="hassio", data={
        "url": "http://kernel:8799", CONF_TOKEN_FILE: "/config/api_token",
        CONF_SETUP_MODE: SETUP_MODE_BOTH, CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv",
    })
    hass.config_entries.add(entry)
    assert run(async_migrate_entry(hass, entry)) is True
    assert entry.unique_id == "hassio"
    assert entry.data[CONF_KERNEL_DEVICE_ID] == DEVICE_ID


def test_mqtt_only_entries_and_future_versions():
    hass = _hass()
    entry = make_entry(version=2, unique_id="mqtt_securacv", data={
        CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT,
    })
    hass.config_entries.add(entry)
    assert run(async_migrate_entry(hass, entry)) is True
    assert entry.version == 3 and CONF_AUTH_MODE not in entry.data
    assert run(async_migrate_entry(hass, make_entry(version=3, data={}))) is True
    assert run(async_migrate_entry(hass, make_entry(version=4, data={}))) is False


# ── Canary liveness and the device link ─────────────────────────────────


def test_canary_status_registers_the_device_linked_to_the_kernel_and_tracks_liveness():
    hass = _hass()
    entry = _both_entry(hass)
    run(async_setup_entry(hass, entry))
    callbacks = dict(hass.data["_securacv_test_mqtt_subscriptions"])
    status_cb = callbacks["securacv/+/status"]
    avail_cb = callbacks["securacv/+/availability"]
    signals = hass.data.setdefault("_securacv_test_dispatcher", {})
    heard: list[str] = []
    signals[entity_mod.canary_status_signal(entry.entry_id, "canary01")] = [lambda: heard.append("x")]

    status_cb(types.SimpleNamespace(topic="securacv/canary01/status", payload=json.dumps({
        "online": True, "firmware_version": "2.4.16", "hardware": "esp32s3",
        "device_name": "Front gate", "ap_ip": "192.168.4.1",
    }).encode()))
    devices = entry.runtime_data.devices
    assert devices["canary01"]["online"] is True
    assert heard == ["x"]
    canary = device_registry(hass).async_get_device(identifiers={(DOMAIN, "canary_canary01")})
    assert canary.name == "Front gate" and canary.sw_version == "2.4.16"
    assert canary.configuration_url == "http://192.168.4.1"
    # Linked to the kernel through the shim (the pre-2026.8 shape under the stub).
    assert canary.via_device == (DOMAIN, f"kernel_{DEVICE_ID}")

    # The last will on the availability topic takes the device down, once.
    avail_cb(types.SimpleNamespace(topic="securacv/canary01/availability", payload=b"offline"))
    avail_cb(types.SimpleNamespace(topic="securacv/canary01/availability", payload=b"offline"))
    assert devices["canary01"]["online"] is False
    assert heard == ["x", "x"], "no re-render for the same state"
    status_cb(types.SimpleNamespace(topic="securacv/canary01/status", payload=b'{"online": false}'))
    assert devices["canary01"]["online"] is False
    avail_cb(types.SimpleNamespace(topic="securacv/canary01/availability", payload="online"))
    assert devices["canary01"]["online"] is True

    # Hostile or malformed input never mints a device.
    status_cb(types.SimpleNamespace(topic="securacv/../status", payload=b"online"))
    status_cb(types.SimpleNamespace(topic="securacv/canary02/status", payload=b"\xff\xfe"))
    assert set(devices) == {"canary01"}

    # A device name that is not a string, or too long, never reaches the
    # registry as published: every DeviceInfo value is a bounded str.
    status_cb(types.SimpleNamespace(topic="securacv/canary03/status", payload=json.dumps({
        "online": True, "device_name": {"evil": 1},
    }).encode()))
    status_cb(types.SimpleNamespace(topic="securacv/canary04/status", payload=json.dumps({
        "online": True, "device_name": "x" * 500,
    }).encode()))
    status_cb(types.SimpleNamespace(topic="securacv/canary05/status", payload=json.dumps({
        "online": True, "device_name": "  Back   garden  ",
    }).encode()))
    registry = device_registry(hass)
    assert registry.async_get_device(identifiers={(DOMAIN, "canary_canary03")}).name == (
        "SecuraCV Canary canary03"
    )
    assert registry.async_get_device(identifiers={(DOMAIN, "canary_canary04")}).name == "x" * 64
    assert registry.async_get_device(identifiers={(DOMAIN, "canary_canary05")}).name == "Back garden"


def test_canary_entities_follow_their_devices_liveness():
    from securacv import binary_sensor as bs_platform
    from securacv import sensor as sensor_platform

    hass = _hass()
    entry = make_entry(hass, runtime=make_runtime(hass))
    sensor = sensor_platform.SecuraCVCanaryWitnessCountSensor("securacv", "canary01", entry)
    online = bs_platform.SecuraCVCanaryOnlineSensor("securacv", "canary01", entry)
    assert sensor.available is True, "never heard from: shown"
    entry.runtime_data.devices["canary01"] = {"status": "offline", "online": False}
    assert sensor.available is False
    assert online.available is True, "the connectivity sensor is the one that says off"
    entry.runtime_data.devices["canary01"]["online"] = True
    assert sensor.available is True
    assert "via_device" not in sensor.device_info and "via_device_id" not in sensor.device_info


def test_via_device_shim_speaks_both_registry_apis(monkeypatch):
    hass = _hass()
    entry = _both_entry(hass)
    dr_mod = sys.modules["homeassistant.helpers.device_registry"]
    # Pre-2026.8: the identifier tuple.
    assert entity_mod.kernel_via_device_kwargs(hass, entry) == {"via_device": (DOMAIN, f"kernel_{DEVICE_ID}")}
    # 2026.8+: the kernel device's registry id, once it exists.
    monkeypatch.setattr(dr_mod.DeviceInfo, "__annotations__",
                        {**dr_mod.DeviceInfo.__annotations__, "via_device_id": str})
    assert entity_mod.kernel_via_device_kwargs(hass, entry) == {}, "not registered yet"
    kernel = device_registry(hass).async_get_or_create(
        config_entry_id=entry.entry_id, identifiers={(DOMAIN, f"kernel_{DEVICE_ID}")})
    assert entity_mod.kernel_via_device_kwargs(hass, entry) == {"via_device_id": kernel.id}
    # An entry without a kernel links nothing.
    assert entity_mod.kernel_via_device_kwargs(hass, make_entry(data={})) == {}


def test_kernel_device_info_is_all_strings():
    entry = make_entry(data={"url": "http://kernel:8799", CONF_KERNEL_DEVICE_ID: DEVICE_ID})
    info = entity_mod.kernel_device_info(entry, kernel_version=2.4)
    assert info["identifiers"] == {(DOMAIN, "http://kernel:8799"), (DOMAIN, f"kernel_{DEVICE_ID}")}
    assert info["name"] == "SecuraCV" and info["model"] == MODEL_KERNEL
    assert info["sw_version"] == "2.4" and info["serial_number"] == DEVICE_ID
    for key, value in info.items():
        if key != "identifiers":
            assert isinstance(value, str), key
    assert "sw_version" not in entity_mod.kernel_device_info(make_entry(data={"url": "http://k:1"}))
