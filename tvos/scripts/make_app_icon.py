#!/usr/bin/env python3
"""Generate the Witness Wall's tvOS asset catalog — the birdfeeder icon.

Apple TV will not accept an archive without an app icon and top-shelf art, and
tvOS icons are not one PNG: the App Icon is a *layered image stack* (Back /
Middle / Front) which the focus engine parallaxes when the icon is selected,
in two sizes, plus two top-shelf images each at @1x and @2x. That is ~20 files
and a tree of Contents.json — the kind of thing that rots when hand-maintained.

So it is generated and committed, the same contract the website uses for its
glTF models: the generator is the source of truth, the output is committed so
the build needs no Python, and `tvos.yml` checks the result.

## The picture

A birdfeeder, with our standard Canary perched on it. The bird is not redrawn
here — it is `brands/logo_512x512.png`, the same mascot the site and the app
icons use, composited in. One bird, one source.

The three layers are chosen for the parallax rather than for convenience:

    Back    the sky — a warm dawn behind the deep "calm room" navy
    Middle  the feeder — roof, hanger, hopper, tray, perch
    Front   the Canary

so that tilting the Apple TV remote slides the bird against the feeder and the
feeder against the sky. It reads as depth, not as a sticker.

Why a feeder: the Wall is where your fleet comes home to be seen. A feeder is
the one object that means exactly that, and it is legible at 400x240.

    python3 tvos/scripts/make_app_icon.py

Needs Pillow (`pip install pillow`). It is a build-time-only dependency — the
committed PNGs are what ships, so no runner needs it to build the app.
"""

from __future__ import annotations

import json
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
CATALOG = os.path.join(HERE, "..", "WitnessWall", "Support", "Assets.xcassets")
BIRD = os.path.join(REPO, "brands", "logo_512x512.png")
BRAND = "App Icon & Top Shelf Image"

# The Wall's own palette (WallView.swift) plus the wood tones the site uses for
# printed enclosures ("Walnut #6b4a2b").
NAVY = (11, 17, 32)
NAVY_DEEP = (6, 10, 20)
DAWN = (61, 46, 28)
CANARY = (245, 179, 1)
WOOD = (138, 106, 74)
WOOD_DARK = (107, 74, 43)
WOOD_LIGHT = (170, 134, 96)
SEED = (214, 178, 116)

# Supersample everything, then downscale once. Cheap, and it is the difference
# between clean edges and a staircase on a 55" screen.
SS = 3


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def vertical_gradient(w: int, h: int, top, bottom) -> Image.Image:
    """A gradient, drawn a row at a time — h draw calls, not w*h."""
    img = Image.new("RGBA", (w, h))
    draw = ImageDraw.Draw(img)
    for y in range(h):
        draw.line([(0, y), (w, y)], fill=(*lerp(top, bottom, y / max(1, h - 1)), 255))
    return img


# ── Back: the sky ───────────────────────────────────────────────────────────


def layer_back(w: int, h: int) -> Image.Image:
    img = vertical_gradient(w, h, NAVY, NAVY_DEEP)
    # A low, warm glow where the feeder stands — dawn behind the room.
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)
    cx, cy = w / 2, h * 0.62
    radius = max(w, h) * 0.42
    steps = 48
    for i in range(steps, 0, -1):
        t = i / steps
        r = radius * t
        alpha = round(46 * (1 - t) ** 1.5)
        if alpha <= 0:
            continue
        draw.ellipse([cx - r, cy - r * 0.72, cx + r, cy + r * 0.72], fill=(*DAWN, alpha))
    return Image.alpha_composite(img, glow)


# ── Middle: the feeder ──────────────────────────────────────────────────────


def layer_feeder(w: int, h: int) -> Image.Image:
    """A hopper feeder: hanger, pitched roof, seed hopper, tray, perch.

    Drawn from proportions of the canvas so it composes identically at every
    size the catalog needs.
    """
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    cx = w / 2
    unit = min(w, h)          # scale from the SHORT side so the wide top-shelf
                              # gets a feeder of sensible size, not a stretched one
    roof_half = unit * 0.37
    roof_top = h * 0.11
    roof_bottom = h * 0.37
    hopper_half = unit * 0.205
    tray_top = h * 0.79
    tray_half = unit * 0.325
    thickness = max(2, int(unit * 0.028))

    # Hanger: a wire up to the top edge, with a ring.
    d.line([(cx, 0), (cx, roof_top + unit * 0.01)], fill=(*WOOD_LIGHT, 255), width=max(2, thickness // 2))
    ring = unit * 0.035
    d.ellipse([cx - ring, roof_top - ring * 2.1, cx + ring, roof_top + ring * 0.1],
              outline=(*WOOD_LIGHT, 255), width=max(2, thickness // 2))

    # Hopper (behind the roof, so the roof's eaves overlap it).
    d.rounded_rectangle(
        [cx - hopper_half, roof_bottom - unit * 0.02, cx + hopper_half, tray_top + unit * 0.005],
        radius=unit * 0.012, fill=(*WOOD_DARK, 255),
    )
    # Seed visible through the hopper's open face.
    d.rounded_rectangle(
        [cx - hopper_half * 0.62, roof_bottom + unit * 0.03,
         cx + hopper_half * 0.62, tray_top - unit * 0.03],
        radius=unit * 0.010, fill=(*SEED, 255),
    )

    # Roof: a pitched gable with a slight overhang, in two tones so the ridge
    # reads at small sizes.
    d.polygon([(cx, roof_top), (cx + roof_half, roof_bottom), (cx - roof_half, roof_bottom)],
              fill=(*WOOD, 255))
    d.polygon([(cx, roof_top), (cx + roof_half, roof_bottom), (cx, roof_bottom)],
              fill=(*WOOD_DARK, 255))
    d.line([(cx, roof_top), (cx, roof_bottom)], fill=(*WOOD_LIGHT, 170), width=max(1, thickness // 3))

    # Tray with a lip, and the perch the bird stands on.
    d.rounded_rectangle([cx - tray_half, tray_top, cx + tray_half, tray_top + thickness * 1.7],
                        radius=thickness * 0.5, fill=(*WOOD, 255))
    d.rounded_rectangle([cx - tray_half, tray_top - thickness * 0.9, cx - tray_half + thickness, tray_top + thickness],
                        radius=thickness * 0.3, fill=(*WOOD_LIGHT, 255))
    d.rounded_rectangle([cx + tray_half - thickness, tray_top - thickness * 0.9, cx + tray_half, tray_top + thickness],
                        radius=thickness * 0.3, fill=(*WOOD_LIGHT, 255))
    # A few seeds scattered on the tray — fixed positions, never random, so the
    # generator stays byte-reproducible.
    for fx in (-0.62, -0.34, 0.30, 0.55, 0.74):
        sx = cx + tray_half * fx
        d.ellipse([sx - thickness * 0.28, tray_top - thickness * 0.5,
                   sx + thickness * 0.28, tray_top + thickness * 0.1], fill=(*SEED, 255))
    return img


# ── Front: the standard Canary ──────────────────────────────────────────────


def layer_bird(w: int, h: int) -> Image.Image:
    """Our standard bird, perched on the feeder's tray.

    Composited from brands/logo_512x512.png rather than redrawn — there is one
    Canary, and this is it.
    """
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    if not os.path.exists(BIRD):
        raise SystemExit(
            f"the standard bird is missing at {BIRD}.\n"
            "The app icon composites the shared mascot rather than redrawing it."
        )
    bird = Image.open(BIRD).convert("RGBA")

    # Trim the logo's transparent margin so "height" means the bird, not the
    # padding around it — otherwise it floats above the perch.
    box = bird.getbbox()
    if box:
        bird = bird.crop(box)

    unit = min(w, h)
    target_h = int(unit * 0.55)
    target_w = max(1, round(bird.width * target_h / bird.height))
    bird = bird.resize((target_w, target_h), Image.LANCZOS)

    tray_top = h * 0.79
    # Off-center by a little, so the seed hopper stays visible past the bird —
    # a feeder the bird completely covers stops reading as a feeder. It also
    # gives the parallax somewhere to travel.
    x = round(w / 2 - target_w / 2 - unit * 0.07)
    # Feet just into the tray so it reads as standing on it, not hovering.
    y = round(tray_top - target_h + unit * 0.012)
    img.alpha_composite(bird, (x, y))
    return img


# ── catalog plumbing ────────────────────────────────────────────────────────

# Painting order (used for compositing the flat top-shelf image): back first.
LAYERS = [("Back", layer_back), ("Middle", layer_feeder), ("Front", layer_bird)]

# The imagestack's Contents.json order is the OPPOSITE: Apple lists layers
# FRONT-TO-BACK, and actool requires the LAST entry (the back plate) to be
# fully opaque. Writing the painting order here shipped an inverted parallax
# and an actool error ("the last image stack layer with content, 'Front',
# must be a fully opaque bitmap") — see check_app_icon.py, which now gates
# the order structurally.
STACK_ORDER = ["Front", "Middle", "Back"]

# (stack, base size, scales). The HOME-SCREEN icon needs every layer at @1x
# AND @2x — App Store Connect rejects the archive by name without the 2x
# ("missing an image for the background layer with a scale value of '2'",
# error 90709, and it says so only at upload validation, minutes after a
# green build). The App Store icon is 1280x768 @1x only, per Apple's spec.
STACKS = [("App Icon", 400, 240, (1, 2)), ("App Icon - App Store", 1280, 768, (1,))]
SHELVES = [("Top Shelf Image", 1920, 720), ("Top Shelf Image Wide", 2320, 720)]

INFO = {"version": 1, "author": "xcode"}


def render(make, w: int, h: int) -> Image.Image:
    """Render supersampled, then downscale once."""
    big = make(w * SS, h * SS)
    return big.resize((w, h), Image.LANCZOS)


def flatten(w: int, h: int) -> Image.Image:
    """All three layers composited — what the top shelf shows (it is one flat
    image; only the app icon is layered)."""
    out = render(layer_back, w, h)
    for _, make in LAYERS[1:]:
        out = Image.alpha_composite(out, render(make, w, h))
    return out


def write_json(path: str, payload: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


def save(img: Image.Image, path: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, "PNG", optimize=True)


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

    for name, w, h, scales in STACKS:
        stack = os.path.join(brand, f"{name}.imagestack")
        write_json(
            os.path.join(stack, "Contents.json"),
            {"info": INFO, "layers": [{"filename": f"{layer}.imagestacklayer"} for layer in STACK_ORDER]},
        )
        for layer, make in LAYERS:
            layer_dir = os.path.join(stack, f"{layer}.imagestacklayer")
            write_json(os.path.join(layer_dir, "Contents.json"), {"info": INFO})
            image_set = os.path.join(layer_dir, "Content.imageset")
            images = []
            for scale in scales:
                suffix = "" if scale == 1 else f"@{scale}x"
                png = f"{name.replace(' ', '_')}_{layer}{suffix}.png"
                images.append({"filename": png, "idiom": "tv", "scale": f"{scale}x"})
                save(render(make, w * scale, h * scale), os.path.join(image_set, png))
            write_json(
                os.path.join(image_set, "Contents.json"),
                {"images": images, "info": INFO},
            )

    for name, w, h in SHELVES:
        image_set = os.path.join(brand, f"{name}.imageset")
        images = []
        for scale in (1, 2):
            png = f"{name.replace(' ', '_')}@{scale}x.png"
            images.append({"filename": png, "idiom": "tv", "scale": f"{scale}x"})
            save(flatten(w * scale, h * scale), os.path.join(image_set, png))
        write_json(os.path.join(image_set, "Contents.json"), {"images": images, "info": INFO})

    print(f"wrote {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
