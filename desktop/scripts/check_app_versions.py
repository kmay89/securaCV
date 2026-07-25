#!/usr/bin/env python3
"""Each desktop app states its version in three files. They must agree.

A Tauri app carries its version in three places, and each one is authoritative
for something different:

    src-tauri/tauri.conf.json   the BUNDLE version — and what the release
                                workflows derive the release TAG from
    package.json                the npm package version
    src-tauri/Cargo.toml        CARGO_PKG_VERSION — what the running app
                                REPORTS TO THE USER in its footer

Nothing keeps them in step, and they drifted: the Flasher shipped as
`flasher-v0.2.2` while its own window footer read `v0.1.0`, because the footer
comes from Cargo.toml (`lib.rs`: `env!("CARGO_PKG_VERSION")`) and only the other
two had ever been bumped. A user reporting a bug from that build would name a
version that was never released, and the maintainer would look for it in the
wrong tree.

An earlier version of this check compared two of the three files — which is
worse than useless, because it *reads* as covered while leaving the number
humans actually see unguarded. So: all three, every app, one script.

Run locally:  python3 desktop/scripts/check_app_versions.py
CI:           lint.yml, and both app release workflows before they build
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# app label -> the three files, each with how to read the version out of it.
APPS: dict[str, dict[str, str]] = {
    "SecuraCV Flasher": {
        "dir": "desktop",
        # The release tag comes from this one, so it is the reference the others
        # are compared against.
        "tauri": "desktop/src-tauri/tauri.conf.json",
        "npm": "desktop/package.json",
        "cargo": "desktop/src-tauri/Cargo.toml",
    },
    "SecuraCV Lab": {
        "dir": "desktop-lab",
        "tauri": "desktop-lab/src-tauri/tauri.conf.json",
        "npm": "desktop-lab/package.json",
        "cargo": "desktop-lab/src-tauri/Cargo.toml",
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

    for kind, path, got in (
        ("npm", npm_path, read_json_version(npm_path)),
        ("cargo", cargo_path, read_cargo_version(cargo_path)),
    ):
        rel = path.relative_to(REPO)
        if got is None:
            problems.append(f"{label}: cannot read a version out of {rel}.")
        elif got != reference:
            extra = (
                " This is the version the app shows in its own footer "
                "(CARGO_PKG_VERSION), so a mismatch here is what users report "
                "back to you."
                if kind == "cargo"
                else ""
            )
            problems.append(
                f"{label}: {rel} says {got}, but {spec['tauri']} says "
                f"{reference} — and the release tag comes from the latter."
                f"{extra} Set all three to the same version."
            )

    if not problems:
        print(f"  {label}: {reference} in all three files ✓")
    return problems


def main() -> int:
    problems: list[str] = []
    print("App version agreement (tauri.conf.json = package.json = Cargo.toml):")
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
