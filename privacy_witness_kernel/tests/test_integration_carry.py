"""Tests for integration_carry.py — option G (D15), the add-on's one path for
placing the SecuraCV integration under /config/custom_components.

The rules under test, each one a reviewer's must-change:
  * the first install is a click, never a boot (on-start leaves an absent
    destination absent);
  * an unmarked copy (HACS, by hand) and an edited marked copy are never
    touched;
  * the carried version never replaces a newer one;
  * the copy is pruned (no tests/, no www/*.test.js), dereferences symlinks,
    lands atomically, and a symlinked destination is refused;
  * nothing here restarts Core — the on-start path raises a notification.

integration_carry lives at the add-on root (not a package), so it is loaded
by file path like serve_wizard in test_serve_wizard.py.
"""

import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).resolve().parent.parent / "integration_carry.py"
_spec = importlib.util.spec_from_file_location("integration_carry", _MODULE_PATH)
carry = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(carry)


def _make_src(root: Path, version: str = "0.9.0") -> Path:
    """A carried integration tree the way the image holds it."""
    src = root / "image" / "integration" / "securacv"
    (src / "www").mkdir(parents=True)
    (src / "manifest.json").write_text(json.dumps({"domain": "securacv", "version": version}))
    (src / "__init__.py").write_text(f"VERSION = {version!r}\n")
    (src / "www" / "securacv-timeline-card.js").write_text("// card\n")
    return src


@pytest.fixture
def paths(tmp_path):
    root = Path(os.path.realpath(tmp_path))  # no symlinks in the destination chain
    src = _make_src(root)
    dest = root / "config" / "custom_components" / "securacv"
    return src, dest


# ---------------------------------------------------------------------------
# install (the wizard's click)
# ---------------------------------------------------------------------------


def test_first_install_copies_marks_and_records_manifest(paths):
    src, dest = paths
    result = carry.install(src, dest)
    assert result["action"] == "installed"
    assert result["installed_version"] == "0.9.0"
    assert (dest / "manifest.json").is_file()
    assert (dest / carry.MARKER_NAME).read_text() == "0.9.0\n"
    manifest = (dest / carry.MANIFEST_NAME).read_text()
    assert "manifest.json" in manifest and "__init__.py" in manifest
    # The marker and manifest describe the tree without describing themselves.
    assert carry.MARKER_NAME not in manifest and carry.MANIFEST_NAME not in manifest
    assert carry.inspect(dest)["state"] == carry.STATE_MANAGED
    # No temp or old sibling is left behind.
    assert sorted(p.name for p in dest.parent.iterdir()) == ["securacv"]


def test_install_prunes_tests_and_js_tests_and_dereferences_symlinks(paths):
    src, dest = paths
    (src / "tests").mkdir()
    (src / "tests" / "test_x.py").write_text("def test(): pass\n")
    (src / "www" / "securacv-timeline-card.test.js").write_text("// test\n")
    (src / "__pycache__").mkdir()
    (src / "__pycache__" / "x.pyc").write_bytes(b"\x00")
    real = src.parent / "shared.txt"
    real.write_text("shared bytes\n")
    (src / "linked.txt").symlink_to(real)

    assert carry.install(src, dest)["action"] == "installed"
    assert not (dest / "tests").exists()
    assert not (dest / "www" / "securacv-timeline-card.test.js").exists()
    assert not (dest / "__pycache__").exists()
    assert (dest / "www" / "securacv-timeline-card.js").is_file()
    linked = dest / "linked.txt"
    assert linked.is_file() and not linked.is_symlink()
    assert linked.read_text() == "shared bytes\n"


def test_install_refuses_symlinked_destination(paths):
    src, dest = paths
    dest.parent.mkdir(parents=True)
    elsewhere = dest.parent.parent / "elsewhere"
    elsewhere.mkdir()
    dest.symlink_to(elsewhere)
    result = carry.install(src, dest)
    assert result["action"] == "refused"
    assert "symlink" in result["detail"]
    assert list(elsewhere.iterdir()) == []


def test_install_refuses_symlinked_parent(paths):
    src, dest = paths
    real_cc = dest.parent.parent / "real_custom_components"
    real_cc.mkdir(parents=True)
    dest.parent.symlink_to(real_cc)
    result = carry.install(src, dest)
    assert result["action"] == "refused"
    assert "symlink" in result["detail"]
    assert list(real_cc.iterdir()) == []


def test_install_never_touches_an_unmarked_hacs_copy(paths):
    src, dest = paths
    dest.mkdir(parents=True)
    (dest / "manifest.json").write_text(json.dumps({"version": "0.1.0"}))
    (dest / "__init__.py").write_text("# hacs copy\n")
    result = carry.install(src, dest)
    assert result["action"] == "refused"
    assert "HACS" in result["detail"]
    assert (dest / "__init__.py").read_text() == "# hacs copy\n"
    assert not (dest / carry.MARKER_NAME).exists()
    assert carry.inspect(dest)["state"] == carry.STATE_FOREIGN


def test_install_never_touches_an_edited_marked_copy(paths):
    src, dest = paths
    assert carry.install(src, dest)["action"] == "installed"
    (dest / "__init__.py").write_text("# the user patched this\n")
    assert carry.inspect(dest)["state"] == carry.STATE_MODIFIED
    newer = _make_src(src.parent.parent.parent / "newer", version="0.10.0")
    result = carry.install(newer, dest)
    assert result["action"] == "refused"
    assert "edited" in result["detail"]
    assert (dest / "__init__.py").read_text() == "# the user patched this\n"


def test_install_updates_a_managed_copy_to_a_newer_version(paths):
    src, dest = paths
    assert carry.install(src, dest)["action"] == "installed"
    newer = _make_src(src.parent.parent.parent / "newer", version="0.10.0")
    (newer / "new_platform.py").write_text("# new\n")
    result = carry.install(newer, dest)
    assert result["action"] == "updated"
    assert result["installed_version"] == "0.10.0"
    assert (dest / "new_platform.py").is_file()
    assert (dest / carry.MARKER_NAME).read_text() == "0.10.0\n"
    assert carry.inspect(dest) == {"state": carry.STATE_MANAGED, "version": "0.10.0",
                                   "path": str(dest)}
    assert sorted(p.name for p in dest.parent.iterdir()) == ["securacv"]


def test_install_same_version_is_unchanged(paths):
    src, dest = paths
    assert carry.install(src, dest)["action"] == "installed"
    before = (dest / carry.MANIFEST_NAME).read_text()
    result = carry.install(src, dest)
    assert result["action"] == "unchanged"
    assert (dest / carry.MANIFEST_NAME).read_text() == before


def test_install_never_downgrades(paths):
    src, dest = paths
    newer = _make_src(src.parent.parent.parent / "newer", version="0.10.0")
    assert carry.install(newer, dest)["action"] == "installed"
    result = carry.install(src, dest)  # 0.9.0 over 0.10.0
    assert result["action"] == "refused"
    assert "never downgrading" in result["detail"]
    assert carry.read_version(dest) == "0.10.0"


def test_install_refuses_when_nothing_is_carried(paths):
    _src, dest = paths
    result = carry.install(dest.parent / "nowhere", dest)
    assert result["action"] == "refused"
    assert not dest.exists()


def test_version_key_orders_numerically():
    assert carry.version_key("0.10.0") > carry.version_key("0.9.1")
    assert carry.version_key("1.0.0") > carry.version_key("0.99.99")
    assert carry.version_key("0.9.0") == carry.version_key("0.9.0")
    # A pre-release tag sorts after the bare number at that position.
    assert carry.version_key("0.9.0b1") > carry.version_key("0.9.0")


# ---------------------------------------------------------------------------
# on-start (the app-start path)
# ---------------------------------------------------------------------------


def test_on_start_never_performs_the_first_install(paths):
    src, dest = paths
    result = carry.on_start(src, dest)
    assert result["action"] == "unchanged"
    assert "click" in result["detail"]
    assert not dest.exists()


def test_on_start_updates_only_a_managed_copy(paths):
    src, dest = paths
    assert carry.install(src, dest)["action"] == "installed"
    newer = _make_src(src.parent.parent.parent / "newer", version="0.10.0")
    assert carry.on_start(newer, dest)["action"] == "updated"
    assert carry.read_version(dest) == "0.10.0"
    # ...and the same start again does nothing.
    assert carry.on_start(newer, dest)["action"] == "unchanged"


def test_on_start_leaves_hacs_copy_alone(paths):
    src, dest = paths
    dest.mkdir(parents=True)
    (dest / "manifest.json").write_text(json.dumps({"version": "0.1.0"}))
    result = carry.on_start(src, dest)
    assert result["action"] == "refused"
    assert carry.read_version(dest) == "0.1.0"


# ---------------------------------------------------------------------------
# The restart notification: a POST to Core through the Supervisor proxy, and
# nothing else — the module has no code path that restarts Core.
# ---------------------------------------------------------------------------


class _Resp:
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self):
        return b"[]"


def test_notify_restart_needed_posts_a_persistent_notification(monkeypatch):
    captured = {}

    def _fake(req, timeout=None):
        captured["req"] = req
        return _Resp()

    monkeypatch.setattr(carry.urllib.request, "urlopen", _fake)
    assert carry.notify_restart_needed("0.10.0", token="tok") is True
    req = captured["req"]
    assert req.full_url == "http://supervisor/core/api/services/persistent_notification/create"
    assert req.method == "POST"
    assert req.headers["Authorization"] == "Bearer tok"
    body = json.loads(req.data)
    assert body["notification_id"] == carry.NOTIFICATION_ID
    assert "0.10.0" in body["message"]
    assert "Restart Home Assistant" in body["message"]


def test_notify_restart_needed_never_raises(monkeypatch):
    def _boom(req, timeout=None):
        raise OSError("supervisor unreachable")

    monkeypatch.setattr(carry.urllib.request, "urlopen", _boom)
    assert carry.notify_restart_needed("0.10.0", token="tok") is False


def test_module_never_calls_a_core_restart():
    source = _MODULE_PATH.read_text()
    assert "homeassistant/restart" not in source
    assert "/core/restart" not in source


# ---------------------------------------------------------------------------
# CLI (what run.sh calls)
# ---------------------------------------------------------------------------


def _cli(*args):
    out = subprocess.run(
        [sys.executable, "-I", str(_MODULE_PATH), *args],
        capture_output=True, text=True, timeout=60, check=True,
    )
    return json.loads(out.stdout)


def test_cli_status_install_and_on_start(paths):
    src, dest = paths
    status = _cli("status", "--src", str(src), "--dest", str(dest))
    assert status["state"] == "absent"
    assert status["carried_version"] == "0.9.0"
    assert _cli("on-start", "--src", str(src), "--dest", str(dest))["action"] == "unchanged"
    assert not dest.exists()
    assert _cli("install", "--src", str(src), "--dest", str(dest))["action"] == "installed"
    status = _cli("status", "--src", str(src), "--dest", str(dest))
    assert status["state"] == "managed" and status["version"] == "0.9.0"
