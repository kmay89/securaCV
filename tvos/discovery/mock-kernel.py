#!/usr/bin/env python3
"""Reference SecuraCV fleet-discovery kernel — the smallest thing the Witness
Wall can find and render.

Serves GET /api/fleet as CORS-enabled JSON (see tvos/discovery/DISCOVERY.md),
so a browser on the same scheme can read it. Real kernels/firmware answer the
same contract; this is here so you can watch discovery work end to end without
hardware.

    python3 tvos/discovery/mock-kernel.py            # port 8099
    python3 tvos/discovery/mock-kernel.py --port 9000

Then, in the emulator's "Connect your fleet" panel, enter this host and Connect.
Stdlib only — no dependencies.
"""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# A spread of what real firmware sends (tvos/discovery/DISCOVERY.md): some
# devices carry the optional `hw` (which board — what resolves the figure a
# client draws) and `hub` (where the device stands with its hub), and some
# omit them the way pre-field firmware does — so a client's fallback path is
# exercised too. The hw values are real board ids (firmware/boards/*/pins,
# CANARY_FIGURE_HARDWARE), so the Wall's figure/turntable path lights up.
DEVICES = [
    {"name": "Front Door", "online": True, "chain": "ok", "product": "canary-wap", "hub": "ok"},
    {"name": "Studio", "online": True, "chain": "ok", "product": "canary"},
    {"name": "Garage", "online": True, "chain": "ok", "product": "canary-sense",
     "hw": "xiao-esp32c6-mr60", "hub": "ok"},
    {"name": "Back Gate", "online": True, "chain": "ok", "product": "canary"},
    {"name": "Driveway", "online": False, "chain": "ok", "product": "canary-vision",
     "hw": "xiao-esp32c3", "hub": "none"},
]

# Populated from CLI: after JOIN_AFTER seconds, a freshly "flashed" Canary joins
# the fleet — so you can watch the emulator discover it appear live.
START = time.monotonic()
JOIN_AFTER = None
JOIN_NAME = "Newly Flashed"


def current_fleet():
    devices = list(DEVICES)
    if JOIN_AFTER is not None and (time.monotonic() - START) >= JOIN_AFTER:
        devices.append({"name": JOIN_NAME, "online": True, "chain": "ok", "product": "canary"})
    return {"kernel": "reference-kernel", "verified_through": "now", "devices": devices}


class Handler(BaseHTTPRequestHandler):
    def _cors(self):
        # The whole reason a browser on another origin can read this.
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "accept")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        # /api/fleet only — the one endpoint the contract names. This used to
        # also answer an "/api/hello" alias that no client in any repo ever
        # requested; a reference kernel that answers more than the contract
        # teaches the wrong contract.
        if self.path.split("?", 1)[0] == "/api/fleet":
            body = json.dumps(current_fleet()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self._cors()
            self.end_headers()

    def log_message(self, fmt, *args):  # keep the console quiet but useful
        print("[mock-kernel] " + (fmt % args))


def main():
    global JOIN_AFTER, JOIN_NAME
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--join-after", type=float, default=None,
                    help="seconds after start when a freshly-flashed Canary joins the fleet")
    ap.add_argument("--join-name", default="Newly Flashed")
    args = ap.parse_args()
    JOIN_AFTER, JOIN_NAME = args.join_after, args.join_name
    if JOIN_AFTER is not None:
        print(f"'{JOIN_NAME}' will join the fleet {JOIN_AFTER}s after start.")
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"SecuraCV reference kernel → http://{args.host}:{args.port}/api/fleet")
    print("Point the Witness Wall emulator's 'Connect your fleet' panel here.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
