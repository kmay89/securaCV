#!/usr/bin/env python3
"""Each desktop app states its version in four files. They must agree.

A Tauri app carries its version in four places, and each one is authoritative
for something different:

    src-tauri/tauri.conf.json   the BUNDLE version — and what the release
                                workflows derive the release TAG from
    package.json                the npm package version
    src-tauri/Cargo.toml        CARGO_PKG_VERSION — what the running app
                                REPORTS TO THE USER in its footer
    src-tauri/Cargo.lock        the resolved version of the app's own package.
                                Cargo rewrites it on the next build, so a stale
                                entry shows up as an unexplained one-line diff
                                in someone else's PR — or as a dirty tree in a
                                release job that expects to build, not to edit.

Nothing keeps them in step, and they drifted: the Flasher shipped as
`flasher-v0.2.2` while its own window footer read `v0.1.0`, because the footer
comes from Cargo.toml (`lib.rs`: `env!("CARGO_PKG_VERSION")`) and only the other
two had ever been bumped. A user reporting a bug from that build would name a
version that was never released, and the maintainer would look for it in the
wrong tree.

An earlier version of this check compared two of the three files — which is
worse than useless, because it *reads* as covered while leaving the number
humans actually see unguarded. Then it covered three and missed Cargo.lock, and
a bump shipped with a stale lockfile while this script printed a tick. A guard
is only as wide as its list, and that is the whole lesson: when a version lands
in a new file, it goes in here in the same change. So: all four, every app, one
script.

Run locally:  python3 desktop/scripts/check_app_versions.py
CI:           lint.yml, and both app release workflows before they build
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# app label -> the four files, each with how to read the version out of it.
APPS: dict[str, dict[str, str]] = {
    "SecuraCV Flasher": {
        "dir": "desktop",
        # The release tag comes from this one, so it is the reference the others
        # are compared against.
        "tauri": "desktop/src-tauri/tauri.conf.json",
        "npm": "desktop/package.json",
        "cargo": "desktop/src-tauri/Cargo.toml",
        "lock": "desktop/src-tauri/Cargo.lock",
        # The [[package]] whose name this is — the app's own entry in the lock.
        "crate": "securacv-flasher",
    },
    "SecuraCV Lab": {
        "dir": "desktop-lab",
        "tauri": "desktop-lab/src-tauri/tauri.conf.json",
        "npm": "desktop-lab/package.json",
        "cargo": "desktop-lab/src-tauri/Cargo.toml",
        "lock": "desktop-lab/src-tauri/Cargo.lock",
        "crate": "securacv-lab",
    },
}

CARGO_VERSION = re.compile(r"^version\s*=\s*\"([^\"]+)\"", re.MULTILINE)


def read_json_version(path: Path) -> str | None:
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("version")
    except (OSError, json.JSONDecodeError):
        return None


def read_cargo_version(path: Path) -> str | None:
    """First top-level `version = "…"` — i.e. [package], which precedes deps."""
    try:
        m = CARGO_VERSION.search(path.read_text(encoding="utf-8"))
    except OSError:
        return None
    return m.group(1) if m else None


def read_lock_version(path: Path, crate: str) -> str | None:
    """The `version` of the `[[package]]` named `crate` in a Cargo.lock.

    Parsed rather than regex-searched for a bare version, because a lockfile
    holds hundreds of packages and the app's own entry is not special in any
    way a loose pattern would notice.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    for block in text.split("[[package]]"):
        name = re.search(r'^name\s*=\s*"([^"]+)"', block, re.MULTILINE)
        if name and name.group(1) == crate:
            version = re.search(r'^version\s*=\s*"([^"]+)"', block, re.MULTILINE)
            return version.group(1) if version else None
    return None


def check_app(label: str, spec: dict[str, str]) -> list[str]:
    problems: list[str] = []
    tauri_path = REPO / spec["tauri"]
    npm_path = REPO / spec["npm"]
    cargo_path = REPO / spec["cargo"]

    reference = read_json_version(tauri_path)
    if reference is None:
        return [
            f"{label}: cannot read a version out of {spec['tauri']} — that file "
            f"names the release tag, so this check cannot pass without it."
        ]

    lock_path = REPO / spec["lock"]

    for kind, path, got in (
        ("npm", npm_path, read_json_version(npm_path)),
        ("cargo", cargo_path, read_cargo_version(cargo_path)),
        ("lock", lock_path, read_lock_version(lock_path, spec["crate"])),
    ):
        rel = path.relative_to(REPO)
        if got is None:
            if kind == "lock":
                problems.append(
                    f"{label}: no [[package]] named {spec['crate']!r} in {rel} — "
                    f"if the crate was renamed, update `crate` in this script."
                )
            else:
                problems.append(f"{label}: cannot read a version out of {rel}.")
            continue
        if got != reference:
            extra = ""
            if kind == "cargo":
                extra = (
                    " This is the version the app shows in its own footer "
                    "(CARGO_PKG_VERSION), so a mismatch here is what users report "
                    "back to you."
                )
            elif kind == "lock":
                extra = (
                    " Cargo rewrites this on the next build, so leaving it stale "
                    "means a surprise one-line diff — or a release job that finds "
                    "a dirty tree. Run `cargo update -w` in that crate, or edit it."
                )
            problems.append(
                f"{label}: {rel} says {got}, but {spec['tauri']} says "
                f"{reference} — and the release tag comes from the latter."
                f"{extra} Set all four to the same version."
            )

    if not problems:
        print(f"  {label}: {reference} in all four files ✓")
    return problems


def main() -> int:
    problems: list[str] = []
    print(
        "App version agreement "
        "(tauri.conf.json = package.json = Cargo.toml = Cargo.lock):"
    )
    for label, spec in APPS.items():
        problems.extend(check_app(label, spec))

    if problems:
        print()
        for p in problems:
            print(f"::error::{p}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
