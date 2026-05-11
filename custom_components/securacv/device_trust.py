"""Per-device pubkey trust store for SecuraCV.

Backs the PKI side of the MQTT pipeline: every Canary signs the chain,
events, and counts MQTT publishes with its on-device Ed25519 privkey
(firmware: `device_signature.cpp`); HA pins the matching pubkey and
verifies every publish before letting it touch entity state.

Trust model
-----------
- **TOFU by default.** First time a device_id appears on MQTT with a
  valid `fp` field, we pin that fingerprint as the trusted identity.
  Subsequent publishes from the same device_id MUST carry the same fp.
- **Manual pin.** The config flow's "Pin device fingerprint" step lets
  an installer enter the fp they read off the device's `/enroll` page;
  that pin takes precedence over any TOFU pin already on record.
- **Rotation.** An explicit options-flow action lets the operator
  re-pin a new fingerprint after a deliberate key change (e.g. NVS
  wipe). Rotation is logged with the old fp + a timestamp so an
  audit trail survives.
- **Mismatch policy.** Per PR design (chosen via AskUserQuestion):
  warn loudly, still accept the publish, mark the device as
  unverified. The persistent_notification is fired by the caller in
  `__init__.py`; this module only computes the verdict.

Persistence
-----------
Pins live in HA's `Store` helper under
`securacv_device_trust_{entry_id}` so the registry survives integration
reload + HA restart. Storage shape:

    {
      "version": 1,
      "devices": {
        "<device_id>": {
          "pubkey_hex": "<64-char hex>",
          "fingerprint_hex": "<16-char hex>",
          "pinned_at": <epoch_seconds>,
          "pin_source": "tofu" | "manual" | "rotation",
          "previous": [
            {"pubkey_hex": "...", "fp": "...", "retired_at": <ts>}
          ]
        },
        ...
      }
    }
"""

from __future__ import annotations

import hashlib
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Optional

from homeassistant.core import HomeAssistant
from homeassistant.helpers.storage import Store

_LOGGER = logging.getLogger(__name__)

STORAGE_VERSION = 1
STORAGE_KEY_FMT = "securacv_device_trust_{entry_id}"

PIN_SOURCE_TOFU = "tofu"
PIN_SOURCE_MANUAL = "manual"
PIN_SOURCE_ROTATION = "rotation"


@dataclass(frozen=True)
class TrustVerdict:
    """The outcome of a verify_publish call.

    `trusted=True` means "the signature checks out and the fingerprint
    matches the pinned one (or just got pinned via TOFU)". `trusted=False`
    with `reason='mismatch'` is the case the caller surfaces as a
    persistent_notification.

    All fields are JSON-serializable so callers can stamp them onto
    entity extra_state_attributes for the dashboard.
    """

    trusted: bool
    reason: str               # "ok" | "tofu_pin" | "mismatch" | "unsigned" | "no_pubkey"
    pinned_fingerprint: Optional[str] = None
    received_fingerprint: Optional[str] = None
    detail: str = ""


@dataclass
class DeviceTrustEntry:
    """One pinned device. `previous` is the audit trail of rotations.

    Kept as a dataclass (not a TypedDict) so HA's repair flow can stamp
    `.pinned_at` directly without re-deserializing the whole store.
    """

    pubkey_hex: str
    fingerprint_hex: str
    pinned_at: float
    pin_source: str
    previous: list[dict[str, Any]] = field(default_factory=list)

    @classmethod
    def from_storage(cls, data: dict[str, Any]) -> "DeviceTrustEntry":
        return cls(
            pubkey_hex=data["pubkey_hex"],
            fingerprint_hex=data["fingerprint_hex"],
            pinned_at=float(data.get("pinned_at", 0.0)),
            pin_source=data.get("pin_source", PIN_SOURCE_TOFU),
            previous=list(data.get("previous", [])),
        )

    def to_storage(self) -> dict[str, Any]:
        return {
            "pubkey_hex": self.pubkey_hex,
            "fingerprint_hex": self.fingerprint_hex,
            "pinned_at": self.pinned_at,
            "pin_source": self.pin_source,
            "previous": self.previous,
        }


def fingerprint_from_pubkey_hex(pubkey_hex: str) -> str:
    """Mirror firmware's domain-separated SHA256 fingerprint.

    `compute_fingerprint` in canary_wap.ino hashes the 32-byte pubkey
    with domain string "securacv:pubkey:fingerprint" and takes the
    first 8 bytes hex-encoded. We replicate it byte-for-byte so HA
    can derive the fp from a pubkey supplied via the /enroll endpoint
    or manual paste without needing the firmware to also publish it.
    """
    domain = b"securacv:pubkey:fingerprint"
    pubkey = bytes.fromhex(pubkey_hex)
    if len(pubkey) != 32:
        raise ValueError(f"Expected 32-byte pubkey, got {len(pubkey)} bytes")
    h = hashlib.sha256()
    h.update(domain)
    h.update(pubkey)
    return h.digest()[:8].hex()


class TrustStore:
    """Async-aware persistent pin registry.

    Construction is cheap and synchronous; the heavy lifting happens
    in `async_load` (reads HA's storage) and `async_save` (writes it).
    Callers should `await async_load()` once during config_entry setup
    and pass the populated instance into the MQTT handler closures.
    """

    def __init__(self, hass: HomeAssistant, entry_id: str) -> None:
        self._hass = hass
        self._entry_id = entry_id
        self._store: Store = Store(
            hass,
            STORAGE_VERSION,
            STORAGE_KEY_FMT.format(entry_id=entry_id),
        )
        self._devices: dict[str, DeviceTrustEntry] = {}
        self._loaded = False

    async def async_load(self) -> None:
        raw = await self._store.async_load()
        if not raw:
            self._loaded = True
            return
        for device_id, payload in raw.get("devices", {}).items():
            try:
                self._devices[device_id] = DeviceTrustEntry.from_storage(payload)
            except (KeyError, ValueError) as err:
                _LOGGER.warning(
                    "Dropping malformed trust entry for %s: %s", device_id, err
                )
        self._loaded = True

    async def async_save(self) -> None:
        await self._store.async_save(
            {
                "version": STORAGE_VERSION,
                "devices": {
                    device_id: entry.to_storage()
                    for device_id, entry in self._devices.items()
                },
            }
        )

    # ─── Public API ────────────────────────────────────────────────────

    def is_pinned(self, device_id: str) -> bool:
        return device_id in self._devices

    def get(self, device_id: str) -> Optional[DeviceTrustEntry]:
        return self._devices.get(device_id)

    def get_pubkey_bytes(self, device_id: str) -> Optional[bytes]:
        entry = self._devices.get(device_id)
        if not entry:
            return None
        try:
            return bytes.fromhex(entry.pubkey_hex)
        except ValueError:
            _LOGGER.error(
                "Stored pubkey for %s isn't valid hex; dropping pin", device_id
            )
            return None

    async def async_pin(
        self,
        device_id: str,
        pubkey_hex: str,
        source: str = PIN_SOURCE_MANUAL,
    ) -> DeviceTrustEntry:
        """Pin (or replace) a device's identity.

        Replacing an existing pin moves the old (pubkey_hex, fp) into
        the `previous` audit trail with a retired_at timestamp. Callers
        should generally route through `async_tofu_pin_if_unknown` or
        `async_rotate` rather than calling this directly — those wrap
        the right pin_source. Direct calls are for the config flow's
        manual entry step.
        """
        fp = fingerprint_from_pubkey_hex(pubkey_hex)
        now = time.time()
        existing = self._devices.get(device_id)
        previous: list[dict[str, Any]] = []
        if existing:
            previous = list(existing.previous)
            previous.append(
                {
                    "pubkey_hex": existing.pubkey_hex,
                    "fp": existing.fingerprint_hex,
                    "retired_at": now,
                }
            )
        new_entry = DeviceTrustEntry(
            pubkey_hex=pubkey_hex,
            fingerprint_hex=fp,
            pinned_at=now,
            pin_source=source,
            previous=previous,
        )
        self._devices[device_id] = new_entry
        await self.async_save()
        _LOGGER.info(
            "Pinned device %s with fp=%s (source=%s)", device_id, fp, source
        )
        return new_entry

    async def async_tofu_pin_if_unknown(
        self, device_id: str, pubkey_hex: str
    ) -> Optional[DeviceTrustEntry]:
        """Pin on first sight; return None if already pinned."""
        if device_id in self._devices:
            return None
        return await self.async_pin(device_id, pubkey_hex, source=PIN_SOURCE_TOFU)

    async def async_rotate(
        self, device_id: str, new_pubkey_hex: str
    ) -> DeviceTrustEntry:
        """Operator-confirmed key rotation. Caller is responsible for
        gating this behind an explicit UI confirmation (the options
        flow does this)."""
        return await self.async_pin(
            device_id, new_pubkey_hex, source=PIN_SOURCE_ROTATION
        )

    async def async_unpin(self, device_id: str) -> bool:
        if device_id not in self._devices:
            return False
        del self._devices[device_id]
        await self.async_save()
        return True

    def all_devices(self) -> dict[str, DeviceTrustEntry]:
        """Returns a *copy* of the device registry so callers can
        iterate without worrying about concurrent mutation from
        pin/rotate paths."""
        return dict(self._devices)
