"""The two kernel actions on a real core: securacv.verify, securacv.export_evidence,
the authenticated evidence view, the media source and the buttons."""
from __future__ import annotations

import json
import stat
import zipfile
from http import HTTPStatus
from pathlib import Path

import pytest
from homeassistant.components import media_source
from homeassistant.components.media_player.errors import BrowseError
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import async_get_persistent_notifications
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator

from conftest import KERNEL_DEVICE_ID, PAIRED_TOKEN, KernelMock, make_mqtt_only_entry
from custom_components.securacv.const import CONF_EVIDENCE_KEEP, DOMAIN

VIEW_PREFIX = "/api/securacv/evidence/"


async def export(hass: HomeAssistant, **data) -> dict:
    """Call securacv.export_evidence and return its response."""
    response = await hass.services.async_call(DOMAIN, "export_evidence", data, blocking=True, return_response=True)
    await hass.async_block_till_done()
    return response


async def test_verify_walks_the_sealed_log_and_answers(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """securacv.verify runs the integration's own walk against the pinned key."""
    for _ in range(3):
        kernel.kernel.seal_event()
    entry = await setup_kernel_entry()

    response = await hass.services.async_call(DOMAIN, "verify", {}, blocking=True, return_response=True)
    assert response == {
        "chain_valid": True,
        "verdict": "chain intact; no new entries since the last walk",
        "events_verified": 0,
        "verified_entries": 3,
        "verified_seq": 3,
        "verified_bucket_start": response["verified_bucket_start"],
        "verified_bucket_size": 600,
        "key_mismatch": False,
        "error": None,
        "kernel_device_id": KERNEL_DEVICE_ID,
    }
    assert response["verified_bucket_start"] % 600 == 0

    # New entries since: the next walk fetches the increment only.
    kernel.kernel.seal_event(zone="garage")
    response = await hass.services.async_call(
        DOMAIN, "verify", {"entry_id": entry.entry_id}, blocking=True, return_response=True
    )
    assert response["chain_valid"] is True
    assert response["events_verified"] == 1
    assert response["verified_entries"] == 4
    assert response["verified_seq"] == 4
    last_call = kernel.calls("/api/sealed-log")[-1]
    assert last_call[1].query.get("from_seq") == "3"

    state = hass.states.get("binary_sensor.securacv_chain_valid")
    assert state.state == "on"
    assert state.attributes["verified_seq"] == 4
    assert state.attributes["last_trigger"] == "service"
    assert hass.states.get("sensor.securacv_chain_length").state == "4"
    verification = hass.states.get("event.securacv_chain_verification")
    assert verification.attributes["event_type"] == "chain_verified"
    assert verification.attributes["trigger"] == "service"
    assert verification.attributes["events_verified"] == 1


async def test_verify_refusals_and_failures_are_translated_errors(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """No kernel → ServiceValidationError; a kernel that cannot be reached → HomeAssistantError."""
    # The actions exist once the component is set up, entry or no entry (action-setup).
    assert await async_setup_component(hass, DOMAIN, {})
    assert hass.services.has_service(DOMAIN, "verify")
    with pytest.raises(ServiceValidationError) as refused:
        await hass.services.async_call(DOMAIN, "verify", {}, blocking=True, return_response=True)
    assert refused.value.translation_key == "no_kernel_entry"
    assert refused.value.translation_domain == DOMAIN

    await setup_kernel_entry()
    with pytest.raises(ServiceValidationError) as refused:
        await hass.services.async_call(DOMAIN, "verify", {"entry_id": "nope"}, blocking=True, return_response=True)
    assert refused.value.translation_key == "entry_not_loaded"

    kernel.connection_down = True
    with pytest.raises(HomeAssistantError) as failed:
        await hass.services.async_call(DOMAIN, "verify", {}, blocking=True, return_response=True)
    assert not isinstance(failed.value, ServiceValidationError)
    assert failed.value.translation_key == "kernel_unreachable"

    kernel.connection_down = False
    kernel.sealed_log_status = 401
    with pytest.raises(HomeAssistantError) as failed:
        await hass.services.async_call(DOMAIN, "verify", {}, blocking=True, return_response=True)
    assert failed.value.translation_key == "kernel_auth_failed"


async def test_export_evidence_writes_one_private_zip_and_notifies(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """C7: the pack, its members, the index, the modes and the signed link."""
    for _ in range(2):
        kernel.kernel.seal_event()
    entry = await setup_kernel_entry(auth_mode="paired")

    response = await export(hass, window_hours=48)
    assert set(response) == {"export_id", "file", "url", "sha256", "events", "window_hours", "kernel_device_id"}
    assert response["window_hours"] == 48
    assert response["events"] == 2
    assert response["kernel_device_id"] == KERNEL_DEVICE_ID
    assert response["url"].startswith(f"{VIEW_PREFIX}{response['export_id']}?authSig=")
    assert response["file"].startswith("securacv-evidence-") and response["file"].endswith(f"-{response['export_id']}.zip")

    directory = Path(hass.config.path("securacv", "evidence"))
    assert stat.S_IMODE(directory.stat().st_mode) == 0o700
    pack = directory / response["file"]
    assert stat.S_IMODE(pack.stat().st_mode) == 0o600
    with zipfile.ZipFile(pack) as zf:
        names = set(zf.namelist())
        assert names == {"securacv-events-1700000400.json", "evidence_viewer.html", "README.txt", "WITNESS_KEY.txt"}
        assert zf.read("securacv-events-1700000400.json") == kernel.bundle
        key_text = zf.read("WITNESS_KEY.txt").decode()
        assert key_text == f"verifying_key={kernel.verifying_key}\nfingerprint={kernel.kernel.fingerprint}\nkernel={KERNEL_DEVICE_ID}\n"
        viewer = Path(__file__).resolve().parents[2] / "custom_components" / "securacv" / "www" / "securacv-evidence-viewer.html"
        assert zf.read("evidence_viewer.html") == viewer.read_bytes()
        assert zf.read("README.txt").startswith(b"") and len(zf.read("README.txt")) > 100
    assert "/www/" not in str(directory) and "www" not in directory.parts

    index = json.loads((directory / "index.json").read_text())
    assert index["v"] == 1
    assert [row["export_id"] for row in index["exports"]] == [response["export_id"]]
    assert index["exports"][0]["sha256"] == response["sha256"]
    assert index["exports"][0]["created_bucket_start"] % 600 == 0

    # The kernel was asked for the window in hours with the paired bearer (one receipted read).
    bundle_calls = kernel.calls("/export/bundle")
    assert len(bundle_calls) == 1
    assert bundle_calls[0][1].query.get("last") == "48h"
    assert kernel.auth_headers("/export/bundle") == [f"Bearer {PAIRED_TOKEN}"]

    notifications = async_get_persistent_notifications(hass)
    note = notifications[f"securacv_export_{entry.entry_id}"]
    assert note["title"] == "SecuraCV evidence export ready"
    assert "last 48 hours, 2 sealed events, chain verified ✓" in note["message"]
    assert f"[Download the evidence pack]({response['url']})" in note["message"]
    assert f"`{kernel.kernel.fingerprint}`" in note["message"]
    assert "Removed" not in note["message"]


async def test_two_concurrent_exports_both_land_in_the_index(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """index.json is read-modified-written per export; a button press beside an
    automation must not drop either row. The real executor runs both."""
    import asyncio

    kernel.kernel.seal_event()
    await setup_kernel_entry(auth_mode="paired")

    first, second = await asyncio.gather(
        hass.services.async_call(DOMAIN, "export_evidence", {"window_hours": 24}, blocking=True, return_response=True),
        hass.services.async_call(DOMAIN, "export_evidence", {"window_hours": 48}, blocking=True, return_response=True),
    )
    await hass.async_block_till_done()
    assert first["export_id"] != second["export_id"]

    directory = Path(hass.config.path("securacv", "evidence"))
    index = json.loads((directory / "index.json").read_text())
    assert {row["export_id"] for row in index["exports"]} == {first["export_id"], second["export_id"]}
    assert sorted(row["file"] for row in index["exports"]) == sorted(p.name for p in directory.glob("*.zip"))
    assert len(kernel.calls("/export/bundle")) == 2


async def test_export_window_is_validated_and_keep_n_prunes(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """window_hours outside 1–720 is a refusal; evidence_keep bounds the packs."""
    entry = await setup_kernel_entry(options={CONF_EVIDENCE_KEEP: 1})
    # The schema refuses 0 before the handler (a vol.Invalid on a real core).
    with pytest.raises(Exception) as refused:
        await export(hass, window_hours=0)
    assert "window_hours" in str(refused.value) or getattr(refused.value, "translation_key", "") == "window_out_of_range"

    first = await export(hass)
    second = await export(hass)
    directory = Path(hass.config.path("securacv", "evidence"))
    zips = sorted(p.name for p in directory.glob("*.zip"))
    assert zips == [second["file"]]
    index = json.loads((directory / "index.json").read_text())
    assert [row["export_id"] for row in index["exports"]] == [second["export_id"]]
    note = async_get_persistent_notifications(hass)[f"securacv_export_{entry.entry_id}"]
    assert note["message"].endswith("Removed 1 older export(s) to stay within your limit of 1.")
    assert first["file"] not in zips


async def test_the_evidence_view_requires_auth_and_serves_the_signed_link(
    hass: HomeAssistant,
    kernel: KernelMock,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
    hass_client_no_auth: ClientSessionGenerator,
) -> None:
    """Nothing under /local: the pack is served by an authenticated view; the
    notification's link is a 24-hour signed path."""
    assert await async_setup_component(hass, "http", {})
    await setup_kernel_entry()
    response = await export(hass)

    anonymous = await hass_client_no_auth()
    refused = await anonymous.get(f"{VIEW_PREFIX}{response['export_id']}")
    assert refused.status == HTTPStatus.UNAUTHORIZED

    signed = await anonymous.get(response["url"])
    assert signed.status == HTTPStatus.OK
    assert signed.headers["Content-Type"] == "application/zip"
    assert signed.headers["Content-Disposition"] == f'attachment; filename="{response["file"]}"'
    body = await signed.read()
    assert body == (Path(hass.config.path("securacv", "evidence")) / response["file"]).read_bytes()

    authenticated = await hass_client()
    ok = await authenticated.get(f"{VIEW_PREFIX}{response['export_id']}")
    assert ok.status == HTTPStatus.OK
    for bad in ("0badf00e", "0BADF00D", "x" * 8):
        missing = await authenticated.get(f"{VIEW_PREFIX}{bad}")
        assert missing.status == HTTPStatus.NOT_FOUND, bad
    traversal = await authenticated.get(f"{VIEW_PREFIX}..%2Findex.json")
    assert traversal.status in (HTTPStatus.NOT_FOUND, HTTPStatus.BAD_REQUEST)


async def test_media_source_lists_and_resolves_the_packs(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """Media → SecuraCV evidence: one folder per entry, packs resolving to a fresh signed link."""
    assert await async_setup_component(hass, "media_source", {})
    entry = await setup_kernel_entry()
    response = await export(hass)

    root = await media_source.async_browse_media(hass, f"media-source://{DOMAIN}")
    assert root.title == "SecuraCV evidence"
    assert root.can_expand and not root.can_play
    assert [child.identifier for child in root.children] == [entry.entry_id]
    assert root.children[0].title == "SecuraCV evidence (SecuraCV)"

    folder = await media_source.async_browse_media(hass, f"media-source://{DOMAIN}/{entry.entry_id}")
    assert len(folder.children) == 1
    pack = folder.children[0]
    assert pack.identifier == f"{entry.entry_id}/{response['export_id']}"
    assert pack.media_content_type == "application/zip"
    assert pack.can_play and not pack.can_expand
    assert pack.title.startswith(response["file"])

    play = await media_source.async_resolve_media(hass, f"media-source://{DOMAIN}/{pack.identifier}", None)
    assert play.mime_type == "application/zip"
    assert play.url.startswith(f"{VIEW_PREFIX}{response['export_id']}?authSig=")

    with pytest.raises(media_source.Unresolvable):
        await media_source.async_resolve_media(hass, f"media-source://{DOMAIN}/{entry.entry_id}", None)
    with pytest.raises(BrowseError):
        await media_source.async_browse_media(hass, f"media-source://{DOMAIN}/{entry.entry_id}/deadbeef")


async def test_export_failures_name_their_cause(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """Kernel down / credential rejected / not a kernel entry."""
    await setup_kernel_entry()
    kernel.bundle_status = 401
    with pytest.raises(HomeAssistantError) as failed:
        await export(hass)
    assert failed.value.translation_key == "kernel_auth_failed"
    kernel.bundle_status = 200
    kernel.connection_down = True
    with pytest.raises(HomeAssistantError) as failed:
        await export(hass)
    assert failed.value.translation_key == "kernel_unreachable"
    directory = Path(hass.config.path("securacv", "evidence"))
    assert not directory.exists() or not list(directory.glob("*.zip")), "a failed export leaves no pack"


async def test_two_kernel_entries_need_an_entry_id(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """With two kernels the action refuses to guess."""
    from conftest import SigningKernel

    await setup_kernel_entry()
    other = KernelMock(kernel.mock, SigningKernel(device_id="pwk_feedbeef"), url="http://192.168.1.99:8799")
    await setup_kernel_entry(device_id="pwk_feedbeef", url=other.url, verifying_key=other.verifying_key)
    with pytest.raises(ServiceValidationError) as refused:
        await hass.services.async_call(DOMAIN, "verify", {}, blocking=True, return_response=True)
    assert refused.value.translation_key == "kernel_entry_ambiguous"


async def test_the_buttons_run_the_actions(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """Verify now and Export evidence press through to the same coroutines."""
    kernel.kernel.seal_event()
    await setup_kernel_entry()
    await hass.services.async_call("button", "press", {"entity_id": "button.securacv_verify_now"}, blocking=True)
    await hass.async_block_till_done()
    verification = hass.states.get("event.securacv_chain_verification")
    assert verification.attributes["event_type"] == "chain_verified"
    assert verification.attributes["trigger"] == "button"

    await hass.services.async_call("button", "press", {"entity_id": "button.securacv_export_evidence"}, blocking=True)
    await hass.async_block_till_done()
    zips = list(Path(hass.config.path("securacv", "evidence")).glob("*.zip"))
    assert len(zips) == 1
    assert kernel.calls("/export/bundle")[0][1].query.get("last") == "24h"


async def test_the_watch_actions_still_register_beside_the_kernel_ones(hass: HomeAssistant, kernel: KernelMock) -> None:
    """Six actions exist whether or not an entry is loaded (action-setup)."""
    entry = make_mqtt_only_entry()
    entry.add_to_hass(hass)
    await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    for service in ("start_watch", "end_watch", "list_watches", "verify", "export_evidence", "staged_update"):
        assert hass.services.has_service(DOMAIN, service), service
