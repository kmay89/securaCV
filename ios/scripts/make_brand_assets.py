#!/usr/bin/env python3
"""Stage the standard Canary mascot into the app asset catalogs.

The bird is composited from brands/logo_512x512.png — the ONE canonical
mascot, never redrawn (the same rule the app icons follow) — trimmed to its
bounding box and written into an imageset in the iPhone catalog AND the
watch catalog, so both apps can show the character in empty states and calm
moments. Generated and committed; deterministic.

    python3 ios/scripts/make_brand_assets.py

Needs Pillow (build-time only; the committed PNGs are what ship)."""

from __future__ import annotations

import json
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
BIRD = os.path.join(REPO, "brands", "logo_512x512.png")

CATALOGS = [
    os.path.normpath(os.path.join(HERE, "..", "Assets.xcassets")),
    os.path.normpath(os.path.join(HERE, "..", "WatchAssets.xcassets")),
    # The Witness Wall stages the same character (CanaryActor is compiled
    # into the tvOS target), so its catalog carries the same imageset —
    # generated here, never redrawn, byte-identical across all three.
    os.path.normpath(
        os.path.join(HERE, "..", "..", "tvos", "WitnessWall", "Support", "Assets.xcassets")
    ),
]

INFO = {"version": 1, "author": "xcode"}


def main() -> int:
    if not os.path.exists(BIRD):
        raise SystemExit(f"the standard bird is missing at {BIRD}")
    bird = Image.open(BIRD).convert("RGBA")
    box = bird.getbbox()
    if box:
        bird = bird.crop(box)

    for catalog in CATALOGS:
        imageset = os.path.join(catalog, "Canary.imageset")
        os.makedirs(imageset, exist_ok=True)
        with open(os.path.join(imageset, "Contents.json"), "w", encoding="utf-8") as handle:
            json.dump(
                {"images": [{"idiom": "universal", "filename": "Canary.png"}],
                 "info": INFO},
                handle, indent=2)
            handle.write("\n")
        bird.save(os.path.join(imageset, "Canary.png"), "PNG", optimize=True)
        print(f"wrote {imageset}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
