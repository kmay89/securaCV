"""The kernel device's two chain entities, held to the words the card, the
dashboard strategy and the digest blueprint read.

  - chain_valid: `on` / `off` is the integration's own walk; a key change is
    UNKNOWN with `key_mismatch: true`, `trust_reason: kernel_key_mismatch`
    and an `error` naming Repairs — never `off` (which every consumer
    renders as "Verification failed"); the error clears with the mismatch;
  - chain_length: the walk head `seq` once a walk has run, else the
    kernel's own row count, with `source` saying which, and NO state class
    (a `total_increasing` class would turn the switch between the two
    measures, and retention pruning, into statistics resets).
"""

from __future__ import annotations

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, make_runtime

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import binary_sensor as bs_platform  # noqa: E402
from .. import sensor as sensor_platform  # noqa: E402
from ..const import (  # noqa: E402
    CONF_KERNEL_VERIFYING_KEY,
    TRUST_REASON_HA_VERIFIED_CHAIN,
    TRUST_REASON_HA_VERIFY_FAILED,
    TRUST_REASON_KERNEL_KEY_MISMATCH,
    VERIFY_ERROR_KEY_CHANGED,
)
from ..coordinator import SecuraCVKernelCoordinator  # noqa: E402
from ..data import VerificationState  # noqa: E402

KEY = "ab" * 32
OTHER_KEY = "cd" * 32


class FakeApi:
    base_url = "http://kernel:8799"
    token_file_unreadable = False

    def __init__(self, chain_length=1200):
        self.identity = {"device_id": "pwk_1a2b3c4d", "verifying_key": KEY, "kernel_version": "2.4.16",
                         "chain_length": chain_length, "latest_hash": "9c" * 32}

    async def async_get_health(self):
        return {"ok": True}

    async def async_get_identity(self):
        return dict(self.identity)

    async def async_get_status(self):
        return None

    async def async_get_latest_event(self):
        return None

    async def async_get_digest(self):
        return None


def _kernel(verification: VerificationState | None = None, *, chain_length=1200):
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass, data={CONF_KERNEL_VERIFYING_KEY: KEY})
    api = FakeApi(chain_length)
    coordinator = SecuraCVKernelCoordinator(
        hass, entry, api, auth_mode="paired", topic_prefix="witness", pinned_key=KEY
    )
    coordinator.data = dict(coordinator.data, identity=dict(api.identity))
    entry.runtime_data = make_runtime(hass, coordinator=coordinator, api=api)
    if verification is not None:
        entry.runtime_data.verification = verification
    return hass, entry, coordinator


# ─── chain_valid ───────────────────────────────────────────────────────


def test_chain_valid_follows_the_walk_and_speaks_the_rows_vocabulary():
    _hass, entry, coordinator = _kernel(VerificationState())
    sensor = bs_platform.SecuraCVKernelChainValidSensor(coordinator, entry)
    assert sensor.is_on is None
    attrs = sensor.extra_state_attributes
    assert attrs["trust_reason"] is None and attrs["key_mismatch"] is False
    assert attrs["error"] is None and attrs["verifying"] is False

    entry.runtime_data.verification = VerificationState(
        ok=True, verified_seq=1204, verified_entries=1204, verified_bucket_start=1700001200, head_seq=1204,
    )
    assert sensor.is_on is True
    attrs = sensor.extra_state_attributes
    assert attrs["trust_reason"] == TRUST_REASON_HA_VERIFIED_CHAIN
    assert attrs["verified_seq"] == 1204 and attrs["failed_seq"] is None

    entry.runtime_data.verification = VerificationState(
        ok=False, error="sealed_events hash chain broken at index 7", failed_seq=1190,
    )
    assert sensor.is_on is False
    attrs = sensor.extra_state_attributes
    assert attrs["trust_reason"] == TRUST_REASON_HA_VERIFY_FAILED
    assert attrs["failed_seq"] == 1190 and "broken" in attrs["error"]


def test_a_key_change_is_unknown_with_the_repairs_words_never_off():
    """The strategy header and the digest blueprint render `off` as
    'Verification failed'; a key change must never read that way (D10)."""
    _hass, entry, coordinator = _kernel(VerificationState(ok=True, verified_seq=3, head_seq=3))
    sensor = bs_platform.SecuraCVKernelChainValidSensor(coordinator, entry)
    assert sensor.is_on is True

    coordinator.async_note_served_key(OTHER_KEY)
    assert sensor.is_on is None, "no verdict against the pin while the served key differs"
    attrs = sensor.extra_state_attributes
    assert attrs["key_mismatch"] is True
    assert attrs["trust_reason"] == TRUST_REASON_KERNEL_KEY_MISMATCH
    assert attrs["error"] == VERIFY_ERROR_KEY_CHANGED
    assert "Repairs" in attrs["error"] and "fail" not in attrs["error"].lower()
    # What the walk had established still shows; nothing is rewritten as a failure.
    assert attrs["verified_seq"] == 3 and attrs["failed_seq"] is None

    # The kernel serves the pinned key again: the verdict and the words return.
    coordinator.async_note_served_key(KEY)
    assert sensor.is_on is True
    attrs = sensor.extra_state_attributes
    assert attrs["key_mismatch"] is False and attrs["error"] is None
    assert attrs["trust_reason"] == TRUST_REASON_HA_VERIFIED_CHAIN


def test_a_real_walk_error_survives_a_key_change_clearing():
    """Only the key-change message is cleared with the mismatch; a walk
    failure recorded before it stays until a walk passes."""
    _hass, entry, coordinator = _kernel(VerificationState(ok=False, error="prev_hash mismatch at index 2", failed_seq=2))
    sensor = bs_platform.SecuraCVKernelChainValidSensor(coordinator, entry)
    coordinator.async_note_served_key(OTHER_KEY)
    assert sensor.is_on is None and sensor.extra_state_attributes["error"] == VERIFY_ERROR_KEY_CHANGED
    coordinator.async_note_served_key(KEY)
    assert sensor.is_on is False
    assert sensor.extra_state_attributes["error"] is None, "the key note was the last error; the failed walk is re-run, not re-stated"
    assert sensor.extra_state_attributes["trust_reason"] == TRUST_REASON_HA_VERIFY_FAILED


# ─── chain_length ──────────────────────────────────────────────────────


def test_chain_length_is_the_kernel_count_until_a_walk_then_the_head_seq():
    _hass, entry, coordinator = _kernel(VerificationState(), chain_length=1200)
    sensor = sensor_platform.SecuraCVKernelChainLengthSensor(coordinator, entry)
    assert sensor.native_value == 1200
    attrs = sensor.extra_state_attributes
    assert attrs["source"] == "kernel_identity" and attrs["head_seq"] is None
    assert attrs["kernel_chain_length"] == 1200
    assert attrs["latest_hash"] == "9c" * 32 and attrs["pinned"] is True

    # After retention pruned 3,000 rows: COUNT(*) 1,200, head seq 4,200. The
    # walk's head wins and the attribute says so; the count stays readable.
    entry.runtime_data.verification = VerificationState(ok=True, head_seq=4200, head_hash="ee" * 32, verified_seq=4200)
    assert sensor.native_value == 4200
    attrs = sensor.extra_state_attributes
    assert attrs["source"] == "walk_head" and attrs["head_seq"] == 4200
    assert attrs["kernel_chain_length"] == 1200
    assert attrs["latest_hash"] == "ee" * 32 and attrs["verified_seq"] == 4200


def test_chain_length_has_no_statistics_class():
    """Pruning shrinks the kernel's count and the first walk switches the
    measure to the head seq: either reads as a meter reset under
    total_increasing, so the sensor declares no state class at all."""
    assert getattr(sensor_platform.SecuraCVKernelChainLengthSensor, "_attr_state_class", None) is None
    _hass, entry, coordinator = _kernel(VerificationState())
    sensor = sensor_platform.SecuraCVKernelChainLengthSensor(coordinator, entry)
    assert getattr(sensor, "_attr_state_class", None) is None
    assert sensor._attr_native_unit_of_measurement == "blocks"
