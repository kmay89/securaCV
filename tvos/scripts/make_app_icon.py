#!/usr/bin/env python3
"""Generate the Witness Wall's tvOS asset catalog.

Apple TV will not accept an archive without an app icon and top-shelf art, and
tvOS icons are not one PNG: the App Icon is a *layered image stack* (Back /
Middle / Front, parallaxed by the focus engine), in two sizes, plus two
top-shelf images each at @1x and @2x. That is ~20 files and a tree of
Contents.json — exactly the kind of thing that rots when hand-maintained.

So it is generated and committed, the same contract the website uses for its
glTF models: the generator is the source of truth, the output is committed so
the build needs no Python, and CI regenerates and fails if the bytes drift
(`tvos.yml`). Edit THIS file, never the .xcassets.

The art is deliberately simple and brand-true rather than illustrative: the
Canary yellow on the deep "calm room" navy the Wall itself renders on, with a
single ring mark. It is a real, submittable icon — not a placeholder that says
"TODO" — but it is geometry, not craft, and a designer should replace it before
a public launch. It is honest either way: it never claims to be finished art.

    python3 tvos/scripts/make_app_icon.py

No third-party dependencies (no Pillow): PNGs are written with zlib + struct,
so this runs on a bare runner.
"""

from __future__ import annotations

import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG = os.path.join(HERE, "..", "WitnessWall", "Support", "Assets.xcassets")
BRAND = "App Icon & Top Shelf Image"

# The Wall's own palette (WallView.swift + the site's apple-tv page).
NAVY = (11, 17, 32)
NAVY_DEEP = (6, 10, 20)
CANARY = (245, 179, 1)
INK = (9, 9, 11)


def write_png(path: str, width: int, height: int, pixel) -> None:
    """Write an 8-bit RGBA PNG. `pixel(x, y) -> (r, g, b, a)`."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter type 0 (None) for each scanline
        for x in range(width):
            r, g, b, a = pixel(x, y)
            rows += bytes((r, g, b, a))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(png)


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


# ── the three parallax layers ───────────────────────────────────────────────
# Back: the room. Middle: the light. Front: the mark. The focus engine slides
# them against each other, so each layer must read on its own.


def back(w: int, h: int):
    def pixel(x, y):
        return (*lerp(NAVY, NAVY_DEEP, y / max(1, h - 1)), 255)

    return pixel


def middle(w: int, h: int):
    cx, cy = w / 2, h / 2
    radius = min(w, h) * 0.62

    def pixel(x, y):
        d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / radius
        glow = max(0.0, 1.0 - d) ** 2
        return (*CANARY, round(150 * glow))

    return pixel


def front(w: int, h: int):
    """A single ring — 'watching without watching': an open eye that is not a lens."""
    cx, cy = w / 2, h / 2
    outer = min(w, h) * 0.30
    inner = outer * 0.66
    dot = outer * 0.17

    def pixel(x, y):
        d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
        edge = max(1.0, min(w, h) * 0.004)  # keep the curve smooth at every size
        if inner - edge <= d <= outer + edge:
            a = 255
            if d > outer:
                a = round(255 * (1 - (d - outer) / edge))
            elif d < inner:
                a = round(255 * (1 - (inner - d) / edge))
            return (*CANARY, max(0, min(255, a)))
        if d <= dot:
            return (*CANARY, 255)
        return (*INK, 0)

    return pixel


LAYERS = [("Back", back), ("Middle", middle), ("Front", front)]

# App Icon stacks: (folder, width, height). tvOS wants both sizes.
STACKS = [
    ("App Icon", 400, 240),
    ("App Icon - App Store", 1280, 768),
]

# Top shelf art: (folder, base width, base height). Each needs @1x and @2x.
SHELVES = [
    ("Top Shelf Image", 1920, 720),
    ("Top Shelf Image Wide", 2320, 720),
]


def shelf(w: int, h: int):
    """Wide art: the room, the light, and the ring — one calm frame.

    The glow and the mark are built on a SQUARE canvas of side `h` and then
    composited into the middle of the wide frame. Building them at (w, h) and
    sampling them with square coordinates is the bug this comment exists to
    stop coming back: it put the ring off the right-hand edge, almost invisible.
    """
    square = h
    b = back(w, h)
    m, f = middle(square, square), front(square, square)
    left = (w - square) / 2  # x of the square's left edge, centred

    def pixel(x, y):
        r, g, bl, _ = b(x, y)
        sx = round(x - left)
        if 0 <= sx < square:
            for layer in (m, f):
                lr, lg, lb, la = layer(sx, y)
                if la:
                    t = la / 255
                    r = round(r + (lr - r) * t)
                    g = round(g + (lg - g) * t)
                    bl = round(bl + (lb - bl) * t)
        return (r, g, bl, 255)

    return pixel


def write_json(path: str, payload: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


INFO = {"version": 1, "author": "xcode"}


def main() -> int:
    root = os.path.normpath(CATALOG)
    write_json(os.path.join(root, "Contents.json"), {"info": INFO})

    brand = os.path.join(root, f"{BRAND}.brandassets")
    write_json(
        os.path.join(brand, "Contents.json"),
        {
            "assets": [
                {"filename": "App Icon.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "400x240"},
                {"filename": "App Icon - App Store.imagestack", "idiom": "tv", "role": "primary-app-icon", "size": "1280x768"},
                {"filename": "Top Shelf Image.imageset", "idiom": "tv", "role": "top-shelf-image", "size": "1920x720"},
                {"filename": "Top Shelf Image Wide.imageset", "idiom": "tv", "role": "top-shelf-image-wide", "size": "2320x720"},
            ],
            "info": INFO,
        },
    )

    for name, w, h in STACKS:
        stack = os.path.join(brand, f"{name}.imagestack")
        write_json(
            os.path.join(stack, "Contents.json"),
            {"info": INFO, "layers": [{"filename": f"{layer}.imagestacklayer"} for layer, _ in LAYERS]},
        )
        for layer, make in LAYERS:
            layer_dir = os.path.join(stack, f"{layer}.imagestacklayer")
            write_json(os.path.join(layer_dir, "Contents.json"), {"info": INFO})
            image_set = os.path.join(layer_dir, "Content.imageset")
            png = f"{name.replace(' ', '_')}_{layer}.png"
            write_json(
                os.path.join(image_set, "Contents.json"),
                {"images": [{"filename": png, "idiom": "tv", "scale": "1x"}], "info": INFO},
            )
            write_png(os.path.join(image_set, png), w, h, make(w, h))

    for name, w, h in SHELVES:
        image_set = os.path.join(brand, f"{name}.imageset")
        images = []
        for scale in (1, 2):
            png = f"{name.replace(' ', '_')}@{scale}x.png"
            images.append({"filename": png, "idiom": "tv", "scale": f"{scale}x"})
            write_png(os.path.join(image_set, png), w * scale, h * scale, shelf(w * scale, h * scale))
        write_json(os.path.join(image_set, "Contents.json"), {"images": images, "info": INFO})

    print(f"wrote {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
