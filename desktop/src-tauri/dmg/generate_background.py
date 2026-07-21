#!/usr/bin/env python3
"""Build the branded .dmg window background for the SecuraCV Flasher installer.

The DMG opens to a designed window: the app icon on the left, an arrow to the
Applications folder on the right, and the one-time first-open note baked in —
so installing feels intentional, not like a raw disk image.

Geometry is locked to tauri.conf.json's dmg block (points; this canvas is 2x
for retina crispness):
    windowSize                = 660 x 460
    appPosition               = (175, 205)   ← app icon lands here
    applicationFolderPosition = (485, 205)   ← Applications folder lands here

Regenerate with:  python3 generate_background.py   (needs Pillow + brand asset)
The produced background.png is committed, so CI needs no Pillow.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
MASCOT = os.path.normpath(os.path.join(HERE, "../../../brands/logo_main.png"))
FONT_DIR = "/usr/share/fonts/truetype/dejavu"

S = 2                               # retina scale (points -> pixels)
WIN_W, WIN_H = 660, 460
W, H = WIN_W * S, WIN_H * S
APP = (175 * S, 205 * S)            # where the app icon lands
APPS = (485 * S, 205 * S)          # where the Applications folder lands

INK_TOP = (18, 27, 51)
INK_BOTTOM = (6, 11, 24)
CANARY = (245, 179, 1)
TEXT = (233, 238, 250)
MUTED = (150, 162, 186)


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
    """Draw text horizontally centred on cx, vertically centred on cy."""
    l, t, r, b = draw.textbbox((0, 0), text, font=fnt)
    draw.text((cx - (r - l) / 2 - l, cy - (b - t) / 2 - t), text, font=fnt, fill=fill)


def main():
    img = vgrad((W, H), INK_TOP, INK_BOTTOM).convert("RGBA")

    # Warm canary glow behind the drag path.
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([W * 0.16, H * 0.20, W * 0.84, H * 0.72], fill=CANARY + (42,))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(120)))
    d = ImageDraw.Draw(img)

    # ── title row: small mascot + wordmark, centred ────────────────────────
    mascot = Image.open(MASCOT).convert("RGBA")
    bb = mascot.getbbox()
    if bb:
        mascot = mascot.crop(bb)
    mh = 46 * S
    mw = round(mascot.width * (mh / mascot.height))
    mascot = mascot.resize((mw, mh), Image.LANCZOS)

    title_f = font("DejaVuSans-Bold.ttf", 26)
    title = "SecuraCV Flasher"
    tl, tt, tr, tb = d.textbbox((0, 0), title, font=title_f)
    gap = 16 * S
    group_w = mw + gap + (tr - tl)
    gx = (W - group_w) // 2
    cy = 40 * S
    img.paste(mascot, (gx, cy - mh // 2), mascot)
    d.text((gx + mw + gap - tl, cy - (tb - tt) / 2 - tt), title, font=title_f, fill=TEXT)

    center(d, W // 2, 84 * S, "Flash a Canary — no browser, no terminal.",
           font("DejaVuSans.ttf", 14), MUTED)

    # ── header (the icons + their Finder filename labels are drawn by the
    #    DMG itself at APP / APPS, so we don't re-label them here) ───────────
    center(d, W // 2, 124 * S, "Drag the app into Applications", font("DejaVuSans-Bold.ttf", 15), TEXT)

    # ── horizontal drag arrow between them ─────────────────────────────────
    y = APP[1]
    x0, x1 = APP[0] + 82 * S, APPS[0] - 82 * S
    arc = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ad = ImageDraw.Draw(arc)
    pts = []
    for i in range(101):
        t = i / 100
        pts.append((x0 + (x1 - x0) * t, y - math.sin(t * math.pi) * 30 * S))
    for i in range(0, len(pts) - 1, 4):
        ad.line([pts[i], pts[min(i + 2, len(pts) - 1)]], fill=CANARY + (235,), width=5 * S)
    hx, hy = pts[-1]
    ang = math.atan2(hy - pts[-8][1], hx - pts[-8][0])
    for a in (ang + 2.5, ang - 2.5):
        ad.line([(hx, hy), (hx + math.cos(a) * 24 * S, hy + math.sin(a) * 24 * S)],
                fill=CANARY + (235,), width=5 * S)
    img = Image.alpha_composite(img, arc)
    d = ImageDraw.Draw(img)

    # ── footer: the one-time first-open note ───────────────────────────────
    px0, px1 = 40 * S, W - 40 * S
    py0, py1 = 318 * S, 438 * S
    panel = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(panel).rounded_rectangle([px0, py0, px1, py1], radius=18 * S,
                                            fill=(255, 255, 255, 15))
    img = Image.alpha_composite(img, panel)
    d = ImageDraw.Draw(img)
    center(d, W // 2, py0 + 26 * S,
           "First launch: right-click the app in Applications → Open  (one time only)",
           font("DejaVuSans-Bold.ttf", 14), TEXT)
    center(d, W // 2, py0 + 55 * S, "— or, in Terminal —", font("DejaVuSans.ttf", 11), MUTED)
    center(d, W // 2, py0 + 82 * S,
           'xattr -dr com.apple.quarantine "/Applications/SecuraCV Flasher.app"',
           font("DejaVuSansMono.ttf", 12), CANARY)

    dst = os.path.join(HERE, "background.png")
    img.convert("RGBA").save(dst)
    print("wrote", dst, os.path.getsize(dst), "bytes", f"({W}x{H})")


if __name__ == "__main__":
    main()
