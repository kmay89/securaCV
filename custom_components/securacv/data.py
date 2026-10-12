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
from typing import TYPE_CHECKING, Any, Callable, TypeAlias

from homeassistant.config_entries import ConfigEntry

from .device_trust import TrustStore

if TYPE_CHECKING:
    from .api import SecuraCVApi
    from .coordinator import SecuraCVAdapterStatsCoordinator, SecuraCVKernelCoordinator
    from .kernel_verify import KernelVerifier


@dataclass
class VerificationState:
    """The integration's OWN chain walk against the pinned kernel key.

    Filled by ``kernel_verify.KernelVerifier``; the diagnostics, the
    chain_valid and chain_length sensors, the verification event entity and
    every kernel event row read this one place. ``ok`` is ``None`` until the
    first walk. "Verified" means this walk checked Ed25519 signatures and
    the hash chain against ``CONF_KERNEL_VERIFYING_KEY`` — never a kernel
    self-report (that is ``kernel_reported_chain_ok``, an attribute).
    """

    ok: bool | None = None
    # The cursor: the newest sealed-log row the walk verified and its hash.
    # Persisted (kernel_verify's Store) so the next fetch is incremental.
    verified_seq: int | None = None
    # How many entries the walks have verified since the cursor was anchored.
    verified_entries: int = 0
    # The bucket the last successful walk ran in (coarse time).
    verified_bucket_start: int | None = None
    verified_bucket_size: int = 600
    head_hash: str | None = None
    # The served document's head (`head.seq`), the chain_length state.
    head_seq: int | None = None
    error: str | None = None
    # Which entry the last failed walk stopped at, when it did.
    failed_seq: int | None = None
    # True while /api/identity serves a key other than the pin (Repairs
    # tofu_mismatch is open); events then render as key_mismatch, never as
    # "verification failed".
    key_mismatch: bool = False
    # A walk is in flight (the header shows "Verifying…").
    verifying: bool = False
    # What started the last walk (button / service / scheduled / idle).
    last_trigger: str | None = None
    # How many entries the LAST walk verified (the increment).
    walk_entries: int = 0
    # Set when the kernel re-anchored the served tail past the cursor (a
    # checkpoint or the size cap): the entries verified, continuity across
    # the gap is the kernel's claim, not this walk's proof.
    anchor_moved: bool = False
    # (event_type, zone_id, time_bucket_start) of every entry the walks have
    # covered, bounded by retention — the lookup a row WITHOUT a `seq` falls
    # back to (kernel_verify.event_verification states the rule).
    covered: set[tuple[str, str, int]] = field(default_factory=set)
    # The same entries by their sealed-log `seq`: seq → the entry's key. A
    # row that carries `seq` (contract C5) is Verified by this index alone,
    # so two entries sharing one key in one bucket (a dog and a cat in one
    # zone) never let the second read Verified on the first's signature.
    covered_seqs: dict[int, tuple[str, str, int]] = field(default_factory=dict)


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
    # The chain verifier (kernel entries only) and its state; the evidence
    # exports' bookkeeping (`{"exports": [index rows]}`; counts only ever
    # reach diagnostics).
    verifier: KernelVerifier | None = None
    verification: VerificationState = field(default_factory=VerificationState)
    exports: dict[str, Any] = field(default_factory=dict)
    # Set once the retained claim has been published this load.
    claim_published: bool = False


if TYPE_CHECKING:
    # Annotated as an alias on purpose: in the stub lane (no core installed)
    # ConfigEntry resolves to Any, and a bare assignment of Any is a variable
    # to mypy, not a type - every `entry: SecuraCVConfigEntry` then fails.
    SecuraCVConfigEntry: TypeAlias = ConfigEntry[SecuraCVRuntimeData]
else:
    # The stub lane's ConfigEntry cannot be subscripted; annotations are
    # strings (``from __future__ import annotations``) so nothing evaluates
    # this at runtime.
    SecuraCVConfigEntry = ConfigEntry


def runtime_for(entry: Any) -> SecuraCVRuntimeData | None:
    """The entry's runtime state, or None once the entry has unloaded."""
    runtime = getattr(entry, "runtime_data", None)
    return runtime if isinstance(runtime, SecuraCVRuntimeData) else None
