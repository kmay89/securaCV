"""The per-entry runtime state, typed, on ``entry.runtime_data``.

One dataclass replaces the ``hass.data[DOMAIN][entry_id]`` dict the
integration used to build in ``async_setup_entry``. The field names are the
old dict's keys, so every reader moved from ``entry_data["replay"]`` to
``runtime.replay`` and nothing else changed. ``hass.data[DOMAIN]`` keeps
only the domain-level values that outlive any one entry: the watch bucket
and its store, the tick hosts, and the frontend-registration flag.

Home Assistant deletes ``runtime_data`` when an entry unloads, so a message
that lands between an unsubscribe and the entry's own teardown finds no
runtime; ``runtime_for`` is the one accessor every callback uses, and it
answers ``None`` instead of raising.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Callable

from homeassistant.config_entries import ConfigEntry

from .device_trust import TrustStore

if TYPE_CHECKING:
    from .api import SecuraCVApi
    from .coordinator import SecuraCVAdapterStatsCoordinator, SecuraCVKernelCoordinator


@dataclass
class VerificationState:
    """The integration's OWN chain walk against the pinned kernel key.

    Stage b (the sealed-log verifier) fills this in; stage a carries the
    shape so the diagnostics, the chain_valid sensor and the event entities
    read one place. ``ok`` is ``None`` until the first walk. "Verified"
    means this walk checked Ed25519 signatures and the hash chain against
    ``CONF_KERNEL_VERIFYING_KEY`` — never a kernel self-report.
    """

    ok: bool | None = None
    verified_seq: int | None = None
    verified_entries: int = 0
    verified_bucket_start: int | None = None
    head_hash: str | None = None
    error: str | None = None
    # True while /api/identity serves a key other than the pin (Repairs
    # tofu_mismatch is open); events then render as key_mismatch, never as
    # "verification failed".
    key_mismatch: bool = False
    # (event_type, zone_id, time_bucket_start) of every entry the last
    # successful walk covered — the lookup the event rows' badge is decided by.
    covered: set[tuple[str, str, int]] = field(default_factory=set)


@dataclass
class SecuraCVRuntimeData:
    """Everything one config entry holds while loaded."""

    trust_store: TrustStore
    setup_mode: str
    api: SecuraCVApi | None = None
    coordinator: SecuraCVKernelCoordinator | None = None
    adapter_stats_coordinator: SecuraCVAdapterStatsCoordinator | None = None
    # Canary devices seen on the status topic: {device_id: {"status": raw
    # payload, "online": bool | None, "_last_fw": str, "last_event": {...}}}.
    devices: dict[str, dict[str, Any]] = field(default_factory=dict)
    # Unsubscribe callables released by async_unload_entry: the discovery
    # wildcards, the status/health hooks, the watch tick, the claim timer.
    unsub_mqtt: list[Callable[[], None]] = field(default_factory=list)
    # Per-device verify state the sensors read for their trust attributes:
    # {device_id: {"trusted", "reason", "pinned_fingerprint",
    #              "received_fingerprint", "detail"}}.
    verify: dict[str, dict[str, Any]] = field(default_factory=dict)
    # Per-device, per-kind high-water marks of the signed counters (the
    # replay gate); seeded from the trust store's persisted counters.
    replay: dict[str, dict[str, int]] = field(default_factory=dict)
    # (device_id, received fingerprint) pairs already surfaced as a
    # persistent notification, so a stream of mismatches notifies once.
    mismatch_notified: set[tuple[str, str]] = field(default_factory=set)
    # The Canary prefix actually subscribed (None when MQTT is off).
    mqtt_prefix: str | None = None
    # Extension points for stage b: the verifier's state and the evidence
    # exports' bookkeeping (counts only ever reach diagnostics).
    verification: VerificationState = field(default_factory=VerificationState)
    exports: dict[str, Any] = field(default_factory=dict)
    # Set once the retained claim has been published this load.
    claim_published: bool = False


if TYPE_CHECKING:
    SecuraCVConfigEntry = ConfigEntry[SecuraCVRuntimeData]
else:
    # The stub lane's ConfigEntry cannot be subscripted; annotations are
    # strings (``from __future__ import annotations``) so nothing evaluates
    # this at runtime.
    SecuraCVConfigEntry = ConfigEntry


def runtime_for(entry: Any) -> SecuraCVRuntimeData | None:
    """The entry's runtime state, or None once the entry has unloaded."""
    runtime = getattr(entry, "runtime_data", None)
    return runtime if isinstance(runtime, SecuraCVRuntimeData) else None
