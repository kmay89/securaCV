"""Evidence export: one private zip, an index, keep-N, a signed link, a view.

What is pinned:
  - the pack is written under `<config>/securacv/evidence` (never www/),
    the directory 0700, every file 0600, through a temp name and a rename;
  - the zip holds the bundle verbatim under the kernel's filename, the
    viewer and the README when W5's files exist, and WITNESS_KEY.txt in the
    contract's exact shape;
  - index.json lists the packs newest first with the contract's fields;
    keep-N removes the oldest zips and the notification says so;
  - the notification is worded exactly as the contract words it and carries
    a 24-hour signed link to the authenticated view — no /local/ URL;
  - the view refuses anything but an eight-hex id it knows, requires auth,
    and streams the zip as an attachment;
  - the action's response shape.
"""

from __future__ import annotations

import json
import os
import re
import stat
import zipfile
from datetime import timedelta
from pathlib import Path

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, make_runtime, notifications, run, signed_paths

from aiohttp import web  # noqa: E402

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import evidence  # noqa: E402
from ..api import BundleDownload  # noqa: E402
from ..const import (  # noqa: E402
    CONF_EVIDENCE_KEEP,
    CONF_KERNEL_DEVICE_ID,
    DOMAIN,
    EVIDENCE_VIEW_NAME,
    EVIDENCE_VIEW_URL,
)
from ..data import VerificationState  # noqa: E402
from ..kernel_trust import kernel_key_fingerprint  # noqa: E402

KEY = "ab" * 32
BUNDLE = json.dumps(
    {"artifact": {"batches": [{"buckets": [{"events": [{}, {}]}, {"events": [{}]}]}]},
     "receipt_entry": {}, "device_public_key": [0] * 32},
    separators=(",", ":"),
).encode()
DOWNLOAD = BundleDownload(filename="securacv-events-1700000400.json", content=BUNDLE)


def _compose(directory: Path, export_id: str, created: int, keep: int = 20, **extra):
    kwargs = dict(
        download=DOWNLOAD, verifying_key=KEY, kernel_device_id="pwk_1a2b3c4d",
        entry_id="e1", window_hours=24, export_id=export_id, date="2026-10-12",
        created_bucket_start=created, keep=keep, viewer_source=None, readme_source=None,
    )
    kwargs.update(extra)
    return evidence.compose_export(directory, **kwargs)


# ─── the pack on disk ──────────────────────────────────────────────────


def test_the_pack_is_private_atomic_and_holds_the_contract_s_members(tmp_path):
    directory = tmp_path / "securacv" / "evidence"
    viewer = tmp_path / "securacv-evidence-viewer.html"
    viewer.write_text("<html>viewer</html>", encoding="utf-8")
    readme = tmp_path / "evidence-README.txt"
    readme.write_text("Open evidence_viewer.html offline.\n", encoding="utf-8")

    composed = _compose(directory, "deadbeef", 1700000400, viewer_source=viewer, readme_source=readme)
    record = composed.record
    assert record.file == "securacv-evidence-2026-10-12-deadbeef.zip"
    assert record.export_id == "deadbeef" and record.events == 3
    assert record.window_hours == 24 and record.kernel_device_id == "pwk_1a2b3c4d"
    assert record.entry_id == "e1" and record.created_bucket_start == 1700000400

    assert stat.S_IMODE(directory.stat().st_mode) == 0o700
    zip_path = directory / record.file
    assert stat.S_IMODE(zip_path.stat().st_mode) == 0o600
    assert stat.S_IMODE((directory / "index.json").stat().st_mode) == 0o600
    assert record.bytes == zip_path.stat().st_size
    assert record.sha256 == __import__("hashlib").sha256(zip_path.read_bytes()).hexdigest()
    assert not [p for p in directory.iterdir() if p.name.startswith(".tmp-")], "no temp file left"

    with zipfile.ZipFile(zip_path) as zf:
        assert sorted(zf.namelist()) == sorted([
            "securacv-events-1700000400.json", "evidence_viewer.html", "README.txt", "WITNESS_KEY.txt",
        ])
        assert zf.read("securacv-events-1700000400.json") == BUNDLE, "the bundle, verbatim"
        assert zf.read("evidence_viewer.html") == b"<html>viewer</html>"
        assert zf.read("README.txt") == b"Open evidence_viewer.html offline.\n"
        assert zf.read("WITNESS_KEY.txt").decode() == (
            f"verifying_key={KEY}\nfingerprint={kernel_key_fingerprint(KEY)}\nkernel=pwk_1a2b3c4d\n"
        )


def test_a_viewer_or_readme_w5_has_not_shipped_is_left_out_not_fatal(tmp_path):
    composed = _compose(tmp_path / "ev", "cafebabe", 1700000400,
                        viewer_source=tmp_path / "missing.html", readme_source=None)
    with zipfile.ZipFile(tmp_path / "ev" / composed.record.file) as zf:
        assert sorted(zf.namelist()) == ["WITNESS_KEY.txt", "securacv-events-1700000400.json"]


def test_a_symlinked_destination_is_refused(tmp_path):
    real = tmp_path / "elsewhere"
    real.mkdir()
    link = tmp_path / "evidence"
    link.symlink_to(real)
    with pytest.raises(OSError, match="symbolic link"):
        _compose(link, "deadbeef", 1700000400)


def test_the_index_is_newest_first_and_keep_n_removes_the_oldest(tmp_path):
    directory = tmp_path / "ev"
    _compose(directory, "00000001", 1700000400, keep=2)
    _compose(directory, "00000002", 1700001000, keep=2)
    index = json.loads((directory / "index.json").read_text(encoding="utf-8"))
    assert index["v"] == 1
    assert [row["export_id"] for row in index["exports"]] == ["00000002", "00000001"]
    assert set(index["exports"][0]) == {
        "export_id", "file", "created_bucket_start", "window_hours", "kernel_device_id",
        "sha256", "bytes", "events", "entry_id",
    }

    third = _compose(directory, "00000003", 1700001600, keep=2)
    assert third.removed == ["securacv-evidence-2026-10-12-00000001.zip"]
    assert not (directory / "securacv-evidence-2026-10-12-00000001.zip").exists()
    assert [r.export_id for r in evidence.read_index(directory)] == ["00000003", "00000002"]
    assert [r.export_id for r in third.records] == ["00000003", "00000002"]


def test_the_index_survives_junk_and_an_unknown_version(tmp_path):
    directory = tmp_path / "ev"
    directory.mkdir()
    (directory / "index.json").write_text("{not json", encoding="utf-8")
    assert evidence.read_index(directory) == []
    (directory / "index.json").write_text(json.dumps({"v": 9, "exports": [{"export_id": "x"}]}), encoding="utf-8")
    assert evidence.read_index(directory) == []
    (directory / "index.json").write_text(json.dumps({"v": 1, "exports": [
        {"export_id": "deadbeef", "file": "a.zip", "created_bucket_start": 5},
        {"export_id": "not-hex!", "file": "b.zip"},
        "junk",
    ]}), encoding="utf-8")
    assert [r.export_id for r in evidence.read_index(directory)] == ["deadbeef"]
    assert evidence.read_index(tmp_path / "absent") == []


def test_the_event_count_reads_the_artifact_and_tolerates_junk():
    assert evidence.count_bundle_events(BUNDLE) == 3
    assert evidence.count_bundle_events(b"not json") == 0
    assert evidence.count_bundle_events(b'{"artifact": {"batches": "x"}}') == 0


def test_chain_state_words():
    assert evidence.chain_state_words(None) == "not verified yet"
    assert evidence.chain_state_words(VerificationState()) == "not verified yet"
    assert evidence.chain_state_words(VerificationState(ok=True)) == "verified ✓"
    assert evidence.chain_state_words(VerificationState(ok=False)) == "verification failed ⚠"
    assert "Repairs" in evidence.chain_state_words(VerificationState(ok=True, key_mismatch=True))


# ─── the action end to end ─────────────────────────────────────────────


class FakeApi:
    def __init__(self) -> None:
        self.windows: list[int] = []

    async def async_download_export_bundle(self, window_hours):
        self.windows.append(window_hours)
        return DOWNLOAD


def _entry(hass, **options):
    import types

    api = FakeApi()
    coordinator = types.SimpleNamespace(pinned_key=KEY, kernel_available=True, data={})
    runtime = make_runtime(hass, coordinator=coordinator, api=api)
    runtime.verification = VerificationState(ok=True, verified_bucket_start=1700001200)
    entry = make_entry(
        hass, data={CONF_KERNEL_DEVICE_ID: "pwk_1a2b3c4d"}, runtime=runtime, options=options
    )
    return entry, api


def test_export_writes_notifies_and_answers(monkeypatch):
    hass = HomeAssistant()
    hass.data = {}
    entry, api = _entry(hass)
    monkeypatch.setattr(evidence, "new_export_id", lambda: "0badf00d")
    response = run(evidence.async_export_evidence(hass, entry, window_hours=48))

    assert api.windows == [48]
    assert response["export_id"] == "0badf00d"
    assert re.fullmatch(r"securacv-evidence-\d{4}-\d{2}-\d{2}-0badf00d\.zip", response["file"])
    assert response["url"] == "/api/securacv/evidence/0badf00d?authSig=test-86400"
    assert response["events"] == 3 and response["window_hours"] == 48
    assert response["kernel_device_id"] == "pwk_1a2b3c4d"
    assert set(response) == {"export_id", "file", "url", "sha256", "events", "window_hours", "kernel_device_id"}

    directory = Path(hass.config.path("securacv", "evidence"))
    assert (directory / response["file"]).is_file()
    assert "www" not in str(directory)
    assert signed_paths(hass) == [("/api/securacv/evidence/0badf00d", timedelta(hours=24))]

    (note,) = notifications(hass)
    assert note["title"] == "SecuraCV evidence export ready"
    assert note["notification_id"] == f"securacv_export_{entry.entry_id}"
    assert note["message"] == (
        "Evidence pack ready — last 48 hours, 3 sealed events, chain verified ✓. "
        "[Download the evidence pack](/api/securacv/evidence/0badf00d?authSig=test-86400) "
        "(link valid 24 hours; also under Media → SecuraCV evidence). Send the whole zip to "
        "whoever needs to check it. Your witness key fingerprint, to confirm by phone: "
        f"`{kernel_key_fingerprint(KEY)}`."
    )
    assert "/local/" not in note["message"]
    # The runtime mirrors its rows (diagnostics count them) and the cache is primed.
    assert [r["export_id"] for r in entry.runtime_data.exports["exports"]] == ["0badf00d"]
    assert [r.export_id for r in run(evidence.async_load_records(hass))] == ["0badf00d"]


def test_keep_n_from_the_options_is_honored_and_said(monkeypatch):
    hass = HomeAssistant()
    hass.data = {}
    entry, _api = _entry(hass, **{CONF_EVIDENCE_KEEP: 1})
    ids = iter(["00000001", "00000002"])
    monkeypatch.setattr(evidence, "new_export_id", lambda: next(ids))
    run(evidence.async_export_evidence(hass, entry, window_hours=24))
    run(evidence.async_export_evidence(hass, entry, window_hours=24))
    directory = Path(hass.config.path("securacv", "evidence"))
    zips = sorted(p.name for p in directory.glob("*.zip"))
    assert len(zips) == 1 and zips[0].endswith("-00000002.zip")
    assert notifications(hass)[-1]["message"].endswith(
        " Removed 1 older export(s) to stay within your limit of 1."
    )
    assert evidence.evidence_keep(entry) == 1
    entry.options = {CONF_EVIDENCE_KEEP: 999}
    assert evidence.evidence_keep(entry) == 200
    entry.options = {CONF_EVIDENCE_KEEP: "many"}
    assert evidence.evidence_keep(entry) == 20


def test_an_entry_without_a_kernel_cannot_export():
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass)
    with pytest.raises(ValueError):
        run(evidence.async_export_evidence(hass, entry, window_hours=24))


# ─── the authenticated view ────────────────────────────────────────────


def test_the_view_is_authenticated_at_the_contract_s_url():
    assert evidence.SecuraCVEvidenceView.url == EVIDENCE_VIEW_URL == "/api/securacv/evidence/{export_id}"
    assert evidence.SecuraCVEvidenceView.name == EVIDENCE_VIEW_NAME == "api:securacv:evidence"
    assert evidence.SecuraCVEvidenceView.requires_auth is True


def test_the_view_serves_a_known_pack_as_an_attachment_and_404s_the_rest(monkeypatch):
    hass = HomeAssistant()
    hass.data = {}
    entry, _api = _entry(hass)
    monkeypatch.setattr(evidence, "new_export_id", lambda: "0badf00d")
    response = run(evidence.async_export_evidence(hass, entry, window_hours=24))
    view = evidence.SecuraCVEvidenceView(hass)

    served = run(view.get(object(), "0badf00d"))
    assert isinstance(served, web.FileResponse)
    # The stub FileResponse records `path`; the real aiohttp one keeps `_path`
    # (this suite also runs with a real aiohttp installed beside the stubs).
    served_path = getattr(served, "path", None) or getattr(served, "_path")
    assert Path(served_path).name == response["file"]
    assert served.headers["Content-Type"] == "application/zip"
    assert served.headers["Content-Disposition"] == f'attachment; filename="{response["file"]}"'

    for bad in ("0badf00e", "../index.json", "0BADF00D", "0badf00d/", "x" * 8, ""):
        with pytest.raises(web.HTTPNotFound):
            run(view.get(object(), bad))

    # A pack whose zip is gone (deleted by hand) is a 404, not a traceback.
    os.unlink(Path(hass.config.path("securacv", "evidence")) / response["file"])
    with pytest.raises(web.HTTPNotFound):
        run(view.get(object(), "0badf00d"))


def test_the_view_registers_once_per_instance_and_waits_for_http():
    import types

    hass = HomeAssistant()
    hass.data = {}
    evidence.async_register_view(hass)  # no http yet: nothing, no error
    assert not hass.data[DOMAIN].get(evidence.DATA_VIEW_REGISTERED)
    registered: list = []
    hass.http = types.SimpleNamespace(register_view=registered.append)
    evidence.async_register_view(hass)
    evidence.async_register_view(hass)
    assert len(registered) == 1 and isinstance(registered[0], evidence.SecuraCVEvidenceView)
