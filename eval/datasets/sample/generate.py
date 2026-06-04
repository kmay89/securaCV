#!/usr/bin/env python3
"""Generate the synthetic, privacy-clean sample eval dataset.

The perception eval harness needs *some* labeled frames to run against, but real surveillance
imagery must never live in the repo (it would itself be a privacy/PII problem and contradict the
project's whole premise). So this dataset is fully synthetic: solid-colored rectangles on a noisy
background, drawn at known normalized coordinates that exactly match the labels in labels.json.

It is deliberately dependency-free (Python stdlib only: zlib + struct), so it regenerates the
PNGs anywhere — including CI — without Pillow/numpy. Run it from anywhere:

    python3 eval/datasets/sample/generate.py

NOTE: a real detector will not "see" a person in a colored box. This dataset exercises the
harness end-to-end and gives deterministic plumbing; for a real measurement, point the harness at
your own locally-held labeled dataset (see eval/README.md).
"""

import json
import os
import struct
import zlib

WIDTH = 96
HEIGHT = 96

# (filename, background_rgb, objects)
# objects: list of (class, color_rgb, nx, ny, nw, nh) in normalized 0..1 coords.
FRAMES = [
    (
        "frame_0001.png",
        (30, 30, 40),
        [("person", (220, 180, 140), 0.25, 0.20, 0.50, 0.60)],  # area 0.30 -> large
    ),
    (
        "frame_0002.png",
        (40, 35, 30),
        [("package", (200, 160, 60), 0.62, 0.62, 0.15, 0.15)],  # area 0.0225 -> small
    ),
    (
        "frame_0003.png",
        (25, 35, 45),
        [("vehicle", (80, 90, 200), 0.10, 0.55, 0.55, 0.28)],  # area 0.154 -> small
    ),
    (
        "frame_0004.png",
        (35, 35, 35),
        [],  # empty scene: no events should be witnessed
    ),
]


def render(bg, objects):
    """Return a HEIGHT x WIDTH list of (r, g, b) with a cheap deterministic noise + boxes."""
    pixels = [[bg for _ in range(WIDTH)] for _ in range(HEIGHT)]
    # Deterministic checkerboard-ish noise so frames are not perfectly flat.
    for y in range(HEIGHT):
        for x in range(WIDTH):
            n = ((x * 7 + y * 13) % 17) - 8
            r, g, b = bg
            pixels[y][x] = (
                max(0, min(255, r + n)),
                max(0, min(255, g + n)),
                max(0, min(255, b + n)),
            )
    for _cls, color, nx, ny, nw, nh in objects:
        x0 = int(nx * WIDTH)
        y0 = int(ny * HEIGHT)
        x1 = min(WIDTH, int((nx + nw) * WIDTH))
        y1 = min(HEIGHT, int((ny + nh) * HEIGHT))
        for y in range(y0, y1):
            for x in range(x0, x1):
                pixels[y][x] = color
    return pixels


def write_png(path, pixels):
    """Minimal RGB PNG encoder (stdlib only)."""

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b) in row:
            raw += bytes((r, g, b))

    ihdr = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)  # 8-bit, color type 2 (RGB)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def size_class(objects):
    if not objects:
        return None
    area = max(o[4] * o[5] for o in objects)
    return "large" if area >= 0.2 else "small"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    frames_json = []
    for name, bg, objects in FRAMES:
        write_png(os.path.join(here, name), render(bg, objects))
        entry = {
            "image": name,
            "objects": [
                {"class": c, "x": nx, "y": ny, "w": nw, "h": nh}
                for (c, _color, nx, ny, nw, nh) in objects
            ],
        }
        sc = size_class(objects)
        if sc is not None:
            entry["expected_size_class"] = sc
        frames_json.append(entry)

    with open(os.path.join(here, "labels.json"), "w") as f:
        json.dump({"frames": frames_json}, f, indent=2)
        f.write("\n")
    print(f"Wrote {len(FRAMES)} frames + labels.json to {here}")


if __name__ == "__main__":
    main()
