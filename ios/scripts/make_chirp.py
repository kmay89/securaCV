#!/usr/bin/env python3
"""Generate the canary's chirp — the one sound in the app, for the one moment
that earned a voice: the alert path verifying end-to-end.

Same contract as the icons (make_app_icon.py): the generator is the source of
truth, the output is committed so no runner needs Python, and it is
deterministic — pure math, no randomness, no timestamps, byte-stable across
runs. A unit test asserts the committed file is present in the app bundle.

The sound itself: two quick upward sweeps with soft attack and exponential
decay — a small, warm "tweet-tweet", quiet by design (peak −9 dBFS). It is
played as a SYSTEM sound (Native/Feedback.swift), which respects the silent
switch: a muted phone stays mute.

    python3 ios/scripts/make_chirp.py

Writes ios/Sounds/canary-verified.wav (44.1 kHz, mono, 16-bit)."""

from __future__ import annotations

import math
import os
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "Sounds"))
OUT = os.path.join(OUT_DIR, "canary-verified.wav")

RATE = 44_100
PEAK = 0.35  # ≈ −9 dBFS — present, never startling


def sweep(start_hz: float, end_hz: float, seconds: float) -> list[float]:
    """One chirp note: linear frequency sweep, soft attack, exponential decay."""
    n = int(RATE * seconds)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / n
        hz = start_hz + (end_hz - start_hz) * t
        phase += 2 * math.pi * hz / RATE
        attack = min(1.0, i / (0.008 * RATE))          # 8 ms in
        decay = math.exp(-4.2 * t)                      # tails off naturally
        out.append(math.sin(phase) * attack * decay)
    return out


def silence(seconds: float) -> list[float]:
    return [0.0] * int(RATE * seconds)


def main() -> int:
    samples = (
        sweep(2_800, 3_900, 0.075)
        + silence(0.045)
        + sweep(3_100, 4_300, 0.110)
        + silence(0.06)
    )
    os.makedirs(OUT_DIR, exist_ok=True)
    with wave.open(OUT, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s * PEAK)) * 32_767))
            for s in samples
        )
        w.writeframes(frames)
    print(f"wrote {OUT} ({len(samples) / RATE:.3f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
