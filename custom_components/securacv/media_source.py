"""Media source: the evidence packs under Media → SecuraCV evidence.

The same packs ``evidence.py`` writes and serves. The root lists one folder
per SecuraCV entry; each folder lists that entry's packs, newest first;
resolving one yields a 24-hour signed link to the authenticated view
(``/api/securacv/evidence/{export_id}``), signed at resolve time. Nothing
here reads a pack or widens where it can be fetched from.
"""
from __future__ import annotations

from homeassistant.components.media_player import BrowseError, MediaClass
from homeassistant.components.media_source import (
    BrowseMediaSource,
    MediaSource,
    MediaSourceItem,
    PlayMedia,
    Unresolvable,
)
from homeassistant.core import HomeAssistant

from .const import DOMAIN
from .evidence import EXPORT_ID_RE, ExportRecord, async_load_records, signed_download_url

MEDIA_ROOT_TITLE = "SecuraCV evidence"
ZIP_MIME = "application/zip"


async def async_get_media_source(hass: HomeAssistant) -> MediaSource:
    """Home Assistant's entry point."""
    return SecuraCVEvidenceMediaSource(hass)


def _pack_title(record: ExportRecord) -> str:
    return f"{record.file} — last {record.window_hours} h, {record.events} events"


class SecuraCVEvidenceMediaSource(MediaSource):
    """The evidence packs, browsable per entry, resolvable to a signed link."""

    name = MEDIA_ROOT_TITLE

    def __init__(self, hass: HomeAssistant) -> None:
        """Bind to the instance."""
        super().__init__(DOMAIN)
        self.hass = hass

    async def async_browse_media(self, item: MediaSourceItem) -> BrowseMediaSource:
        """Root → entries → packs."""
        identifier = (item.identifier or "").strip("/")
        records = await async_load_records(self.hass)
        if not identifier:
            return self._root(records)
        entry_id, _, export_id = identifier.partition("/")
        entry = self.hass.config_entries.async_get_entry(entry_id)
        if entry is None:
            raise BrowseError(f"Unknown SecuraCV entry {entry_id}")
        if export_id:
            # A pack is a leaf; browsing it shows itself.
            record = self._record_for(records, entry_id, export_id)
            return self._pack(record)
        return self._entry_folder(entry, records)

    async def async_resolve_media(self, item: MediaSourceItem) -> PlayMedia:
        """A pack → its signed download link, signed now for 24 hours."""
        identifier = (item.identifier or "").strip("/")
        entry_id, _, export_id = identifier.partition("/")
        if not entry_id or not export_id:
            raise Unresolvable("Choose an evidence pack, not a folder")
        records = await async_load_records(self.hass)
        try:
            record = self._record_for(records, entry_id, export_id)
        except BrowseError as err:
            raise Unresolvable(str(err)) from err
        return PlayMedia(signed_download_url(self.hass, record.export_id), ZIP_MIME)

    def _record_for(self, records: list[ExportRecord], entry_id: str, export_id: str) -> ExportRecord:
        if not EXPORT_ID_RE.fullmatch(export_id):
            raise BrowseError("Not an evidence pack id")
        for record in records:
            if record.export_id == export_id and record.entry_id == entry_id:
                return record
        raise BrowseError(f"No evidence pack {export_id}")

    def _root(self, records: list[ExportRecord]) -> BrowseMediaSource:
        children = [
            self._entry_folder(entry, records, with_children=False)
            for entry in self.hass.config_entries.async_entries(DOMAIN)
        ]
        return BrowseMediaSource(
            domain=DOMAIN,
            identifier=None,
            media_class=MediaClass.DIRECTORY,
            media_content_type=ZIP_MIME,
            title=MEDIA_ROOT_TITLE,
            can_play=False,
            can_expand=True,
            children=children,
            children_media_class=MediaClass.DIRECTORY,
        )

    def _entry_folder(
        self, entry, records: list[ExportRecord], *, with_children: bool = True
    ) -> BrowseMediaSource:
        title = getattr(entry, "title", None) or "SecuraCV"
        own = [r for r in records if r.entry_id == entry.entry_id]
        return BrowseMediaSource(
            domain=DOMAIN,
            identifier=entry.entry_id,
            media_class=MediaClass.DIRECTORY,
            media_content_type=ZIP_MIME,
            title=f"{MEDIA_ROOT_TITLE} ({title})",
            can_play=False,
            can_expand=True,
            children=[self._pack(r) for r in own] if with_children else None,
            children_media_class=MediaClass.URL,
        )

    def _pack(self, record: ExportRecord) -> BrowseMediaSource:
        return BrowseMediaSource(
            domain=DOMAIN,
            identifier=f"{record.entry_id}/{record.export_id}",
            # MediaClass has no "document"; a downloadable pack is a URL.
            media_class=MediaClass.URL,
            media_content_type=ZIP_MIME,
            title=_pack_title(record),
            can_play=True,
            can_expand=False,
        )
