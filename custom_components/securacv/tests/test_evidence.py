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
import shutil
import stat
import subprocess
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


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_viewer_s_zip_core_reads_the_pack_the_integration_writes(tmp_path):
    """Contract C7, both ends: the zip this module writes is opened by the
    offline viewer's own zip reader (viewer/zip_core.js, inlined into the
    viewer the pack carries) — it finds the kernel-named bundle member, hands
    back its bytes verbatim, and reads WITNESS_KEY.txt."""
    zip_core = Path(__file__).resolve().parents[3] / "viewer" / "zip_core.js"
    if not zip_core.is_file():
        pytest.skip("viewer/zip_core.js lives at the repository root")
    directory = tmp_path / "securacv" / "evidence"
    viewer = tmp_path / "securacv-evidence-viewer.html"
    viewer.write_text("<html>viewer</html>", encoding="utf-8")
    readme = tmp_path / "evidence-README.txt"
    readme.write_text("Open evidence_viewer.html offline.\n", encoding="utf-8")
    composed = _compose(directory, "deadbeef", 1700000400, viewer_source=viewer, readme_source=readme)
    zip_path = directory / composed.record.file

    # Under `node -e`, the user arguments start at process.argv[1].
    script = (
        "const z = require(process.argv[1]);"
        "const bytes = require('node:fs').readFileSync(process.argv[2]);"
        "z.extractEvidence(new Uint8Array(bytes)).then((out) => {"
        "  process.stdout.write(JSON.stringify({memberName: out.memberName, text: out.text,"
        "    members: out.members, witnessKey: out.witnessKey}));"
        "}).catch((e) => { process.stderr.write(String(e && e.stack || e)); process.exit(1); });"
    )
    proc = subprocess.run(
        ["node", "-e", script, str(zip_core), str(zip_path)],
        capture_output=True, text=True, timeout=60, check=False,
    )
    assert proc.returncode == 0, proc.stderr
    out = json.loads(proc.stdout)
    assert out["memberName"] == "securacv-events-1700000400.json"
    assert out["text"].encode("utf-8") == BUNDLE, "the bundle, byte for byte"
    assert sorted(out["members"]) == sorted(
        ["securacv-events-1700000400.json", "evidence_viewer.html", "README.txt", "WITNESS_KEY.txt"]
    )
    assert out["witnessKey"]["verifying_key"] == KEY
    assert out["witnessKey"]["fingerprint"] == kernel_key_fingerprint(KEY)


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


def test_concurrent_exports_are_serialized_and_both_rows_survive(monkeypatch):
    """index.json is one file per instance, read-modified-written per export:
    two exports in flight at once (a button press beside an automation) must
    both land in it. The compose + index step holds the instance lock, so the
    second export's compose only starts after the first one's index write."""
    import asyncio
    import types

    hass = HomeAssistant()
    hass.data = {}
    gate = asyncio.Event()

    class GatedApi:
        """Both downloads are in flight before either export composes."""

        def __init__(self):
            self.windows: list[int] = []

        async def async_download_export_bundle(self, window_hours):
            self.windows.append(window_hours)
            await gate.wait()
            return DOWNLOAD

    api = GatedApi()
    coordinator = types.SimpleNamespace(pinned_key=KEY, kernel_available=True, data={})
    runtime = make_runtime(hass, coordinator=coordinator, api=api)
    runtime.verification = VerificationState(ok=True, verified_bucket_start=1700001200)
    entry = make_entry(hass, data={CONF_KERNEL_DEVICE_ID: "pwk_1a2b3c4d"}, runtime=runtime)
    ids = iter(["00000001", "00000002"])
    monkeypatch.setattr(evidence, "new_export_id", lambda: next(ids))

    held: list[bool] = []
    real_compose = evidence.compose_export

    def spying_compose(*args, **kwargs):
        held.append(evidence.export_lock(hass).locked())
        return real_compose(*args, **kwargs)

    monkeypatch.setattr(evidence, "compose_export", spying_compose)

    async def two_at_once():
        first = asyncio.ensure_future(evidence.async_export_evidence(hass, entry, window_hours=24))
        second = asyncio.ensure_future(evidence.async_export_evidence(hass, entry, window_hours=48))
        await asyncio.sleep(0)
        assert api.windows == [24, 48], "both downloads were in flight before either composed"
        gate.set()
        return await asyncio.gather(first, second)

    first, second = run(two_at_once())
    assert held == [True, True], "every compose ran under the instance lock"
    assert evidence.export_lock(hass).locked() is False
    directory = Path(hass.config.path("securacv", "evidence"))
    rows = evidence.read_index(directory)
    assert {r.export_id for r in rows} == {"00000001", "00000002"}, "neither row was dropped"
    assert sorted(p.name for p in directory.glob("*.zip")) == sorted(r.file for r in rows)
    assert {first["export_id"], second["export_id"]} == {"00000001", "00000002"}
    assert sorted(r["export_id"] for r in entry.runtime_data.exports["exports"]) == ["00000001", "00000002"]
    assert sorted(r.export_id for r in run(evidence.async_load_records(hass))) == ["00000001", "00000002"]
    # One lock per instance, shared by every entry; never a fresh one per call.
    assert evidence.export_lock(hass) is evidence.export_lock(hass)


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
