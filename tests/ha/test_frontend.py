"""The Lovelace cards and the dashboard strategy on a real http component."""
from __future__ import annotations

import hashlib
from http import HTTPStatus
from unittest.mock import patch

from homeassistant.core import HomeAssistant
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import async_get_persistent_notifications
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator

from conftest import REPO_ROOT, KernelMock
from custom_components.securacv import LOVELACE_CARD_FILENAMES
from custom_components.securacv.const import CONF_DASHBOARD_NOTICE_SHOWN, DOMAIN

WWW = REPO_ROOT / "custom_components" / "securacv" / "www"


async def test_cards_and_strategy_are_served_and_auto_loaded(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry, hass_client_no_auth: ClientSessionGenerator
) -> None:
    """Static paths for every card file, add_extra_js_url keyed by the file's bytes."""
    assert await async_setup_component(hass, "http", {})
    with patch("homeassistant.components.frontend.add_extra_js_url") as add_js:
        entry = await setup_kernel_entry()

    expected = []
    for filename in LOVELACE_CARD_FILENAMES:
        digest = hashlib.sha256((WWW / filename).read_bytes()).hexdigest()[:12]
        expected.append(f"/{DOMAIN}_www/{filename}?v={digest}")
    assert [call.args[1] for call in add_js.call_args_list] == expected
    assert "securacv-strategy.js" in LOVELACE_CARD_FILENAMES

    client = await hass_client_no_auth()
    for filename in LOVELACE_CARD_FILENAMES:
        resp = await client.get(f"/{DOMAIN}_www/{filename}")
        assert resp.status == HTTPStatus.OK, filename
        assert await resp.read() == (WWW / filename).read_bytes()
    # Nothing else under www/ is served (the viewer and README are zip members, not cards).
    resp = await client.get(f"/{DOMAIN}_www/securacv-evidence-viewer.html")
    assert resp.status == HTTPStatus.NOT_FOUND

    # J1: the one-time pointer to the dashboard, only because the strategy was served.
    notifications = async_get_persistent_notifications(hass)
    note = notifications[f"securacv_dashboard_{entry.entry_id}"]
    assert "Settings → Dashboards → Add dashboard → SecuraCV" in note["message"]
    assert entry.data[CONF_DASHBOARD_NOTICE_SHOWN] is True

    # A reload posts it no second time; removal dismisses it.
    assert await hass.config_entries.async_reload(entry.entry_id)
    await hass.async_block_till_done()
    assert len([k for k in async_get_persistent_notifications(hass) if k.startswith("securacv_dashboard_")]) == 1
    assert await hass.config_entries.async_remove(entry.entry_id)
    await hass.async_block_till_done()
    assert f"securacv_dashboard_{entry.entry_id}" not in async_get_persistent_notifications(hass)


async def test_no_http_means_no_cards_and_no_dashboard_notice(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """Headless: the frontend is optional, setup never fails on it and the notice is honest."""
    entry = await setup_kernel_entry()
    assert entry.state.value == "loaded"
    assert f"securacv_dashboard_{entry.entry_id}" not in async_get_persistent_notifications(hass)
    assert CONF_DASHBOARD_NOTICE_SHOWN not in entry.data
    assert hass.data[DOMAIN].get("_frontend_registered") is False
