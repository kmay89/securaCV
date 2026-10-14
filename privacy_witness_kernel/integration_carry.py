#!/usr/bin/env python3
"""Carry the SecuraCV integration from the app image into Home Assistant.

The app image ships a pruned copy of ``custom_components/securacv`` (no
tests, no ``www/*.test.js``) under ``/usr/share/securacv/integration``. This
module is the ONE place that copies it into ``/config/custom_components``
(option G of the product spec, decision D15), and it is deliberately timid:

* The FIRST install happens only on the wizard's explicit click (the done
  screen's "Install the integration and restart Home Assistant"). Nothing is
  copied on boot into a tree that has no SecuraCV integration yet.
* On app start the copy is UPDATED only when the installed tree is the app's
  own: it carries the marker ``.installed_by_securacv_app`` AND every file
  still matches the sha256 manifest the app wrote beside it. A tree without
  the marker (HACS or a hand copy) or with any edited file is never touched;
  HACS becomes the updater the moment it manages the copy.
* The carried version is never installed over a newer one (no downgrade).
* The copy is written to a temporary sibling directory, then swapped in with
  ``os.rename`` (atomic on one filesystem); symlinks are dereferenced on the
  way (the ``cp -RL`` lesson from RELEASE_LESSONS.md) and a symlinked
  destination is refused outright.
* The app NEVER restarts Home Assistant Core by itself. After an update on
  start it raises a persistent notification asking the person to restart;
  the wizard's restart button is the only restart path, and it is a click.

stdlib only (the app image has no third-party Python). Importable by
serve_wizard.py and runnable as a CLI from run.sh:

    integration_carry.py status   [--src DIR] [--dest DIR]
    integration_carry.py install  [--src DIR] [--dest DIR]
    integration_carry.py on-start [--src DIR] [--dest DIR] [--notify]

Every command prints one JSON object on stdout and exits 0; a refusal is a
JSON ``action`` of ``refused``, not a non-zero exit, so run.sh can log it
without failing the container start.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path

MARKER_NAME = ".installed_by_securacv_app"
MANIFEST_NAME = ".securacv_app_sha256sums"
DEFAULT_SRC = Path("/usr/share/securacv/integration/securacv")
DEFAULT_DEST = Path("/config/custom_components/securacv")
SUPERVISOR_URL = "http://supervisor"
NOTIFICATION_ID = "securacv_integration_update"
# Never carried into /config even if the image somehow still holds them.
PRUNE_PATTERNS = ("__pycache__", "*.pyc", "tests", "*.test.js", ".pytest_cache")

STATE_ABSENT = "absent"
STATE_MANAGED = "managed"
STATE_MODIFIED = "modified"
STATE_FOREIGN = "foreign"
STATE_SYMLINK = "symlink"


def read_version(tree: Path) -> str | None:
    """The ``version`` of ``tree/manifest.json``, or None when unreadable."""
    try:
        data = json.loads((tree / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    version = data.get("version") if isinstance(data, dict) else None
    return str(version) if version else None


def version_key(version: str) -> tuple:
    """Order versions numerically per dotted part ("0.10.0" > "0.9.1"); a
    non-numeric part sorts after every numeric one at that position."""
    key = []
    for part in str(version).split("."):
        if part.isdigit():
            key.append((0, int(part), ""))
        else:
            key.append((1, 0, part))
    return tuple(key)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_sha256sums(tree: Path) -> dict[str, str]:
    """``{relative_path: sha256}`` for every regular file under ``tree``,
    except the app's own marker/manifest and Python caches."""
    sums: dict[str, str] = {}
    for root, dirs, files in os.walk(tree):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__")
        for name in sorted(files):
            if name in (MARKER_NAME, MANIFEST_NAME) or name.endswith(".pyc"):
                continue
            path = Path(root) / name
            if not path.is_file():
                continue
            sums[str(path.relative_to(tree))] = _sha256_file(path)
    return sums


def _read_manifest(tree: Path) -> dict[str, str] | None:
    try:
        text = (tree / MANIFEST_NAME).read_text(encoding="utf-8")
    except OSError:
        return None
    sums: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            return None
        sums[parts[1]] = parts[0]
    return sums


def _write_manifest(tree: Path) -> None:
    sums = tree_sha256sums(tree)
    lines = [f"{sums[rel]}  {rel}" for rel in sorted(sums)]
    (tree / MANIFEST_NAME).write_text("\n".join(lines) + "\n", encoding="utf-8")


def inspect(dest: Path = DEFAULT_DEST) -> dict:
    """What lives at ``dest`` and whether the app may touch it.

    ``state`` is one of absent | managed | modified | foreign | symlink;
    ``version`` is the installed manifest.json version (None when absent).
    """
    if dest.is_symlink():
        return {"state": STATE_SYMLINK, "version": None, "path": str(dest)}
    if not dest.exists():
        return {"state": STATE_ABSENT, "version": None, "path": str(dest)}
    version = read_version(dest)
    marker = dest / MARKER_NAME
    if not marker.is_file():
        return {"state": STATE_FOREIGN, "version": version, "path": str(dest)}
    recorded = _read_manifest(dest)
    if recorded is None or recorded != tree_sha256sums(dest):
        return {"state": STATE_MODIFIED, "version": version, "path": str(dest)}
    return {"state": STATE_MANAGED, "version": version, "path": str(dest)}


def _symlink_in_path(path: Path) -> Path | None:
    """The first symlinked component on the way to ``path`` (itself or any
    parent), or None. A root copy must never be redirected by a link."""
    for candidate in (path, *path.parents):
        if candidate.is_symlink():
            return candidate
    return None


def _copy_pruned(src: Path, tmp: Path) -> None:
    # symlinks=False dereferences every link (the cp -RL lesson): the tree
    # that lands in /config holds files, never pointers into the image.
    shutil.copytree(src, tmp, symlinks=False, ignore=shutil.ignore_patterns(*PRUNE_PATTERNS))


def _swap_in(tmp: Path, dest: Path) -> None:
    """Atomically replace ``dest`` with ``tmp`` (both under one parent)."""
    old: Path | None = None
    if dest.exists():
        old = dest.parent / f".securacv.old.{os.getpid()}.{secrets.token_hex(4)}"
        os.rename(dest, old)
    try:
        os.rename(tmp, dest)
    except OSError:
        if old is not None:
            os.rename(old, dest)
        raise
    if old is not None:
        shutil.rmtree(old, ignore_errors=True)


def install(src: Path = DEFAULT_SRC, dest: Path = DEFAULT_DEST, *,
            allow_first_install: bool = True) -> dict:
    """Install or update the carried integration at ``dest``.

    Returns ``{"action": installed|updated|unchanged|refused, "detail", ...}``
    with the carried and installed versions. With ``allow_first_install``
    False (the on-start path) an absent destination is left absent.
    """
    carried = read_version(src)
    result = {"carried_version": carried, "path": str(dest)}
    if carried is None:
        result.update(action="refused", detail=f"no carried integration at {src}")
        return result
    link = _symlink_in_path(dest)
    if link is not None:
        result.update(action="refused",
                      detail=f"{link} is a symlink; the app will not copy through a link")
        return result
    state = inspect(dest)
    result["installed_version"] = state["version"]
    result["state"] = state["state"]
    if state["state"] == STATE_FOREIGN:
        result.update(action="refused", detail=(
            "a SecuraCV integration is already installed by HACS or by hand "
            f"(no {MARKER_NAME} marker); the app never touches that copy — "
            "update it through HACS"))
        return result
    if state["state"] == STATE_MODIFIED:
        result.update(action="refused", detail=(
            "the installed copy was edited after the app placed it; the app "
            "never overwrites an edited copy — remove it to let the app reinstall"))
        return result
    if state["state"] == STATE_ABSENT and not allow_first_install:
        result.update(action="unchanged",
                      detail="no SecuraCV integration installed; the first install is a click in the wizard")
        return result
    if state["state"] == STATE_MANAGED:
        installed = state["version"] or "0"
        if version_key(installed) > version_key(carried):
            result.update(action="refused", detail=(
                f"installed {installed} is newer than carried {carried}; never downgrading"))
            return result
        if version_key(installed) == version_key(carried):
            result.update(action="unchanged", detail=f"{carried} already installed")
            return result

    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.parent / f".securacv.new.{os.getpid()}.{secrets.token_hex(4)}"
    try:
        _copy_pruned(src, tmp)
        (tmp / MARKER_NAME).write_text(carried + "\n", encoding="utf-8")
        _write_manifest(tmp)
        _swap_in(tmp, dest)
    except Exception as exc:  # the caller reports; a half copy never lands
        shutil.rmtree(tmp, ignore_errors=True)
        result.update(action="refused", detail=f"copy failed: {exc}")
        return result
    action = "installed" if state["state"] == STATE_ABSENT else "updated"
    result.update(action=action, detail=f"{action} SecuraCV integration {carried} at {dest}",
                  installed_version=carried)
    return result


def on_start(src: Path = DEFAULT_SRC, dest: Path = DEFAULT_DEST) -> dict:
    """The app-start path: update the app's own copy when the carried version
    is newer; never a first install, never a HACS/edited copy."""
    return install(src, dest, allow_first_install=False)


def notify_restart_needed(version: str, *, supervisor_url: str = SUPERVISOR_URL,
                          token: str | None = None, timeout: int = 10) -> bool:
    """Raise the Core persistent notification that asks for the one restart.
    Returns True when Core accepted it. Never raises: a missed notification
    is logged by the caller, never fatal to the start."""
    token = token if token is not None else (
        os.environ.get("SUPERVISOR_TOKEN") or os.environ.get("HASSIO_TOKEN", ""))
    body = json.dumps({
        "notification_id": NOTIFICATION_ID,
        "title": "SecuraCV integration updated",
        "message": (
            f"SecuraCV integration {version} is installed. Restart Home Assistant to "
            "load it: Settings → System → Restart. Until then the previous version "
            "keeps running."
        ),
    }).encode()
    req = urllib.request.Request(
        f"{supervisor_url}/core/api/services/persistent_notification/create",
        data=body, method="POST",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout):
            return True
    except (urllib.error.URLError, OSError, ValueError):
        return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("status", "install", "on-start"))
    parser.add_argument("--src", type=Path, default=DEFAULT_SRC)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--notify", action="store_true",
                        help="on-start: raise the Core restart notification after an update")
    args = parser.parse_args(argv)
    if args.command == "status":
        result = inspect(args.dest)
        result["carried_version"] = read_version(args.src)
    elif args.command == "install":
        result = install(args.src, args.dest)
    else:
        result = on_start(args.src, args.dest)
        if args.notify and result.get("action") == "updated":
            result["notified"] = notify_restart_needed(str(result["installed_version"]))
    sys.stdout.write(json.dumps(result, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
