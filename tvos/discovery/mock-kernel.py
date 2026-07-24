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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FLEET = {
    "kernel": "reference-kernel",
    "verified_through": "now",
    "devices": [
        {"name": "Front Door", "online": True, "chain": "ok", "product": "canary-wap"},
        {"name": "Studio", "online": True, "chain": "ok", "product": "canary"},
        {"name": "Garage", "online": True, "chain": "ok", "product": "canary-sense"},
        {"name": "Back Gate", "online": True, "chain": "ok", "product": "canary"},
        {"name": "Driveway", "online": False, "chain": "ok", "product": "canary-vision"},
    ],
}


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
        if self.path.split("?", 1)[0] in ("/api/fleet", "/api/hello"):
            body = json.dumps(FLEET).encode()
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"SecuraCV reference kernel → http://{args.host}:{args.port}/api/fleet")
    print("Point the Witness Wall emulator's 'Connect your fleet' panel here.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
