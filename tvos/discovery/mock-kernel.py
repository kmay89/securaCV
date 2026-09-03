#!/usr/bin/env python3
"""Reference SecuraCV fleet-discovery kernel — the smallest thing the Witness
Wall can find and render.

Serves GET /api/fleet as CORS-enabled JSON (see tvos/discovery/DISCOVERY.md),
so a browser on the same scheme can read it. Real kernels/firmware answer the
same contract; this is here so you can watch discovery work end to end without
hardware.

    python3 tvos/discovery/mock-kernel.py            # port 8099
    python3 tvos/discovery/mock-kernel.py --port 9000
    python3 tvos/discovery/mock-kernel.py --sealed-log   # + GET /api/sealed-log

Then, in the emulator's "Connect your fleet" panel, enter this host and Connect.
Stdlib only — no dependencies.
"""
import argparse
import json
import pathlib
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# A spread of what real firmware sends (tvos/discovery/DISCOVERY.md): some
# devices carry the optional `hw` (which board — what resolves the figure a
# client draws) and `hub` (where the device stands with its hub), and some
# omit them the way pre-field firmware does — so a client's fallback path is
# exercised too. The hw values are real board ids (firmware/boards/*/pins,
# CANARY_FIGURE_HARDWARE), so the Wall's figure/turntable path lights up.
#
# The optional wellbeing keys ride two rows the way the contract phrases
# them: coarse WORDS, absent everywhere the device cannot honestly say —
# never a default a client could mistake for an empty calm room. The sense
# row carries the room story (presence/occupants/breathing); a separate,
# ONLINE vision row carries a seeing claim, because a kernel that reports a
# device offline cannot honestly also claim it is seeing something right now
# (a stale claim omits rather than lies) — which is why Driveway stays
# offline and key-free.
DEVICES = [
    {"name": "Front Door", "online": True, "chain": "ok", "product": "canary-wap", "hub": "ok"},
    {"name": "Studio", "online": True, "chain": "ok", "product": "canary"},
    {"name": "Garage", "online": True, "chain": "ok", "product": "canary-sense",
     "hw": "xiao-esp32c6-mr60", "hub": "ok",
     "presence": "present", "occupants": "1", "breathing": True},
    {"name": "Back Gate", "online": True, "chain": "ok", "product": "canary"},
    {"name": "Porch", "online": True, "chain": "ok", "product": "canary-vision",
     "hw": "xiao-esp32c3", "hub": "ok", "seeing": "package", "seeing_score": 92},
    {"name": "Driveway", "online": False, "chain": "ok", "product": "canary-vision",
     "hw": "xiao-esp32c3", "hub": "none"},
]

# The kernel's own pinned sealed-log vector — a three-entry chain that
# genuinely verifies (the same fixture witness-core's tests replay), read
# from the checkout rather than duplicated here so the bytes can never
# drift. Loaded only under --sealed-log; SEALED_LOG stays None otherwise.
SEALED_LOG_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "tests" / "fixtures" / "envelope" / "sealed_log_document_vector.json"
)
SEALED_LOG = None  # bytes once --sealed-log loads the vector

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
        # /api/fleet is the contract's one required endpoint; /api/sealed-log
        # is its optional companion (DISCOVERY.md), answered here ONLY under
        # --sealed-log — a reference kernel that answers more than the
        # contract teaches the wrong contract, so the default stays minimal.
        # (This used to also answer an "/api/hello" alias that no client in
        # any repo ever requested.) Note the real kernel gates /api/sealed-log
        # behind its capability token; this flag exists so you can watch a
        # client's verification path light up without one.
        path = self.path.split("?", 1)[0]
        if path == "/api/fleet":
            self._json(json.dumps(current_fleet()).encode())
        elif path == "/api/sealed-log" and SEALED_LOG is not None:
            self._json(SEALED_LOG)
        else:
            self.send_response(404)
            self._cors()
            self.end_headers()

    def _json(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self._cors()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # keep the console quiet but useful
        print("[mock-kernel] " + (fmt % args))


def main():
    global JOIN_AFTER, JOIN_NAME, SEALED_LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--join-after", type=float, default=None,
                    help="seconds after start when a freshly-flashed Canary joins the fleet")
    ap.add_argument("--join-name", default="Newly Flashed")
    ap.add_argument("--sealed-log", action="store_true",
                    help="also answer GET /api/sealed-log with the repo's pinned sealed-log "
                         "vector, so a client's verification path lights up end to end")
    args = ap.parse_args()
    JOIN_AFTER, JOIN_NAME = args.join_after, args.join_name
    if JOIN_AFTER is not None:
        print(f"'{JOIN_NAME}' will join the fleet {JOIN_AFTER}s after start.")
    if args.sealed_log:
        try:
            SEALED_LOG = SEALED_LOG_PATH.read_bytes()
        except OSError as e:
            raise SystemExit(
                f"--sealed-log needs the pinned vector at {SEALED_LOG_PATH} ({e}).\n"
                "Run from a securaCV checkout, or drop the flag."
            )
        print("Also serving GET /api/sealed-log (the repo's pinned three-entry chain).")
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"SecuraCV reference kernel → http://{args.host}:{args.port}/api/fleet")
    print("Point the Witness Wall emulator's 'Connect your fleet' panel here.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
