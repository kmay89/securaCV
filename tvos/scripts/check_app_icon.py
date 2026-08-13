#!/usr/bin/env python3
"""Verify the committed tvOS asset catalog is complete and correctly sized.

Apple rejects a tvOS archive with a missing or wrong-sized app icon, and it
reports that as an opaque `altool --validate-app` failure minutes into a
publish. This turns it into a named failure in CI, before anything is built.

Why this and not a byte-diff against `make_app_icon.py`'s output: the icon
composites `brands/logo_512x512.png` through Pillow, whose resampling can
differ between versions. A byte-diff would go red because a runner image
bumped a dependency — a spurious failure, and this pipeline is supposed to
never have those. What matters is that every image Apple requires exists at
exactly the size it requires, which is what this checks. It needs no Pillow
and no network: it reads PNG headers directly.

    python3 tvos/scripts/check_app_icon.py
"""

from __future__ import annotations

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BRAND = os.path.join(
    HERE, "..", "WitnessWall", "Support", "Assets.xcassets",
    "App Icon & Top Shelf Image.brandassets",
)

LAYERS = ("Back", "Middle", "Front")

# (stack folder, base size, scales) — every layer of a stack exists at every
# scale. The home-screen stack needs @1x AND @2x: App Store Connect rejects
# an archive whose layers lack the 2x (error 90709, "missing an image for the
# background layer with a scale value of '2'"), and it says so only at upload
# validation, minutes after a green build. The App Store stack is @1x only.
STACKS = [("App Icon", 400, 240, (1, 2)), ("App Icon - App Store", 1280, 768, (1,))]
# (imageset folder, base size) — @1x and @2x.
SHELVES = [("Top Shelf Image", 1920, 720), ("Top Shelf Image Wide", 2320, 720)]


def png_size(path: str) -> tuple[int, int]:
    """Width and height from a PNG's IHDR — no image library needed."""
    with open(path, "rb") as handle:
        header = handle.read(24)
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    return struct.unpack(">II", header[16:24])


def main() -> int:
    problems: list[str] = []
    root = os.path.normpath(BRAND)

    if not os.path.isdir(root):
        print(f"::error::the tvOS asset catalog is missing at {root}")
        print("Regenerate it with: python3 tvos/scripts/make_app_icon.py")
        return 1

    def check(path: str, want_w: int, want_h: int, label: str) -> None:
        if not os.path.exists(path):
            problems.append(f"{label}: missing ({os.path.relpath(path, root)})")
            return
        if os.path.getsize(path) == 0:
            problems.append(f"{label}: empty")
            return
        try:
            got_w, got_h = png_size(path)
        except (OSError, ValueError) as exc:
            problems.append(f"{label}: unreadable PNG ({exc})")
            return
        if (got_w, got_h) != (want_w, want_h):
            problems.append(f"{label}: is {got_w}x{got_h}, Apple requires {want_w}x{want_h}")

    for name, w, h, scales in STACKS:
        stack = os.path.join(root, f"{name}.imagestack")
        manifest = os.path.join(stack, "Contents.json")
        if not os.path.exists(manifest):
            problems.append(f"{name}: no Contents.json — the layer stack is not declared")
            continue
        with open(manifest, encoding="utf-8") as handle:
            declared = [layer["filename"] for layer in json.load(handle).get("layers", [])]
        for layer in LAYERS:
            if f"{layer}.imagestacklayer" not in declared:
                problems.append(f"{name}: the {layer} layer is not declared in Contents.json")
        # Apple lists imagestack layers FRONT-TO-BACK, and actool requires the
        # LAST entry — the back plate — to be fully opaque. The painting order
        # (Back first) shipped here once and produced both an inverted parallax
        # and "the last image stack layer with content, 'Front', must be a
        # fully opaque bitmap" at build time.
        want_order = ["Front.imagestacklayer", "Middle.imagestacklayer", "Back.imagestacklayer"]
        if declared and declared != want_order:
            problems.append(
                f"{name}: layers are declared {declared} — Apple's imagestack "
                f"order is front-to-back, so it must be {want_order}"
            )
        # Every layer, at every scale. This loop used to sit INSIDE the
        # wrong-order branch above (one indentation level too deep), so the
        # per-layer size checks only ran when the catalog was ALREADY broken
        # a different way — which is how a stack with no @2x sailed through
        # CI and died at App Store Connect validation (90709).
        for layer in LAYERS:
            image_set = os.path.join(stack, f"{layer}.imagestacklayer", "Content.imageset")
            for scale in scales:
                suffix = "" if scale == 1 else f"@{scale}x"
                png = os.path.join(image_set, f"{name.replace(' ', '_')}_{layer}{suffix}.png")
                check(png, w * scale, h * scale, f"{name} / {layer} @{scale}x")
            # The manifest is load-bearing, not optional: a PNG on disk that
            # Contents.json does not declare (or declares under another
            # filename) fills no slot, and actool/App Store validation fails
            # on the "missing" scale while every expected file exists. So a
            # missing manifest is a failure, and the comparison covers the
            # whole declaration — filename, idiom, scale — not just scales.
            layer_manifest = os.path.join(image_set, "Contents.json")
            if not os.path.exists(layer_manifest):
                problems.append(
                    f"{name} / {layer}: Content.imageset has no Contents.json — "
                    "its PNGs fill no slots without one"
                )
            else:
                with open(layer_manifest, encoding="utf-8") as handle:
                    declared = sorted(
                        (image.get("filename", ""), image.get("idiom", ""), image.get("scale", ""))
                        for image in json.load(handle).get("images", [])
                    )
                want = sorted(
                    (
                        f"{name.replace(' ', '_')}_{layer}{'' if scale == 1 else f'@{scale}x'}.png",
                        "tv",
                        f"{scale}x",
                    )
                    for scale in scales
                )
                if declared != want:
                    problems.append(
                        f"{name} / {layer}: Contents.json declares {declared}, "
                        f"Apple requires {want}"
                    )

    for name, w, h in SHELVES:
        for scale in (1, 2):
            png = os.path.join(root, f"{name}.imageset", f"{name.replace(' ', '_')}@{scale}x.png")
            check(png, w * scale, h * scale, f"{name} @{scale}x")

    if problems:
        print("::error::the tvOS asset catalog is incomplete — App Store Connect would reject the archive.")
        for problem in problems:
            print(f"  · {problem}")
        print("\nRegenerate it with: python3 tvos/scripts/make_app_icon.py")
        return 1

    total = sum(len(LAYERS) * len(scales) for *_rest, scales in STACKS) + len(SHELVES) * 2
    print(f"tvOS asset catalog complete — {total} images, all at the sizes Apple requires. ✅")
    return 0


if __name__ == "__main__":
    sys.exit(main())
