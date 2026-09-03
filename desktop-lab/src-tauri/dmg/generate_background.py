#!/usr/bin/env python3
"""Build the branded .dmg window background for the SecuraCV Lab installer.

The star of the bottom panel is the **Terminal one-liner** that clears macOS's
quarantine flag — because on recent macOS (Sequoia+) right-click → Open no
longer offers a bypass, so the command is the reliable path, not a fallback.

Layout is deliberately COLLISION-PROOF: Finder places the two icons (the app
and the Applications alias) roughly in the vertical middle, and their exact Y
can't be pixel-tuned without a real Mac. So all artwork lives in the top and
bottom safe zones, leaving a wide clear band in the middle for the icons and
their Finder labels — no text or arrow lands under an icon.

    windowSize                = 660 x 480   (the saved PNG is exactly this)
    appPosition               = (175, 220)  ← app icon lands here
    applicationFolderPosition = (485, 220)  ← Applications alias lands here

The canvas is drawn at 2x and downscaled to 660x480 before saving. Finder maps
a background image's PIXELS straight onto window POINTS (it ignores PNG DPI
metadata), so a PNG saved at the raw 2x size shows up double-size and cropped —
the title blown up huge, the subtitle cut off mid-word, the bottom panel pushed
out of the window entirely. The 2x pass exists only to supersample the text;
the file we ship must match windowSize pixel-for-point.

Regenerate:  python3 generate_background.py   (needs Pillow + logo.png)
The produced background.png is committed, so CI needs no Pillow.
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
LOGO = os.path.join(HERE, "logo.png")
FONT_DIR = "/usr/share/fonts/truetype/dejavu"

S = 2
WIN_W, WIN_H = 660, 480
W, H = WIN_W * S, WIN_H * S

INK_TOP = (18, 27, 51)
INK_BOTTOM = (6, 11, 24)
CANARY = (245, 179, 1)
TEXT = (233, 238, 250)
MUTED = (150, 162, 186)

# Vertical safe zones (points). Nothing is drawn in the middle band, where the
# icons + their Finder labels live.
TOP_LIMIT = 150      # artwork stays above this
BOTTOM_LIMIT = 300   # the first-open panel starts here


def font(name, pt):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), pt * S)


def vgrad(size, top, bottom):
    w, h = size
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(round(top[i] * (1 - t) + bottom[i] * t) for i in range(3)))
    return g.resize((w, h))


def center(draw, cx, cy, text, fnt, fill):
    l, t, r, b = draw.textbbox((0, 0), text, font=fnt)
    draw.text((cx - (r - l) / 2 - l, cy - (b - t) / 2 - t), text, font=fnt, fill=fill)


def main():
    img = vgrad((W, H), INK_TOP, INK_BOTTOM).convert("RGBA")

    # Warm canary glow low-center, well clear of the text.
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([W * 0.18, H * 0.32, W * 0.82, H * 0.84], fill=CANARY + (34,))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(120)))
    d = ImageDraw.Draw(img)

    # ── TOP safe zone: mascot + wordmark, subtitle, drag instruction ───────
    mascot = Image.open(LOGO).convert("RGBA")
    bb = mascot.getbbox()
    if bb:
        mascot = mascot.crop(bb)
    mh = 46 * S
    mw = round(mascot.width * (mh / mascot.height))
    mascot = mascot.resize((mw, mh), Image.LANCZOS)

    title_f = font("DejaVuSans-Bold.ttf", 25)
    title = "SecuraCV Lab"
    tl, tt, tr, tb = d.textbbox((0, 0), title, font=title_f)
    gap = 15 * S
    gx = (W - (mw + gap + (tr - tl))) // 2
    cy = 34 * S
    img.paste(mascot, (gx, cy - mh // 2), mascot)
    d.text((gx + mw + gap - tl, cy - (tb - tt) / 2 - tt), title, font=title_f, fill=TEXT)

    center(d, W // 2, 72 * S, "Your Canary workshop — local-first, talks only to your own devices.",
           font("DejaVuSans.ttf", 13), MUTED)
    center(d, W // 2, 108 * S, "Drag the app into Applications  →",
           font("DejaVuSans-Bold.ttf", 15), CANARY)

    # (icons + their Finder labels render in the clear band here — no artwork)

    # ── BOTTOM safe zone: the one-time first-open command (the star) ───────
    py0, py1 = BOTTOM_LIMIT * S, (WIN_H - 20) * S
    panel = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(panel).rounded_rectangle([32 * S, py0, W - 32 * S, py1],
                                            radius=18 * S, fill=(255, 255, 255, 15))
    img = Image.alpha_composite(img, panel)
    d = ImageDraw.Draw(img)

    center(d, W // 2, py0 + 24 * S,
           "First launch blocked? macOS quarantines apps from the web.",
           font("DejaVuSans-Bold.ttf", 14), TEXT)
    center(d, W // 2, py0 + 47 * S,
           "Open Terminal (⌘-Space → “Terminal”), paste this line, press return:",
           font("DejaVuSans.ttf", 12), MUTED)

    # The command, on its own highlighted pill — the thing to copy.
    cmd = 'xattr -dr com.apple.quarantine "/Applications/SecuraCV Lab.app"'
    cmd_f = font("DejaVuSansMono-Bold.ttf", 12)
    cl, ct, cr, cb = d.textbbox((0, 0), cmd, font=cmd_f)
    cw = cr - cl
    pill_y = py0 + 66 * S
    pill = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(pill).rounded_rectangle(
        [W // 2 - cw // 2 - 16 * S, pill_y, W // 2 + cw // 2 + 16 * S, pill_y + 30 * S],
        radius=8 * S, fill=(0, 0, 0, 90))
    img = Image.alpha_composite(img, pill)
    d = ImageDraw.Draw(img)
    center(d, W // 2, pill_y + 15 * S, cmd, cmd_f, CANARY)

    center(d, W // 2, py1 - 16 * S,
           "Then launch it normally, forever. Full steps: INSTALL.md / the release notes.",
           font("DejaVuSans.ttf", 10), MUTED)

    # Downscale the supersampled canvas to the exact window size — Finder maps
    # image pixels to window points, so anything else renders wrong-scale.
    img = img.resize((WIN_W, WIN_H), Image.LANCZOS)
    dst = os.path.join(HERE, "background.png")
    img.convert("RGBA").save(dst)
    print("wrote", dst, os.path.getsize(dst), "bytes", f"({WIN_W}x{WIN_H})")


if __name__ == "__main__":
    main()
