"""The real-Home-Assistant lane: fixtures over a live core.

Everything under tests/ha runs against the Home Assistant release
pytest-homeassistant-custom-component pins (two cells in CI: the floor the
integration declares in hacs.json, and the current release). Nothing here is
a stub — ``hass`` is a real core, ``mqtt_mock`` the real MQTT integration
over a mocked paho client, ``aioclient_mock`` the real aiohttp session the
integration gets from ``async_get_clientsession``. The kernel is the only
thing mocked, at its HTTP surface, by ``KernelMock`` below.

Runner recipe (also in tests/ha/pytest.ini and .github/workflows/python.yml):

    pytest -c tests/ha/pytest.ini --confcutdir=tests/ha tests/ha

The repo-root conftest.py (the stub lane's module stubs) is gated so it
installs nothing when a real core is importable and the stub suite is not
the target; the ``--confcutdir`` keeps it from being collected at all.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
import sys
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import aiohttp
import pytest
from cryptography.hazmat.primitives.asymmetric import ed25519

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from homeassistant.const import CONF_TOKEN, CONF_URL  # noqa: E402
from homeassistant.core import HomeAssistant  # noqa: E402
from pytest_homeassistant_custom_component.common import MockConfigEntry  # noqa: E402
from pytest_homeassistant_custom_component.test_util.aiohttp import (  # noqa: E402
    AiohttpClientMocker,
    AiohttpClientMockResponse,
)

from custom_components.securacv.const import (  # noqa: E402
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_KEY_PINNED_BUCKET_START,
    CONF_KERNEL_TOKEN_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_SETUP_MODE,
    CONF_TOKEN_FILE,
    DEFAULT_MQTT_PREFIX,
    DEFAULT_MQTT_TOPIC_PREFIX,
    DOMAIN,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    SETUP_MODE_MQTT,
    TIME_BUCKET_SECONDS,
)

# ── the kernel this lane talks to ───────────────────────────────────────

KERNEL_HOST = "192.168.1.20"
KERNEL_PORT = 8799
KERNEL_URL = f"http://{KERNEL_HOST}:{KERNEL_PORT}"
KERNEL_DEVICE_ID = "pwk_1a2b3c4d"
KERNEL_VERSION = "2.4.16"
RULESET_ID = "ruleset:frigate_v1"
STATIC_TOKEN = "ab" * 32
PAIRED_TOKEN = "cd" * 32
PAIRED_TOKEN_ID = "0badf00d"
CLAIM_NONCE = "0123456789abcdef"
# The shared sealed-log vector the three verifiers (Rust, JS, Python) agree on.
SEALED_LOG_VECTOR_PATH = REPO_ROOT / "tests" / "fixtures" / "envelope" / "sealed_log_document_vector.json"
SEALED_LOG_VECTOR: dict[str, Any] = json.loads(SEALED_LOG_VECTOR_PATH.read_text(encoding="utf-8"))
VECTOR_KEY: str = SEALED_LOG_VECTOR["verifying_key"]
# The vector's entries live in November 2023; tests that walk it freeze time there.
VECTOR_NOW = 1700001500.0  # inside the live window of the vector's buckets (coordinator.event_is_live)

# The sealed-log entry signing domain (src/crypto/signatures.rs; verify_core.js).
SEALED_LOG_DOMAIN = b"securacv:pwk:sealed-log-entry:v2"
GENESIS = "0" * 64


def bucket_start(now: float | None = None) -> int:
    """The 10-minute bucket start of `now` (coarse time)."""
    stamp = time.time() if now is None else now
    return int(stamp // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS


def fingerprint_of(key_hex: str) -> str:
    """First 8 hex of sha256 over the raw key bytes — what `pair` prints."""
    return hashlib.sha256(bytes.fromhex(key_hex)).hexdigest()[:8]


# ── a signing kernel: sealed logs and announces the tests can shape ─────


class SigningKernel:
    """An Ed25519 identity that seals entries and signs announces.

    Written from the contracts, not from the integration: entry hash =
    SHA256(prev_hash || payload), the signature over
    SHA256(le32(len(domain)) || domain || entry_hash); the announce signature
    over ``securacv-announce|v1|`` + the compact JSON of the payload without
    ``sig`` in the order the keys were written.
    """

    def __init__(self, device_id: str = KERNEL_DEVICE_ID) -> None:
        """Mint a fresh key."""
        self.private = ed25519.Ed25519PrivateKey.generate()
        self.verifying_key = self.private.public_key().public_bytes_raw().hex()
        self.device_id = device_id
        self.entries: list[dict[str, Any]] = []

    @property
    def fingerprint(self) -> str:
        """The key's 8-hex fingerprint."""
        return fingerprint_of(self.verifying_key)

    @property
    def head_hash(self) -> str:
        """The newest entry's hash (genesis when empty)."""
        return self.entries[-1]["entry_hash"] if self.entries else GENESIS

    def seal(self, payload: dict[str, Any]) -> dict[str, Any]:
        """Append one sealed entry for `payload` and return the served row."""
        payload_json = json.dumps(payload, separators=(",", ":"))
        prev = self.head_hash
        entry_hash = hashlib.sha256(bytes.fromhex(prev) + payload_json.encode("utf-8")).digest()
        message = hashlib.sha256(
            struct.pack("<I", len(SEALED_LOG_DOMAIN)) + SEALED_LOG_DOMAIN + entry_hash
        ).digest()
        signature = self.private.sign(message)
        seq = len(self.entries) + 1
        row = {
            "entry_hash": entry_hash.hex(),
            "id": seq,
            "payload": payload_json,
            "prev_hash": prev,
            "seq": seq,
            "signature": signature.hex(),
        }
        self.entries.append(row)
        return row

    def seal_event(
        self,
        event_type: str = "BoundaryCrossingObjectLarge",
        zone: str = "front_door",
        bucket: int | None = None,
    ) -> dict[str, Any]:
        """Seal one witness event record."""
        return self.seal(
            {
                "record_type": "event",
                "event_type": event_type,
                "zone_id": f"zone:{zone}",
                "time_bucket": {
                    "start_epoch_s": bucket_start() if bucket is None else bucket,
                    "size_s": TIME_BUCKET_SECONDS,
                },
            }
        )

    def document(self, from_seq: int | None = None) -> dict[str, Any]:
        """The GET /api/sealed-log[?from_seq=] document (contract C4 as amended)."""
        entries = self.entries if from_seq is None else [e for e in self.entries if e["seq"] > from_seq]
        doc: dict[str, Any] = {
            "entries": [dict(e) for e in entries],
            "head": {"entry_hash": self.head_hash, "seq": len(self.entries)},
            "verifying_key": self.verifying_key,
        }
        if from_seq is not None:
            doc["from_seq"] = from_seq
            anchor = next((e["entry_hash"] for e in self.entries if e["seq"] == from_seq), None)
            if anchor is not None:
                doc["checkpoint_head"] = anchor
        return doc

    def announce(self, **overrides: Any) -> dict[str, Any]:
        """A signed ``witness/<id>/announce`` payload in the bridge's key order."""
        payload: dict[str, Any] = {
            "v": 1,
            "device_id": self.device_id,
            "kernel_version": KERNEL_VERSION,
            "verifying_key": self.verifying_key,
            "api": {"scheme": "http", "bind": "all", "port": KERNEL_PORT},
            "pairing": "required",
            "mqtt_topic_prefix": DEFAULT_MQTT_TOPIC_PREFIX,
            "discovery_prefix": "homeassistant",
            "availability_topic": f"{DEFAULT_MQTT_TOPIC_PREFIX}/status",
            "announced_bucket_start": bucket_start(),
            "announced_bucket_size": TIME_BUCKET_SECONDS,
        }
        payload.update(overrides)
        body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        payload["sig"] = self.private.sign(b"securacv-announce|v1|" + body).hex()
        return payload


# ── the C5 event payload the bridge publishes ───────────────────────────


def bridge_event(
    *,
    event_type: str = "BoundaryCrossingObjectLarge",
    zone: str = "front_door",
    bucket: int | None = None,
    seq: int | None = None,
    device_id: str = KERNEL_DEVICE_ID,
    **extra: Any,
) -> dict[str, Any]:
    """One ``witness/events`` / ``witness/last_event`` payload (contract C5)."""
    start = bucket_start() if bucket is None else bucket
    payload: dict[str, Any] = {
        "event_type": event_type,
        "zone_id": f"zone:{zone}",
        "time_bucket": {"start_epoch_s": start, "size_s": TIME_BUCKET_SECONDS},
        "time_bucket_start": start,
        "time_bucket_size": TIME_BUCKET_SECONDS,
        "confidence": 0.87,
        "attestation": "adapter",
        "kernel_version": KERNEL_VERSION,
        "ruleset_id": RULESET_ID,
        "device_id": device_id,
        "published_bucket_start": start,
        "published_bucket_size": TIME_BUCKET_SECONDS,
    }
    if seq is not None:
        payload["seq"] = seq
    payload.update(extra)
    return payload


def digest_document(total: int = 3) -> dict[str, Any]:
    """A /digest document (also the retained ``witness/digest``)."""
    return {
        "total_events": total,
        "per_zone": {"zone:front_door": total},
        "per_period": {"night": 0, "morning": total, "afternoon": 0, "evening": 0},
        "event_types": {"BoundaryCrossingObjectLarge": total},
        "window_secs": 86400,
        "retention_days": 30,
        "generated_at_bucket_start": bucket_start(),
        "generated_at_bucket_size": TIME_BUCKET_SECONDS,
    }


def status_document(storage_status: str = "good", wear_pct: float | None = 12.5) -> dict[str, Any]:
    """A /status document (the kernel's storage sampler)."""
    return {
        "kernel_version": KERNEL_VERSION,
        "storage": {
            "status": storage_status,
            "free_bytes": 7_000_000_000,
            "total_bytes": 16_000_000_000,
            "free_pct": 43.75,
            "db_bytes": 1_000_000,
            "wal_bytes": 0,
            "write_errors": 0,
            "write_rate_bytes_per_day": 1_048_576,
            "lifetime_bytes_written": 2_000_000_000,
            "endurance_tbw": 16,
            "wear_pct": wear_pct,
            "estimated_days_remaining": 9000,
            "source_device": "/dev/mmcblk0",
        },
        "thermal": {"soc_temp_c": 45.5, "status": "ok"},
        "time_bucket": {"start_epoch_s": bucket_start(), "size_s": TIME_BUCKET_SECONDS},
    }


def export_bundle_bytes(events: int = 2) -> bytes:
    """A stand-in GET /export/bundle body with `events` sealed events."""
    bundle = {
        "artifact": {
            "batches": [{"buckets": [{"events": [{"event_type": "BoundaryCrossingObjectLarge"}] * events}]}]
        },
        "receipt": {"auth_mode": "paired", "token_id": PAIRED_TOKEN_ID},
    }
    return json.dumps(bundle).encode("utf-8")


# ── the kernel's HTTP surface ───────────────────────────────────────────


@dataclass
class KernelMock:
    """The kernel API on ``aioclient_mock``: every route answers from the
    fields below, so a test flips a field and the next request sees it.

    ``kernel`` is the identity that signs the sealed log and the announce;
    ``verifying_key`` defaults to its key (the vector tests swap in the
    vector's). The HTTP mocker records every call in ``mock.mock_calls``.
    """

    mock: AiohttpClientMocker
    kernel: SigningKernel
    url: str = KERNEL_URL
    verifying_key: str | None = None
    kernel_version: str = KERNEL_VERSION
    health_status: int = 200
    identity_status: int = 200
    identity_overrides: dict[str, Any] = field(default_factory=dict)
    status: dict[str, Any] | None = field(default_factory=status_document)
    status_http: int = 200
    digest: dict[str, Any] | None = field(default_factory=digest_document)
    latest_event: dict[str, Any] | None = None
    latest_event_status: int = 200
    sealed_log: dict[str, Any] | None = None
    sealed_log_status: int = 200
    bundle: bytes = field(default_factory=export_bundle_bytes)
    bundle_filename: str = "securacv-events-1700000400.json"
    bundle_status: int = 200
    pair_status: int = 200
    # The `error` word of a non-200 pair answer; None picks the kernel's
    # default for the status (401 invalid_pairing_code, else forbidden).
    pair_error: str | None = None
    pair_code_accepted: str = "483921"
    revoke_status: int = 200
    revoked: list[str] = field(default_factory=list)
    paired: list[dict[str, Any]] = field(default_factory=list)
    # True: every route fails at the socket (the kernel is down).
    connection_down: bool = False

    def __post_init__(self) -> None:
        """Register every route."""
        if self.verifying_key is None:
            self.verifying_key = self.kernel.verifying_key
        self.mock.get(f"{self.url}/health", side_effect=self._health)
        self.mock.get(f"{self.url}/api/identity", side_effect=self._identity)
        self.mock.get(f"{self.url}/status", side_effect=self._status)
        self.mock.get(f"{self.url}/digest", side_effect=self._digest)
        self.mock.get(f"{self.url}/events/latest", side_effect=self._latest)
        self.mock.get(f"{self.url}/api/sealed-log", side_effect=self._sealed_log)
        self.mock.get(f"{self.url}/export/bundle", side_effect=self._bundle)
        self.mock.post(f"{self.url}/api/pair", side_effect=self._pair)
        self.mock.delete(
            re.compile(rf"^{re.escape(self.url)}/api/pair/[0-9a-f]{{8}}$"),
            side_effect=self._revoke,
        )

    # the identity ----------------------------------------------------------

    def identity(self) -> dict[str, Any]:
        """The GET /api/identity document (contract C4 as amended)."""
        doc: dict[str, Any] = {
            "v": 1,
            "device_id": self.kernel.device_id,
            "verifying_key": self.verifying_key,
            "key_algorithm": "ed25519",
            "kernel_version": self.kernel_version,
            "ruleset_id": RULESET_ID,
            "chain_length": len(self.kernel.entries),
            "latest_hash": self.kernel.head_hash,
            "last_verify": None,
            "fleet_rollcall": "self",
            "paired_tokens": len(self.paired),
        }
        doc.update(self.identity_overrides)
        return doc

    # the routes --------------------------------------------------------------

    def _reply(self, method: str, url: Any, status: int, *, json_body: Any = None, content: bytes | None = None, headers: dict[str, str] | None = None) -> AiohttpClientMockResponse:
        if self.connection_down:
            return AiohttpClientMockResponse(
                method=method, url=url, exc=aiohttp.ClientConnectionError("kernel down")
            )
        return AiohttpClientMockResponse(
            method=method,
            url=url,
            status=status,
            json=json_body if content is None else None,
            response=content,
            headers=headers,
        )

    async def _health(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.health_status != 200:
            return self._reply(method, url, self.health_status, json_body={"error": "unavailable"})
        return self._reply(method, url, 200, json_body={"status": "ok", "kernel_version": self.kernel_version})

    async def _identity(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.identity_status != 200:
            return self._reply(method, url, self.identity_status, json_body={"error": "unauthorized"})
        return self._reply(method, url, 200, json_body=self.identity())

    async def _status(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.status is None or self.status_http != 200:
            return self._reply(method, url, 404 if self.status is None else self.status_http, json_body={"error": "not_available"})
        return self._reply(method, url, 200, json_body=self.status)

    async def _digest(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.digest is None:
            return self._reply(method, url, 404, json_body={"error": "not_found"})
        return self._reply(method, url, 200, json_body=self.digest)

    async def _latest(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.latest_event_status != 200:
            return self._reply(method, url, self.latest_event_status, json_body={"error": "unauthorized"})
        if self.latest_event is None:
            return self._reply(method, url, 404, json_body={"error": "not_found"})
        return self._reply(method, url, 200, json_body=self.latest_event)

    async def _sealed_log(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.sealed_log_status != 200:
            return self._reply(method, url, self.sealed_log_status, json_body={"error": "unauthorized"})
        from_seq = url.query.get("from_seq")
        if self.sealed_log is not None:
            doc = dict(self.sealed_log)
            if from_seq is not None:
                cursor = int(from_seq)
                doc["entries"] = [e for e in doc.get("entries", []) if int(e.get("seq", e.get("id", 0))) > cursor]
                doc["from_seq"] = cursor
            return self._reply(method, url, 200, json_body=doc)
        return self._reply(
            method, url, 200, json_body=self.kernel.document(int(from_seq) if from_seq is not None else None)
        )

    async def _bundle(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.bundle_status != 200:
            return self._reply(method, url, self.bundle_status, json_body={"error": "unauthorized"})
        return self._reply(
            method,
            url,
            200,
            content=self.bundle,
            headers={
                "Content-Type": "application/json",
                "Content-Disposition": f'attachment; filename="{self.bundle_filename}"',
            },
        )

    async def _pair(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        if self.pair_status != 200:
            word = self.pair_error or ("invalid_pairing_code" if self.pair_status == 401 else "forbidden")
            return self._reply(method, url, self.pair_status, json_body={"error": word})
        body = data if isinstance(data, dict) else json.loads(data or b"{}")
        if body.get("code") != self.pair_code_accepted:
            return self._reply(method, url, 401, json_body={"error": "invalid_pairing_code"})
        token_id = f"{len(self.paired) + 1:08x}" if self.paired else PAIRED_TOKEN_ID
        self.paired.append({"id": token_id, "label": body.get("label", "home-assistant")})
        return self._reply(
            method,
            url,
            200,
            json_body={
                "token": PAIRED_TOKEN,
                "token_id": token_id,
                "device_id": self.kernel.device_id,
                "verifying_key": self.verifying_key,
                "kernel_version": self.kernel_version,
            },
        )

    async def _revoke(self, method: str, url: Any, data: Any) -> AiohttpClientMockResponse:
        token_id = str(url).rsplit("/", 1)[-1]
        self.revoked.append(token_id)
        if self.revoke_status != 200:
            return self._reply(method, url, self.revoke_status, json_body={"error": "not_found"})
        return self._reply(method, url, 200, json_body={"revoked": token_id})

    # what the integration asked --------------------------------------------

    def calls(self, path: str, method: str = "GET") -> list[tuple[Any, ...]]:
        """The recorded calls to `path` (query string ignored)."""
        return [
            call
            for call in self.mock.mock_calls
            if call[0].upper() == method.upper() and call[1].path == path
        ]

    def auth_headers(self, path: str) -> list[str | None]:
        """The Authorization header of every call to `path`."""
        return [
            (call[3] or {}).get("Authorization") if call[3] is not None else None
            for call in self.calls(path)
        ]


# ── config entries ───────────────────────────────────────────────────────


def kernel_entry_data(
    *,
    auth_mode: str = AUTH_MODE_STATIC,
    enable_mqtt: bool = False,
    device_id: str = KERNEL_DEVICE_ID,
    verifying_key: str | None = VECTOR_KEY,
    url: str = KERNEL_URL,
    token_file: str | None = None,
    **overrides: Any,
) -> dict[str, Any]:
    """The data of a kernel entry (contracts C2 / C6), with overrides."""
    data: dict[str, Any] = {
        CONF_URL: url,
        CONF_TOKEN: PAIRED_TOKEN if auth_mode == AUTH_MODE_PAIRED else (STATIC_TOKEN if auth_mode == AUTH_MODE_STATIC else ""),
        CONF_AUTH_MODE: auth_mode,
        CONF_KERNEL_DEVICE_ID: device_id,
        CONF_MQTT_TOPIC_PREFIX: DEFAULT_MQTT_TOPIC_PREFIX,
        CONF_CLAIM_NONCE: CLAIM_NONCE,
        CONF_ENABLE_MQTT: enable_mqtt,
        CONF_SETUP_MODE: SETUP_MODE_BOTH if enable_mqtt else SETUP_MODE_KERNEL,
    }
    if verifying_key is not None:
        data[CONF_KERNEL_VERIFYING_KEY] = verifying_key
        data[CONF_KERNEL_KEY_PINNED_BUCKET_START] = bucket_start()
    if auth_mode == AUTH_MODE_PAIRED:
        data[CONF_KERNEL_TOKEN_ID] = PAIRED_TOKEN_ID
    if auth_mode == AUTH_MODE_TOKEN_FILE:
        data[CONF_TOKEN_FILE] = token_file or "/config/api_token"
    if enable_mqtt:
        data[CONF_MQTT_PREFIX] = DEFAULT_MQTT_PREFIX
    data.update(overrides)
    return data


def make_kernel_entry(*, unique_id: str | None = None, options: dict[str, Any] | None = None, **kwargs: Any) -> MockConfigEntry:
    """A VERSION 3 kernel entry, not yet added to hass."""
    data = kernel_entry_data(**kwargs)
    return MockConfigEntry(
        domain=DOMAIN,
        title="SecuraCV",
        data=data,
        options=options or {},
        unique_id=unique_id or f"kernel_{data[CONF_KERNEL_DEVICE_ID]}",
        version=3,
    )


def make_mqtt_only_entry(prefix: str = DEFAULT_MQTT_PREFIX) -> MockConfigEntry:
    """The Canaries-only entry the Automatic flow creates without a kernel."""
    return MockConfigEntry(
        domain=DOMAIN,
        title=f"SecuraCV ({prefix})",
        data={CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: prefix, CONF_SETUP_MODE: SETUP_MODE_MQTT},
        unique_id=f"mqtt_{prefix}",
        version=3,
    )


# ── what the MQTT client is subscribed to ───────────────────────────────


def subscribed_topics(hass: HomeAssistant) -> set[str]:
    """Every topic filter the real MQTT client currently holds a subscription for.

    Read off the client's own tables (the plain and the wildcard ones); an
    integration that unloads cleanly leaves none of its filters behind.
    """
    # Under mqtt_mock the client is a MagicMock wrapping the real one: method
    # calls (.items(), .keys()) pass through to the real tables, iteration of
    # the mock itself would not.
    client = hass.data["mqtt"].client
    topics: set[str] = set()
    for topic, subs in client._simple_subscriptions.items():
        if subs:
            topics.add(topic)
    for sub in client._wildcard_subscriptions.keys():
        topics.add(sub.topic)
    return topics


async def settle(hass: HomeAssistant) -> None:
    """Let every pending job finish, the entry-bound background tasks included.

    The verifier's chain walks run as ``entry.async_create_background_task``
    tasks, which a plain ``async_block_till_done()`` does not wait for.
    """
    await hass.async_block_till_done(wait_background_tasks=True)


# ── registry lookups that work on both cells ────────────────────────────


def device_by_identifier(hass: HomeAssistant, identifier: tuple[str, str], entry_id: str) -> Any:
    """The device registry entry for `identifier`, on 2026.5 and 2026.10 alike.

    2026.10 deprecated ``async_get_device(identifiers=...)`` (and refuses it
    from a non-integration frame, which a test is) in favor of the
    config-entry-scoped ``async_get_device_by_identifier``; the floor has only
    the old form.
    """
    from homeassistant.helpers import device_registry as dr

    registry = dr.async_get(hass)
    lookup = getattr(registry, "async_get_device_by_identifier", None)
    if lookup is not None:
        return lookup(identifier, entry_id)
    return registry.async_get_device(identifiers={identifier})


def kernel_device(hass: HomeAssistant, entry: MockConfigEntry, device_id: str = KERNEL_DEVICE_ID) -> Any:
    """The kernel device of `entry`."""
    return device_by_identifier(hass, (DOMAIN, f"kernel_{device_id}"), entry.entry_id)


def canary_device(hass: HomeAssistant, entry: MockConfigEntry, canary_id: str) -> Any:
    """The device of Canary `canary_id` under `entry`."""
    return device_by_identifier(hass, (DOMAIN, f"canary_{canary_id}"), entry.entry_id)


# ── fixtures ────────────────────────────────────────────────────────────


@pytest.fixture(autouse=True)
def _load_from_custom_components(enable_custom_integrations: None) -> None:
    """Every test here loads securacv from custom_components/, like a user."""


@pytest.fixture(autouse=True)
async def _unload_securacv_before_mqtt(hass: HomeAssistant):
    """Unload the integration's entries before the core tears the rest down.

    The ``hass`` fixture unloads every loaded entry at once; the MQTT
    integration torn down beside the entries that still subscribe through
    it leaves its own periodic timer behind, which the plugin's cleanup check
    reports as a leak that is not this integration's. Dependents go first
    here — the order a person's "Remove" button produces — so the check
    keeps watching the integration's own timers and tasks strictly.
    """
    yield
    import asyncio

    from homeassistant.config_entries import ConfigEntryState
    from homeassistant.util.async_ import get_scheduled_timer_handles

    for entry in hass.config_entries.async_entries(DOMAIN):
        if entry.state is ConfigEntryState.LOADED:
            await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done(wait_background_tasks=True)
    for entry in hass.config_entries.async_entries("mqtt"):
        if entry.state is ConfigEntryState.LOADED:
            await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    # The mocked paho client never closes its socket, so the MQTT component's
    # own one-second misc loop (MQTT._async_start_misc_periodic) survives its
    # unload; the core's own test suite excuses it wholesale for component
    # tests. Excused here by name only — any other timer is still a failure.
    for handle in get_scheduled_timer_handles(asyncio.get_running_loop()):
        callback = getattr(handle, "_callback", None)
        if "_async_start_misc_periodic" in getattr(callback, "__qualname__", ""):
            handle.cancel()


@pytest.fixture
def hass_config_dir(hass_tmp_config_dir: str) -> str:
    """A throwaway configuration directory: the evidence packs land in it."""
    return hass_tmp_config_dir


@pytest.fixture
def kernel(aioclient_mock: AiohttpClientMocker) -> KernelMock:
    """A kernel with a fresh signing key, answering on aioclient_mock."""
    return KernelMock(aioclient_mock, SigningKernel())


SetupEntry = Callable[..., Awaitable[MockConfigEntry]]


@pytest.fixture
def setup_kernel_entry(hass: HomeAssistant, kernel: KernelMock) -> SetupEntry:
    """Create a kernel entry pinned to `kernel`'s key, add it, set it up."""

    async def _setup(**kwargs: Any) -> MockConfigEntry:
        kwargs.setdefault("verifying_key", kernel.verifying_key)
        entry = make_kernel_entry(**kwargs)
        entry.add_to_hass(hass)
        assert await hass.config_entries.async_setup(entry.entry_id)
        await settle(hass)
        return entry

    return _setup


__all__ = [
    "AUTH_MODE_PAIRED",
    "AUTH_MODE_STATIC",
    "AUTH_MODE_TOKEN_FILE",
    "CLAIM_NONCE",
    "KERNEL_DEVICE_ID",
    "KERNEL_HOST",
    "KERNEL_URL",
    "KERNEL_VERSION",
    "KernelMock",
    "PAIRED_TOKEN",
    "PAIRED_TOKEN_ID",
    "REPO_ROOT",
    "SEALED_LOG_VECTOR",
    "STATIC_TOKEN",
    "SigningKernel",
    "VECTOR_KEY",
    "VECTOR_NOW",
    "bridge_event",
    "bucket_start",
    "digest_document",
    "export_bundle_bytes",
    "fingerprint_of",
    "kernel_entry_data",
    "make_kernel_entry",
    "make_mqtt_only_entry",
    "settle",
    "status_document",
    "subscribed_topics",
]
