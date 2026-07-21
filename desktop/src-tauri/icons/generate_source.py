#!/usr/bin/env python3
"""Rasterize the SecuraCV Flasher app-icon source PNG (1024x1024, RGBA).

No external deps — writes the PNG by hand (zlib + CRC). This is the *source*
icon; CI runs `tauri icon source.png` to produce the .icns/.ico/png set the
bundler needs. Kept in-repo so the icon is reproducible and reviewable.

The mark echoes the canary in the app header: a warm rounded-square badge
with a yellow bird body + head, an orange beak, and a dark eye.
"""
import math
import struct
import zlib

N = 1024
CORNER = 224  # rounded-square radius


def rounded_alpha(x, y):
    # Distance-based rounding of the badge corners for a soft macOS-y squircle.
    cx = min(max(x, CORNER), N - CORNER)
    cy = min(max(y, CORNER), N - CORNER)
    d = math.hypot(x - cx, y - cy)
    if d <= CORNER - 1:
        return 255
    if d >= CORNER + 1:
        return 0
    return int(round((CORNER + 1 - d) * 127.5))


def blend(dst, src, a):
    a /= 255.0
    return tuple(int(round(dst[i] * (1 - a) + src[i] * a)) for i in range(3))


def disc(x, y, cx, cy, r):
    """Antialiased coverage (0..1) of point in a filled circle."""
    d = math.hypot(x - cx, y - cy)
    if d <= r - 1:
        return 1.0
    if d >= r + 1:
        return 0.0
    return (r + 1 - d) / 2.0


BG = (0x11, 0x18, 0x2b)      # deep navy badge
BODY = (0xf5, 0xb3, 0x01)    # canary yellow
BODY2 = (0xe3, 0xb3, 0x3c)   # wing shade
BEAK = (0xf0, 0x8c, 0x2e)    # orange
EYE = (0x14, 0x14, 0x14)


def pixel(x, y):
    # Badge background with rounded corners.
    a_badge = rounded_alpha(x, y)
    if a_badge == 0:
        return (0, 0, 0, 0)
    col = BG

    # Bird body (big circle) + head (smaller, upper-right), scaled into badge.
    body = disc(x, y, 470, 560, 250)
    if body:
        col = blend(col, BODY, int(body * 255))
    wing = disc(x, y, 400, 590, 130)
    if wing:
        col = blend(col, BODY2, int(wing * 200))
    head = disc(x, y, 610, 380, 150)
    if head:
        col = blend(col, BODY, int(head * 255))

    # Beak — a small triangle off the head.
    if 690 <= x <= 800 and abs(y - 360) <= (x - 690) * 0.42:
        col = blend(col, BEAK, 255)

    # Eye.
    eye = disc(x, y, 660, 350, 26)
    if eye:
        col = blend(col, EYE, int(eye * 255))

    return (col[0], col[1], col[2], a_badge)


def build():
    raw = bytearray()
    for y in range(N):
        raw.append(0)  # filter type 0
        for x in range(N):
            r, g, b, a = pixel(x, y)
            raw += bytes((r, g, b, a))
    return raw


def chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def main():
    ihdr = struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(build()), 9))
        + chunk(b"IEND", b"")
    )
    with open("source.png", "wb") as f:
        f.write(png)
    print("wrote source.png", len(png), "bytes")


if __name__ == "__main__":
    main()
