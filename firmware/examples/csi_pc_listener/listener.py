#!/usr/bin/env python3
"""
csi_pc_listener — subscribe to a SecuraCV Canary's CSI stream and print
motion / breathing scores once per second.

Usage:
    python3 listener.py                   # auto-discover via canary.local
    python3 listener.py --host 192.168.1.42
    python3 listener.py --plot            # live matplotlib chart (optional dep)

The default endpoint is the Server-Sent-Events stream emitted by the
canary-wap firmware (Phase 4 of the WiFi CSI Tool plan):

    GET http://canary.local/api/csi/stream

If that endpoint is not yet live on your firmware, pass --poll to fall
back to the existing /api/status JSON (rolled motion/breathing scalars).

The output is privacy-conformant by construction: only aggregate scalars,
no MAC addresses, no precise wallclock — exactly what the SecuraCV event
contract permits.

License: MIT.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from typing import Iterator, Optional


def stream_sse(host: str) -> Iterator[dict]:
    """Yield one parsed event dict per SSE 'data:' line."""
    url = f"http://{host}/api/csi/stream"
    req = urllib.request.Request(url, headers={"Accept": "text/event-stream"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        buf = []
        for raw in resp:
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            if line.startswith("data:"):
                buf.append(line[5:].lstrip())
                continue
            if line == "":
                if buf:
                    payload = "\n".join(buf)
                    buf = []
                    try:
                        yield json.loads(payload)
                    except json.JSONDecodeError:
                        # Skip malformed frames silently — the SSE channel
                        # may carry comments or other non-JSON lines.
                        pass


def poll_status(host: str) -> Iterator[dict]:
    """Fallback: poll /api/status once per second and yield a CSI-shaped row."""
    url = f"http://{host}/api/status"
    last_t = 0
    while True:
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                doc = json.load(resp)
        except (urllib.error.URLError, json.JSONDecodeError) as e:
            print(f"[poll] {e}", file=sys.stderr)
            time.sleep(2)
            continue
        last_t += 1
        yield {
            "t":         last_t,
            "motion":    int(doc.get("motion", 0)),
            "breathing": int(doc.get("breathing", 0)),
            "confidence": doc.get("confidence", "tentative"),
            "rssi_mean":  int(doc.get("rssi_mean", 0)),
            "frames":     int(doc.get("frames", 0)),
        }
        time.sleep(1)


def render_text(row: dict) -> None:
    print(
        f"t={row.get('t', 0):>4}  "
        f"motion={row.get('motion', 0):>3}  "
        f"breathing={row.get('breathing', 0):>3}  "
        f"conf={row.get('confidence', 'tentative'):>9}  "
        f"frames={row.get('frames', 0)}"
    )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="canary.local",
                   help="hostname or IP of the Canary device")
    p.add_argument("--poll", action="store_true",
                   help="fall back to polling /api/status if SSE is unavailable")
    p.add_argument("--plot", action="store_true",
                   help="open a live matplotlib chart (requires matplotlib)")
    args = p.parse_args()

    plotter: Optional["LivePlotter"] = None
    if args.plot:
        try:
            plotter = LivePlotter()
        except ImportError:
            print("matplotlib not installed; pip install matplotlib --user",
                  file=sys.stderr)
            return 2

    source = poll_status(args.host) if args.poll else stream_sse(args.host)
    print(f"[csi_pc_listener] subscribed to http://{args.host}/  (Ctrl-C to stop)")
    try:
        for row in source:
            render_text(row)
            if plotter:
                plotter.add(row)
    except KeyboardInterrupt:
        print()
        return 0
    except urllib.error.URLError as e:
        print(f"[csi_pc_listener] cannot reach {args.host}: {e}", file=sys.stderr)
        if not args.poll:
            print("                  retry with --poll to use /api/status",
                  file=sys.stderr)
        return 1
    return 0


# Optional matplotlib live plot. Imported lazily so the script remains
# usable on a headless machine without matplotlib installed.
class LivePlotter:
    def __init__(self) -> None:
        import matplotlib.pyplot as plt   # noqa: WPS433  (intentional lazy import)
        self._plt = plt
        self._fig, self._ax = plt.subplots()
        self._motion: list[int] = []
        self._breathing: list[int] = []
        self._line_m, = self._ax.plot([], [], label="motion")
        self._line_b, = self._ax.plot([], [], label="breathing")
        self._ax.set_ylim(0, 100)
        self._ax.set_xlabel("seconds")
        self._ax.set_ylabel("score (0..100)")
        self._ax.legend(loc="upper right")
        plt.ion()
        plt.show()

    def add(self, row: dict) -> None:
        self._motion.append(int(row.get("motion", 0)))
        self._breathing.append(int(row.get("breathing", 0)))
        if len(self._motion) > 120:
            self._motion = self._motion[-120:]
            self._breathing = self._breathing[-120:]
        x = list(range(len(self._motion)))
        self._line_m.set_data(x, self._motion)
        self._line_b.set_data(x, self._breathing)
        self._ax.set_xlim(max(0, len(x) - 120), max(60, len(x)))
        self._fig.canvas.draw_idle()
        self._fig.canvas.flush_events()


if __name__ == "__main__":
    sys.exit(main())
