#!/usr/bin/env python3
"""Verify the committed iPhone + watch icon catalogs are complete and correct.

App Store Connect rejects an .ipa whose app — or whose EMBEDDED WATCH APP —
lacks a proper icon, and reports it as an opaque validation failure minutes
into a publish. This turns that into a named failure in PR CI, before
anything is built. Same posture as tvos/scripts/check_app_icon.py, and the
same deliberate design: structural checks, NOT a byte-diff against a
regenerated copy — Pillow's resampling may differ between versions, and a
gate that goes red because a runner image bumped a dependency trains people
to ignore it. What matters is what Apple checks: the image exists, at the
exact size, with no alpha. Needs no Pillow and no network — it reads PNG
headers directly.

    python3 ios/scripts/check_app_icon.py
"""

from __future__ import annotations

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IOS_DIR = os.path.normpath(os.path.join(HERE, ".."))

# (catalog, declared platform) — must match make_app_icon.py's CATALOGS.
CATALOGS = [
    (os.path.join(IOS_DIR, "Assets.xcassets"), "ios"),
    (os.path.join(IOS_DIR, "WatchAssets.xcassets"), "watchos"),
]

SIZE = 1024
# PNG color types that carry an alpha channel — ASC rejects transparency.
ALPHA_COLOR_TYPES = {4, 6}


def png_header(path: str) -> tuple[int, int, int]:
    """(width, height, color_type) from a PNG's IHDR — no image library."""
    with open(path, "rb") as handle:
        header = handle.read(26)
    if len(header) < 26 or header[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    width, height = struct.unpack(">II", header[16:24])
    color_type = header[25]
    return width, height, color_type


def check_catalog(catalog: str, platform: str, problems: list[str]) -> None:
    label = os.path.relpath(catalog, IOS_DIR)
    iconset = os.path.join(catalog, "AppIcon.appiconset")
    manifest = os.path.join(iconset, "Contents.json")

    if not os.path.isfile(manifest):
        problems.append(f"{label}: AppIcon.appiconset/Contents.json is missing")
        return
    with open(manifest, encoding="utf-8") as handle:
        images = json.load(handle).get("images", [])

    declared = [img for img in images if img.get("platform") == platform]
    if not declared:
        problems.append(f"{label}: no image declared for platform '{platform}'")
        return
    for img in declared:
        if img.get("size") != f"{SIZE}x{SIZE}":
            problems.append(f"{label}: declared size {img.get('size')!r}, "
                            f"Apple requires {SIZE}x{SIZE}")
        filename = img.get("filename")
        if not filename:
            problems.append(f"{label}: declared image has no filename")
            continue
        path = os.path.join(iconset, filename)
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            problems.append(f"{label}: {filename} missing or empty")
            continue
        try:
            width, height, color_type = png_header(path)
        except (OSError, ValueError) as exc:
            problems.append(f"{label}: {filename} unreadable PNG ({exc})")
            continue
        if (width, height) != (SIZE, SIZE):
            problems.append(f"{label}: {filename} is {width}x{height}, "
                            f"Apple requires {SIZE}x{SIZE}")
        if color_type in ALPHA_COLOR_TYPES:
            problems.append(f"{label}: {filename} carries an alpha channel — "
                            "App Store validation rejects transparency")


def main() -> int:
    problems: list[str] = []
    for catalog, platform in CATALOGS:
        check_catalog(catalog, platform, problems)
    if problems:
        for problem in problems:
            print(f"::error::{problem}")
        print("Regenerate with: python3 ios/scripts/make_app_icon.py")
        return 1
    print(f"app icon catalogs OK ({', '.join(p for _, p in CATALOGS)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
