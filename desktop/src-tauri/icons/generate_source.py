#!/usr/bin/env python3
"""Build the SecuraCV Flasher app-icon source (1024x1024 RGBA) from the
official brand mascot.

Composites `brands/logo_main.png` (the note-taking Canary) onto a deep-navy
brand squircle with a soft canary glow and a drop shadow — a proper
macOS-style badge. CI then runs `tauri icon source.png` to derive the
.icns/.ico/png set the bundler needs.

Regenerate with:  python3 generate_source.py   (needs Pillow + the brand asset)
The produced `source.png` is committed, so CI itself needs no Pillow.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
# icons -> src-tauri -> desktop -> repo root -> brands/
MASCOT = os.path.normpath(os.path.join(HERE, "../../../brands/logo_main.png"))

N = 1024
RADIUS = 224            # squircle corner radius
PAD_FRAC = 0.06         # mascot inset from the badge edge

# Brand palette (from the site theme): ink navy + canary.
INK_TOP = (18, 27, 51)     # #121b33
INK_BOTTOM = (6, 11, 24)   # #060b18
CANARY = (245, 179, 1)     # #f5b301


def vertical_gradient(size, top, bottom):
    w, h = size
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        grad.putpixel(
            (0, y),
            tuple(round(top[i] * (1 - t) + bottom[i] * t) for i in range(3)),
        )
    return grad.resize((w, h))


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius, fill=255)
    return m


def main():
    # ── badge background: navy gradient clipped to a squircle ──────────────
    badge = vertical_gradient((N, N), INK_TOP, INK_BOTTOM).convert("RGBA")

    # Soft canary glow behind where the mascot sits (adds depth/warmth).
    glow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([N * 0.14, N * 0.10, N * 0.86, N * 0.82], fill=CANARY + (70,))
    glow = glow.filter(ImageFilter.GaussianBlur(90))
    badge = Image.alpha_composite(badge, glow)

    # ── the mascot ─────────────────────────────────────────────────────────
    mascot = Image.open(MASCOT).convert("RGBA")
    bbox = mascot.getbbox()          # trim the transparent margin
    if bbox:
        mascot = mascot.crop(bbox)

    avail = int(N * (1 - 2 * PAD_FRAC))
    scale = min(avail / mascot.width, avail / mascot.height)
    mw, mh = round(mascot.width * scale), round(mascot.height * scale)
    mascot = mascot.resize((mw, mh), Image.LANCZOS)

    ox = (N - mw) // 2
    oy = (N - mh) // 2 - int(N * 0.01)   # nudge up a touch; the Canary has feet

    # Drop shadow: a blurred dark silhouette of the mascot, offset down.
    alpha = mascot.split()[3]
    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 150), (ox, oy + 26), alpha)
    shadow = shadow.filter(ImageFilter.GaussianBlur(24))

    out = Image.alpha_composite(badge, shadow)
    out.paste(mascot, (ox, oy), mascot)

    # Clip everything to the squircle so the corners stay transparent.
    out.putalpha(rounded_mask((N, N), RADIUS))

    dst = os.path.join(HERE, "source.png")
    out.save(dst)
    print("wrote", dst, os.path.getsize(dst), "bytes")


if __name__ == "__main__":
    main()
