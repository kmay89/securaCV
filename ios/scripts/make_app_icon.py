#!/usr/bin/env python3
"""Generate the iPhone AND watch app icon catalogs — the standard Canary at dawn.

App Store Connect refuses an .ipa without an app icon — and since the watch
app ships inside the iPhone .ipa, a missing WATCH icon fails the same upload.
Ship blockers, not decoration. Same contract as the tvOS icon
(tvos/scripts/make_app_icon.py) and the website's glTF models: the generator
is the source of truth, the output is committed so no runner needs Python,
and it is deterministic — no randomness, no timestamps.
`scripts/check_app_icon.py` structurally verifies the committed output in CI.

Both platforms are single-size (iOS since Xcode 14; watchOS likewise): one
1024x1024 opaque PNG each, and the OS masks its own shape — rounded rect on
the phone, circle on the watch (never pre-round, never add alpha — App Store
validation rejects transparency). The picture is the Witness Wall icon's calm
sibling: the "calm room" navy, a warm dawn glow rising behind the perch, and
the one standard Canary (brands/logo_512x512.png — composited, never redrawn).
The composition is centered, so the watch's circular mask crops only sky and
perch ends.

    python3 ios/scripts/make_app_icon.py

Needs Pillow (build-time only; the committed PNGs are what ship).
"""

from __future__ import annotations

import json
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
# (catalog path, asset-catalog `platform` value) — one entry per Apple surface
# this project ships an icon for. The watch catalog is separate because the
# watch app is its own target with its own ASSETCATALOG_COMPILER_APPICON_NAME.
CATALOGS = [
    (os.path.normpath(os.path.join(HERE, "..", "Assets.xcassets")), "ios"),
    (os.path.normpath(os.path.join(HERE, "..", "WatchAssets.xcassets")), "watchos"),
]
BIRD = os.path.join(REPO, "brands", "logo_512x512.png")

# The Wall's palette (tvos WallView.swift) and the shared wood tones.
NAVY = (11, 17, 32)
NAVY_DEEP = (6, 10, 20)
DAWN = (61, 46, 28)
WOOD = (138, 106, 74)
WOOD_LIGHT = (170, 134, 96)

SIZE = 1024
SS = 3  # supersample, then downscale once — clean edges on the 1024 master

INFO = {"version": 1, "author": "xcode"}


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def backdrop(w: int, h: int) -> Image.Image:
    """Navy gradient with the low dawn glow — same sky as the Wall's icon."""
    img = Image.new("RGBA", (w, h))
    draw = ImageDraw.Draw(img)
    for y in range(h):
        draw.line([(0, y), (w, y)], fill=(*lerp(NAVY, NAVY_DEEP, y / max(1, h - 1)), 255))
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    cx, cy = w / 2, h * 0.66
    radius = w * 0.48
    steps = 48
    for i in range(steps, 0, -1):
        t = i / steps
        r = radius * t
        alpha = round(80 * (1 - t) ** 1.5)
        if alpha <= 0:
            continue
        d.ellipse([cx - r, cy - r * 0.72, cx + r, cy + r * 0.72], fill=(*DAWN, alpha))
    return Image.alpha_composite(img, glow)


def compose(size: int) -> Image.Image:
    w = h = size
    img = backdrop(w, h)
    d = ImageDraw.Draw(img)

    # The perch: one calm bar, a nod to the feeder without its busyness at
    # home-screen sizes.
    perch_y = h * 0.78
    perch_half = w * 0.35
    thickness = max(2, int(h * 0.022))
    d.rounded_rectangle(
        [w / 2 - perch_half, perch_y, w / 2 + perch_half, perch_y + thickness * 1.6],
        radius=thickness * 0.6, fill=(*WOOD, 255),
    )
    d.rounded_rectangle(
        [w / 2 - perch_half, perch_y, w / 2 + perch_half, perch_y + thickness * 0.5],
        radius=thickness * 0.25, fill=(*WOOD_LIGHT, 200),
    )

    # The one standard Canary, feet just into the perch so it stands, not
    # hovers (same trim-then-place move as the tvOS generator).
    if not os.path.exists(BIRD):
        raise SystemExit(
            f"the standard bird is missing at {BIRD}.\n"
            "The app icon composites the shared mascot rather than redrawing it."
        )
    bird = Image.open(BIRD).convert("RGBA")
    box = bird.getbbox()
    if box:
        bird = bird.crop(box)
    target_h = int(h * 0.56)
    target_w = max(1, round(bird.width * target_h / bird.height))
    bird = bird.resize((target_w, target_h), Image.LANCZOS)
    x = round(w / 2 - target_w / 2)
    y = round(perch_y - target_h + h * 0.012)
    img.alpha_composite(bird, (x, y))
    return img


def write_json(path: str, payload: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


def main() -> int:
    big = compose(SIZE * SS).resize((SIZE, SIZE), Image.LANCZOS)
    icon = Image.new("RGB", (SIZE, SIZE), NAVY)     # opaque: ASC rejects alpha
    icon.paste(big, mask=big.split()[3])

    for catalog, platform in CATALOGS:
        write_json(os.path.join(catalog, "Contents.json"), {"info": INFO})
        iconset = os.path.join(catalog, "AppIcon.appiconset")
        write_json(
            os.path.join(iconset, "Contents.json"),
            {
                "images": [
                    {"filename": "AppIcon.png", "idiom": "universal",
                     "platform": platform, "size": "1024x1024"}
                ],
                "info": INFO,
            },
        )
        os.makedirs(iconset, exist_ok=True)
        icon.save(os.path.join(iconset, "AppIcon.png"), "PNG", optimize=True)
        print(f"wrote {catalog}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
