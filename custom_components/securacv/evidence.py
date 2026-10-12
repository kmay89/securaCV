"""Evidence export: one zip, written where nobody can read it unauthenticated.

``securacv.export_evidence(window_hours)`` asks the kernel for its signed
bundle (``GET /export/bundle?last=<h>h`` — the one receipted read the
integration makes, a disclosure act) and composes ONE zip,
``securacv-evidence-<YYYY-MM-DD>-<8 hex>.zip``, holding:

  - the bundle bytes verbatim under the kernel's own filename (the receipt
    signs the artifact hash; nothing is added, removed or re-serialized);
  - ``evidence_viewer.html`` — the offline viewer W5 ships under ``www/``
    byte-equal to ``viewer/evidence_viewer.html``;
  - ``README.txt`` — the recipient's recipe (``www/evidence-README.txt``);
  - ``WITNESS_KEY.txt`` — the pinned verifying key and its fingerprint, so a
    recipient can turn "self-attested" into "authorship confirmed".

The zip lands under ``hass.config.path("securacv", "evidence")`` — a
directory Home Assistant does not serve — created 0700, files 0600, written
to a temp name and renamed (never a half-written pack). ``index.json`` lists
every pack newest first; the ``evidence_keep`` option bounds how many stay
(the oldest zip goes, and the notification says so). The pack is served by
one authenticated view, ``/api/securacv/evidence/{export_id}``
(``requires_auth``), and the persistent notification carries a 24-hour
signed link to it — never a ``/local/`` URL (``/config/www`` is served with
no authentication at all). ``media_source.py`` lists the same packs under
Media → SecuraCV evidence.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import os
import re
import secrets
import shutil
import stat
import tempfile
import time
import zipfile
from dataclasses import asdict, dataclass
from datetime import timedelta
from pathlib import Path
from typing import Any

from aiohttp import web

from homeassistant.components import persistent_notification
from homeassistant.components.http import HomeAssistantView
from homeassistant.components.http.auth import async_sign_path
from homeassistant.core import HomeAssistant, callback
from homeassistant.util import dt as dt_util

from .api import BundleDownload
from .const import (
    CONF_EVIDENCE_KEEP,
    CONF_KERNEL_DEVICE_ID,
    DEFAULT_EVIDENCE_KEEP,
    DOMAIN,
    EVIDENCE_DIR_PARTS,
    EVIDENCE_INDEX_FILENAME,
    EVIDENCE_LINK_HOURS,
    EVIDENCE_README_SOURCE,
    EVIDENCE_VIEW_NAME,
    EVIDENCE_VIEW_URL,
    EVIDENCE_VIEWER_SOURCE,
    EVIDENCE_ZIP_KEY,
    EVIDENCE_ZIP_README,
    EVIDENCE_ZIP_VIEWER,
    MAX_EVIDENCE_KEEP,
    MIN_EVIDENCE_KEEP,
    TIME_BUCKET_SECONDS,
)
from .data import SecuraCVConfigEntry, VerificationState, runtime_for
from .kernel_trust import kernel_key_fingerprint, normalize_kernel_key

_LOGGER = logging.getLogger(__name__)

# An export id is eight hex characters, random per export; the view and the
# media source accept nothing else in a path.
EXPORT_ID_RE = re.compile(r"^[0-9a-f]{8}$")
EXPORT_ID_BYTES = 4
INDEX_SCHEMA_VERSION = 1
DIR_MODE = 0o700
FILE_MODE = 0o600
# hass.data key holding "the view is registered" for this instance.
DATA_VIEW_REGISTERED = "_evidence_view_registered"
# hass.data key of the cached index rows (one directory per instance).
DATA_INDEX_CACHE = "_evidence_index"
# One lock per instance around compose + index: index.json is one file for
# every entry, read-modified-written per export, so two exports in flight
# at once (a button press beside an automation) would drop one's row.
DATA_EXPORT_LOCK = "_evidence_export_lock"

NOTIFICATION_TITLE = "SecuraCV evidence export ready"
# The persistent notification, exactly as the contract words it.
NOTIFICATION_MESSAGE = (
    "Evidence pack ready — last {hours} hours, {events} sealed events, chain {chain_state}. "
    "[Download the evidence pack]({signed_url}) (link valid 24 hours; also under "
    "Media → SecuraCV evidence). Send the whole zip to whoever needs to check it. "
    "Your witness key fingerprint, to confirm by phone: `{fingerprint}`."
)
NOTIFICATION_KEEP_LINE = (
    " Removed {removed} older export(s) to stay within your limit of {keep}."
)


@dataclass(frozen=True)
class ExportRecord:
    """One row of index.json (and the response of the action)."""

    export_id: str
    file: str
    created_bucket_start: int
    window_hours: int
    kernel_device_id: str | None
    sha256: str
    bytes: int
    events: int
    entry_id: str

    def as_row(self) -> dict[str, Any]:
        """The index.json row."""
        return asdict(self)


def record_from_row(row: Any) -> ExportRecord | None:
    """An index row back into a record; None for a row this version cannot read."""
    if not isinstance(row, dict):
        return None
    try:
        export_id = str(row["export_id"])
        if not EXPORT_ID_RE.fullmatch(export_id):
            return None
        return ExportRecord(
            export_id=export_id,
            file=str(row["file"]),
            created_bucket_start=int(row.get("created_bucket_start", 0)),
            window_hours=int(row.get("window_hours", 0)),
            kernel_device_id=(
                str(row["kernel_device_id"]) if row.get("kernel_device_id") else None
            ),
            sha256=str(row.get("sha256", "")),
            bytes=int(row.get("bytes", 0)),
            events=int(row.get("events", 0)),
            entry_id=str(row.get("entry_id", "")),
        )
    except (KeyError, TypeError, ValueError):
        return None


def evidence_dir(hass: HomeAssistant) -> Path:
    """`<config>/securacv/evidence` — not under www/, never served statically."""
    return Path(hass.config.path(*EVIDENCE_DIR_PARTS))


def www_dir() -> Path:
    """Where W5 ships the viewer and the README."""
    return Path(__file__).parent / "www"


def new_export_id() -> str:
    """Eight random hex characters."""
    return secrets.token_hex(EXPORT_ID_BYTES)


def zip_filename(date: str, export_id: str) -> str:
    """`securacv-evidence-<YYYY-MM-DD>-<8 hex>.zip`."""
    return f"securacv-evidence-{date}-{export_id}.zip"


def witness_key_text(verifying_key: str, kernel_device_id: str | None) -> str:
    """WITNESS_KEY.txt: `verifying_key=…\\nfingerprint=…\\nkernel=…\\n`."""
    key = normalize_kernel_key(verifying_key) or verifying_key
    fingerprint = kernel_key_fingerprint(key) if normalize_kernel_key(key) else ""
    return f"verifying_key={key}\nfingerprint={fingerprint}\nkernel={kernel_device_id or ''}\n"


def count_bundle_events(bundle_bytes: bytes) -> int:
    """How many sealed events the bundle's artifact carries (display only)."""
    try:
        bundle = json.loads(bundle_bytes)
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
        return 0
    if not isinstance(bundle, dict):
        return 0
    artifact = bundle.get("artifact")
    if not isinstance(artifact, dict):
        return 0
    count = 0
    for batch in artifact.get("batches") or []:
        if not isinstance(batch, dict):
            continue
        for bucket in batch.get("buckets") or []:
            if isinstance(bucket, dict):
                events = bucket.get("events")
                if isinstance(events, list):
                    count += len(events)
    return count


def chain_state_words(state: VerificationState | None) -> str:
    """The `chain {…}` phrase of the notification, from the integration's walk."""
    if state is None or state.ok is None:
        return "not verified yet"
    if state.key_mismatch:
        return "key changed — confirm it in Settings → Repairs"
    if state.ok:
        return "verified ✓"
    return "verification failed ⚠"


def _write_private(path: Path, data: bytes) -> None:
    """Write `data` to `path` atomically, 0600, inside `path`'s directory."""
    fd, tmp_name = tempfile.mkstemp(prefix=".tmp-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        os.chmod(tmp_name, FILE_MODE)
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def _ensure_private_dir(directory: Path) -> None:
    """Create the export directory 0700; refuse a symlinked destination."""
    if directory.is_symlink():
        raise OSError(f"{directory} is a symbolic link; refusing to write evidence there")
    directory.mkdir(parents=True, exist_ok=True, mode=DIR_MODE)
    mode = stat.S_IMODE(directory.stat().st_mode)
    if mode != DIR_MODE:
        os.chmod(directory, DIR_MODE)


def read_index(directory: Path) -> list[ExportRecord]:
    """The index rows on disk, newest first (blocking)."""
    path = directory / EVIDENCE_INDEX_FILENAME
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return []
    except (OSError, json.JSONDecodeError, UnicodeDecodeError, ValueError) as err:
        _LOGGER.warning("evidence index unreadable, starting a new one: %s", err)
        return []
    if not isinstance(raw, dict) or raw.get("v") != INDEX_SCHEMA_VERSION:
        return []
    rows = raw.get("exports")
    if not isinstance(rows, list):
        return []
    records = [record for record in (record_from_row(row) for row in rows) if record is not None]
    records.sort(key=lambda record: record.created_bucket_start, reverse=True)
    return records


def write_index(directory: Path, records: list[ExportRecord]) -> None:
    """Write index.json (newest first), atomically, 0600 (blocking)."""
    rows = [record.as_row() for record in sorted(records, key=lambda r: r.created_bucket_start, reverse=True)]
    payload = json.dumps({"v": INDEX_SCHEMA_VERSION, "exports": rows}, indent=2) + "\n"
    _write_private(directory / EVIDENCE_INDEX_FILENAME, payload.encode("utf-8"))


@dataclass(frozen=True)
class ComposedExport:
    """What the executor job returns."""

    record: ExportRecord
    removed: list[str]
    records: list[ExportRecord]


def compose_export(
    directory: Path,
    *,
    download: BundleDownload,
    verifying_key: str | None,
    kernel_device_id: str | None,
    entry_id: str,
    window_hours: int,
    export_id: str,
    date: str,
    created_bucket_start: int,
    keep: int,
    viewer_source: Path | None,
    readme_source: Path | None,
) -> ComposedExport:
    """Write the zip, the index and prune to `keep` (blocking; executor).

    Every write is a temp file renamed into place; the directory is 0700
    and the files 0600. A viewer or README W5 has not shipped yet is left
    out of the zip (and the README then explains itself by absence) rather
    than failing the export.
    """
    _ensure_private_dir(directory)
    filename = zip_filename(date, export_id)
    fd, tmp_name = tempfile.mkstemp(prefix=".tmp-", suffix=".zip", dir=str(directory))
    try:
        with os.fdopen(fd, "wb") as fh:
            with zipfile.ZipFile(fh, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr(download.filename, download.content)
                if viewer_source is not None and viewer_source.is_file():
                    zf.writestr(EVIDENCE_ZIP_VIEWER, viewer_source.read_bytes())
                if readme_source is not None and readme_source.is_file():
                    zf.writestr(EVIDENCE_ZIP_README, readme_source.read_bytes())
                if verifying_key:
                    zf.writestr(EVIDENCE_ZIP_KEY, witness_key_text(verifying_key, kernel_device_id))
        os.chmod(tmp_name, FILE_MODE)
        os.replace(tmp_name, directory / filename)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise
    zip_bytes = (directory / filename).read_bytes()
    record = ExportRecord(
        export_id=export_id,
        file=filename,
        created_bucket_start=created_bucket_start,
        window_hours=window_hours,
        kernel_device_id=kernel_device_id,
        sha256=hashlib.sha256(zip_bytes).hexdigest(),
        bytes=len(zip_bytes),
        events=count_bundle_events(download.content),
        entry_id=entry_id,
    )
    records = [record] + [r for r in read_index(directory) if r.export_id != export_id]
    records.sort(key=lambda r: r.created_bucket_start, reverse=True)
    keep = max(MIN_EVIDENCE_KEEP, min(MAX_EVIDENCE_KEEP, keep))
    removed: list[str] = []
    while len(records) > keep:
        oldest = records.pop()
        try:
            (directory / oldest.file).unlink()
        except FileNotFoundError:
            pass
        removed.append(oldest.file)
    write_index(directory, records)
    return ComposedExport(record=record, removed=removed, records=records)


def evidence_keep(entry: SecuraCVConfigEntry) -> int:
    """The `evidence_keep` option, bounded."""
    try:
        keep = int(entry.options.get(CONF_EVIDENCE_KEEP, DEFAULT_EVIDENCE_KEEP))
    except (TypeError, ValueError):
        keep = DEFAULT_EVIDENCE_KEEP
    return max(MIN_EVIDENCE_KEEP, min(MAX_EVIDENCE_KEEP, keep))


def signed_download_url(hass: HomeAssistant, export_id: str) -> str:
    """A 24-hour signed path to the authenticated view."""
    return async_sign_path(
        hass, EVIDENCE_VIEW_URL.format(export_id=export_id), timedelta(hours=EVIDENCE_LINK_HOURS)
    )


def _cache_records(hass: HomeAssistant, records: list[ExportRecord]) -> None:
    hass.data.setdefault(DOMAIN, {})[DATA_INDEX_CACHE] = list(records)


def export_lock(hass: HomeAssistant) -> asyncio.Lock:
    """The instance-wide lock every export's compose + index update holds.

    Instance-wide, not per entry: the index (and the keep-N pruning that
    rewrites it) is shared by every kernel entry on this Home Assistant.
    """
    store = hass.data.setdefault(DOMAIN, {})
    lock = store.get(DATA_EXPORT_LOCK)
    if not isinstance(lock, asyncio.Lock):
        lock = asyncio.Lock()
        store[DATA_EXPORT_LOCK] = lock
    return lock


async def async_load_records(hass: HomeAssistant) -> list[ExportRecord]:
    """The index rows, read once from disk and cached for this instance."""
    cached = hass.data.get(DOMAIN, {}).get(DATA_INDEX_CACHE)
    if isinstance(cached, list):
        return list(cached)
    records = await hass.async_add_executor_job(read_index, evidence_dir(hass))
    _cache_records(hass, records)
    return records


def _mirror_to_runtime(hass: HomeAssistant, records: list[ExportRecord]) -> None:
    """Each loaded entry's `runtime.exports` lists its own rows (diagnostics count them)."""
    for entry in hass.config_entries.async_entries(DOMAIN):
        runtime = runtime_for(entry)
        if runtime is None:
            continue
        runtime.exports = {
            "exports": [r.as_row() for r in records if r.entry_id == entry.entry_id]
        }


async def async_export_evidence(
    hass: HomeAssistant, entry: SecuraCVConfigEntry, *, window_hours: int
) -> dict[str, Any]:
    """Download the bundle, compose the pack, notify, and return the response.

    Raises the API errors (the caller translates them) and OSError when the
    pack could not be written.
    """
    runtime = runtime_for(entry)
    if runtime is None or runtime.api is None:
        raise ValueError("entry has no kernel")
    hours = int(window_hours)
    download = await runtime.api.async_download_export_bundle(hours)

    verifying_key = normalize_kernel_key(
        runtime.coordinator.pinned_key if runtime.coordinator is not None else None
    )
    kernel_device_id = entry.data.get(CONF_KERNEL_DEVICE_ID)
    export_id = new_export_id()
    now = time.time()
    # The download runs concurrently with anything; the pack + index write
    # is one export at a time (index.json is a shared read-modify-write).
    async with export_lock(hass):
        composed = await hass.async_add_executor_job(
            lambda: compose_export(
                evidence_dir(hass),
                download=download,
                verifying_key=verifying_key,
                kernel_device_id=str(kernel_device_id) if kernel_device_id else None,
                entry_id=entry.entry_id,
                window_hours=hours,
                export_id=export_id,
                date=dt_util.now().strftime("%Y-%m-%d"),
                created_bucket_start=int(now // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS,
                keep=evidence_keep(entry),
                viewer_source=www_dir() / EVIDENCE_VIEWER_SOURCE,
                readme_source=www_dir() / EVIDENCE_README_SOURCE,
            )
        )
        _cache_records(hass, composed.records)
        _mirror_to_runtime(hass, composed.records)

    record = composed.record
    signed_url = signed_download_url(hass, record.export_id)
    message = NOTIFICATION_MESSAGE.format(
        hours=hours,
        events=record.events,
        chain_state=chain_state_words(runtime.verification),
        signed_url=signed_url,
        fingerprint=kernel_key_fingerprint(verifying_key) if verifying_key else "not pinned yet",
    )
    if composed.removed:
        message += NOTIFICATION_KEEP_LINE.format(
            removed=len(composed.removed), keep=evidence_keep(entry)
        )
    persistent_notification.async_create(
        hass, message, NOTIFICATION_TITLE, f"securacv_export_{entry.entry_id}"
    )
    _LOGGER.info(
        "Evidence pack %s written (%d events, last %d hours)%s",
        record.file,
        record.events,
        hours,
        f"; removed {len(composed.removed)} older pack(s)" if composed.removed else "",
    )
    return {
        "export_id": record.export_id,
        "file": record.file,
        "url": signed_url,
        "sha256": record.sha256,
        "events": record.events,
        "window_hours": hours,
        "kernel_device_id": record.kernel_device_id,
    }


def find_record(records: list[ExportRecord], export_id: str) -> ExportRecord | None:
    """The row for `export_id`, if any."""
    for record in records:
        if record.export_id == export_id:
            return record
    return None


class SecuraCVEvidenceView(HomeAssistantView):
    """Streams one evidence pack to an authenticated caller (or a signed link)."""

    url = EVIDENCE_VIEW_URL
    name = EVIDENCE_VIEW_NAME
    requires_auth = True

    def __init__(self, hass: HomeAssistant) -> None:
        """Bind to the instance whose export directory is served."""
        self.hass = hass

    async def get(self, request: web.Request, export_id: str) -> web.StreamResponse:
        """GET /api/securacv/evidence/{export_id}: the zip as an attachment."""
        if not EXPORT_ID_RE.fullmatch(export_id):
            raise web.HTTPNotFound()
        records = await async_load_records(self.hass)
        record = find_record(records, export_id)
        if record is None:
            raise web.HTTPNotFound()
        path = evidence_dir(self.hass) / record.file
        exists = await self.hass.async_add_executor_job(path.is_file)
        if not exists:
            raise web.HTTPNotFound()
        return web.FileResponse(
            path,
            headers={
                "Content-Type": "application/zip",
                "Content-Disposition": f'attachment; filename="{record.file}"',
            },
        )


@callback
def async_register_view(hass: HomeAssistant) -> None:
    """Register the view once per instance (needs the http component)."""
    domain_data = hass.data.setdefault(DOMAIN, {})
    if domain_data.get(DATA_VIEW_REGISTERED):
        return
    http = getattr(hass, "http", None)
    if http is None:
        _LOGGER.debug("http is not set up; the evidence view waits for the next load")
        return
    http.register_view(SecuraCVEvidenceView(hass))
    domain_data[DATA_VIEW_REGISTERED] = True


def remove_export_files(directory: Path) -> None:
    """Delete every pack and the index (not called by the integration itself:
    exports are the owner's evidence and survive removal; here for tests and
    a future explicit action)."""
    if directory.is_dir():
        shutil.rmtree(directory)
