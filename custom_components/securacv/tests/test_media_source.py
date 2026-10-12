"""Media → SecuraCV evidence: the packs, per entry, resolving to signed links.

The root lists one folder per entry, each folder its packs newest first;
resolving a pack signs the authenticated view's path for 24 hours at resolve
time. Nothing else is browsable, and no path escapes the eight-hex id rule.
"""

from __future__ import annotations

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, make_runtime, run, signed_paths

from homeassistant.components.media_player import BrowseError  # noqa: E402
from homeassistant.components.media_source import MediaSourceItem, Unresolvable  # noqa: E402
from homeassistant.core import HomeAssistant  # noqa: E402

from .. import evidence, media_source  # noqa: E402
from ..const import DOMAIN  # noqa: E402


def _records():
    return [
        evidence.ExportRecord("00000002", "securacv-evidence-2026-10-12-00000002.zip", 1700001000, 24,
                              "pwk_1a2b3c4d", "ab" * 32, 100, 3, "e1"),
        evidence.ExportRecord("00000001", "securacv-evidence-2026-10-11-00000001.zip", 1700000400, 168,
                              "pwk_1a2b3c4d", "cd" * 32, 100, 9, "e1"),
        evidence.ExportRecord("0000000a", "securacv-evidence-2026-10-12-0000000a.zip", 1700000400, 24,
                              "pwk_ffffffff", "ef" * 32, 100, 1, "e2"),
    ]


def _hass():
    hass = HomeAssistant()
    hass.data = {DOMAIN: {evidence.DATA_INDEX_CACHE: _records()}}
    make_entry(hass, "e1", title="SecuraCV", runtime=make_runtime(hass, "e1"))
    make_entry(hass, "e2", title="Garage kernel", runtime=make_runtime(hass, "e2"))
    return hass


def _item(hass, identifier):
    return MediaSourceItem(hass=hass, domain=DOMAIN, identifier=identifier)


def test_the_source_is_registered_under_the_domain_with_the_contract_s_title():
    hass = _hass()
    source = run(media_source.async_get_media_source(hass))
    assert isinstance(source, media_source.SecuraCVEvidenceMediaSource)
    assert source.domain == DOMAIN and source.name == "SecuraCV evidence"


def test_the_root_lists_one_folder_per_entry():
    hass = _hass()
    source = media_source.SecuraCVEvidenceMediaSource(hass)
    root = run(source.async_browse_media(_item(hass, "")))
    assert root.domain == DOMAIN and root.identifier is None
    assert root.title == "SecuraCV evidence"
    assert root.media_class == "directory" and root.can_play is False and root.can_expand is True
    assert [c.identifier for c in root.children] == ["e1", "e2"]
    assert [c.title for c in root.children] == [
        "SecuraCV evidence (SecuraCV)", "SecuraCV evidence (Garage kernel)"
    ]
    assert all(c.can_expand and not c.can_play for c in root.children)


def test_an_entry_folder_lists_its_packs_newest_first_as_playable_zips():
    hass = _hass()
    source = media_source.SecuraCVEvidenceMediaSource(hass)
    folder = run(source.async_browse_media(_item(hass, "e1")))
    assert [c.identifier for c in folder.children] == ["e1/00000002", "e1/00000001"]
    pack = folder.children[0]
    assert pack.media_class == "url" and pack.media_content_type == "application/zip"
    assert pack.can_play is True and pack.can_expand is False
    assert pack.title == "securacv-evidence-2026-10-12-00000002.zip — last 24 h, 3 events"
    assert pack.media_content_id == f"media-source://{DOMAIN}/e1/00000002"
    # Browsing a pack shows the pack; another entry's pack is not under e1.
    assert run(source.async_browse_media(_item(hass, "e1/00000001"))).identifier == "e1/00000001"
    with pytest.raises(BrowseError):
        run(source.async_browse_media(_item(hass, "e1/0000000a")))
    with pytest.raises(BrowseError):
        run(source.async_browse_media(_item(hass, "e9")))
    with pytest.raises(BrowseError):
        run(source.async_browse_media(_item(hass, "e1/../index")))


def test_resolving_a_pack_signs_the_view_s_path_for_24_hours():
    hass = _hass()
    source = media_source.SecuraCVEvidenceMediaSource(hass)
    play = run(source.async_resolve_media(_item(hass, "e2/0000000a")))
    assert play.url == "/api/securacv/evidence/0000000a?authSig=test-86400"
    assert play.mime_type == "application/zip"
    assert signed_paths(hass)[-1][0] == "/api/securacv/evidence/0000000a"
    assert signed_paths(hass)[-1][1].total_seconds() == 24 * 3600
    for bad in ("", "e1", "e1/00000009", "e2/00000002", "e1/zz"):
        with pytest.raises(Unresolvable):
            run(source.async_resolve_media(_item(hass, bad)))
