"""Device info and the entity bases every platform shares.

Two devices per entry: the kernel (one, "SecuraCV", model "Privacy Witness
Kernel") and the Canaries (one per device id seen on the status topic).
The kernel device carries two identifiers — the durable ``kernel_<device
id>`` and the URL older registries already hold — so an upgrade merges
into the existing device instead of creating a second. Canary devices link
to the kernel as their via-device; the shim below speaks whichever
device-registry API the running core has (``via_device_id`` from 2026.8,
``via_device`` before).

Every DeviceInfo value is a string: a non-string ``sw_version`` is a
deprecation warning today and a refusal from 2026.12.
"""
from __future__ import annotations

from typing import Any, cast

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_URL
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import CONF_KERNEL_DEVICE_ID, DOMAIN, MANUFACTURER, MODEL_CANARY, MODEL_KERNEL
from .coordinator import DATA_IDENTITY, SecuraCVKernelCoordinator

KERNEL_DEVICE_NAME = "SecuraCV"


def kernel_identifier(device_id: str) -> tuple[str, str]:
    """The durable registry identifier of a kernel."""
    return (DOMAIN, f"kernel_{device_id}")


def kernel_identifiers(entry: ConfigEntry) -> set[tuple[str, str]]:
    """Both identifiers when the device id is known; the URL one always."""
    identifiers: set[tuple[str, str]] = set()
    url = entry.data.get(CONF_URL)
    if url:
        identifiers.add((DOMAIN, str(url)))
    device_id = entry.data.get(CONF_KERNEL_DEVICE_ID)
    if device_id:
        identifiers.add(kernel_identifier(str(device_id)))
    return identifiers


def kernel_device_info(entry: ConfigEntry, kernel_version: Any = None) -> DeviceInfo:
    """The kernel device (contract C11): every value a str."""
    info = DeviceInfo(
        identifiers=kernel_identifiers(entry),
        manufacturer=MANUFACTURER,
        model=MODEL_KERNEL,
        name=KERNEL_DEVICE_NAME,
    )
    url = entry.data.get(CONF_URL)
    if url:
        info["configuration_url"] = str(url)
    device_id = entry.data.get(CONF_KERNEL_DEVICE_ID)
    if device_id:
        info["serial_number"] = str(device_id)
    if kernel_version:
        info["sw_version"] = str(kernel_version)
    return info


def kernel_via_device_kwargs(hass: HomeAssistant, entry: ConfigEntry) -> dict[str, Any]:
    """The DeviceInfo kwargs that link a Canary device to this entry's kernel.

    Empty when the entry has no kernel, or when the kernel device is not in
    the registry yet (the link is retried on the Canary's next status).
    Only securacv-owned Canary devices are linked; MQTT-discovery devices
    belong to the mqtt entry and are never edited from here (2026.8's
    single-config-entry rule).
    """
    device_id = entry.data.get(CONF_KERNEL_DEVICE_ID)
    if not device_id:
        return {}
    identifier = kernel_identifier(str(device_id))
    if "via_device_id" in getattr(DeviceInfo, "__annotations__", {}):
        lookup = getattr(dr, "async_get_device_id_by_identifier", None)
        if lookup is None:
            return {}
        try:
            return {"via_device_id": lookup(hass, identifier, config_entry_id=entry.entry_id)}
        except ValueError:
            # The kernel device is not registered yet.
            return {}
    return {"via_device": identifier}


# The longest device name a status payload may set; the broker is untrusted.
DEVICE_NAME_MAX_LENGTH = 64


def canary_display_name(friendly_name: Any, device_id: str) -> str:
    """The Canary's device name: what the owner named it, else the id.

    Always a str (a non-string DeviceInfo value is refused from 2026.12), and
    bounded — the broker is untrusted input.
    """
    if isinstance(friendly_name, str):
        name = " ".join(friendly_name.split())[:DEVICE_NAME_MAX_LENGTH]
        if name:
            return name
    return f"SecuraCV Canary {device_id}"


def canary_name_for(entry: ConfigEntry, device_id: str) -> str:
    """The name the device registry should carry for `device_id`.

    Read from the Canary's last status payload (``device_name`` / ``name``),
    the one source ``__init__`` names the device from. Every entity's
    ``device_info`` goes through ``async_get_or_create`` too, and a name
    there REPLACES the device's name — so an entity that said
    "SecuraCV Canary <id>" wiped the owner's "Kitchen cam" the moment its
    first entity registered. One rule, one name.
    """
    from .data import runtime_for
    from .mqtt_payload import parse_mqtt_json

    runtime = runtime_for(entry)
    device = runtime.devices.get(device_id) if runtime is not None else None
    status = device.get("status") if isinstance(device, dict) else None
    data = status if isinstance(status, dict) else (parse_mqtt_json(status) or {})
    return canary_display_name(data.get("device_name") or data.get("name"), device_id)


def canary_device_info(hass: HomeAssistant, entry: ConfigEntry, device_id: str) -> DeviceInfo:
    """A Canary device, linked to the kernel when the entry has one."""
    info = DeviceInfo(
        identifiers={(DOMAIN, f"canary_{device_id}")},
        manufacturer=MANUFACTURER,
        model=MODEL_CANARY,
        name=canary_name_for(entry, device_id),
    )
    link = kernel_via_device_kwargs(hass, entry)
    if link:
        # Which key exists depends on the running core (the shim above), so
        # the TypedDict cannot name it statically.
        cast(dict[str, Any], info).update(link)
    return info


def canary_status_signal(entry_id: str, device_id: str) -> str:
    """The dispatcher signal sent when a Canary's online state changes."""
    return f"{DOMAIN}_canary_status_{entry_id}_{device_id}"


class SecuraCVKernelEntity(CoordinatorEntity[SecuraCVKernelCoordinator]):
    """Base for every entity on the kernel device.

    Availability is the coordinator's: the bridge's ``witness/status`` says
    online, or the HTTP backstop answered. Names come from translation keys
    only (``has_entity_name``); unique ids are ``securacv_<entry_id>_<key>``.
    """

    _attr_has_entity_name = True

    def __init__(
        self, coordinator: SecuraCVKernelCoordinator, entry: ConfigEntry, key: str
    ) -> None:
        """Bind to the coordinator and the entry; `key` is the unique_id suffix."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        """The kernel device, with the kernel version once identity answered."""
        kernel_version = None
        data = getattr(self.coordinator, "data", None)
        if isinstance(data, dict):
            identity = data.get(DATA_IDENTITY)
            if isinstance(identity, dict):
                kernel_version = identity.get("kernel_version")
        return kernel_device_info(self._entry, kernel_version)

    @property
    def available(self) -> bool:
        """Push availability OR the backstop, not last_update_success alone."""
        return bool(getattr(self.coordinator, "kernel_available", False))
