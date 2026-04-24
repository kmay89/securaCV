#!/usr/bin/env python3
"""
SecuraCV Setup Wizard HTTP Server

Serves the first-run setup wizard on the add-on's ingress port.
Called by run.sh when device_key_seed is not yet configured.

Endpoints:
  GET  /            → wizard HTML
  GET  /static/*    → static assets
  POST /api/save    → validate and save add-on options via HA Supervisor API
  POST /api/test-camera → test RTSP/IP camera TCP reachability
  GET  /api/status  → current add-on options (for pre-filling the form)
  POST /api/verify  → run a quick health check after setup
"""

import http.server
import json
import os
import socket
import sys
import urllib.request
import urllib.error
from pathlib import Path

INGRESS_PORT = int(os.environ.get("INGRESS_PORT", "8788"))
SUPERVISOR_TOKEN = os.environ.get("SUPERVISOR_TOKEN") or os.environ.get("HASSIO_TOKEN", "")
SUPERVISOR_URL = "http://supervisor"
WIZARD_DIR = Path("/usr/local/share/securacv-wizard")
DEVICE_KEY_FILE = Path("/config/.securacv/device_key")


# ---------------------------------------------------------------------------
# Supervisor API helpers
# ---------------------------------------------------------------------------

def _supervisor_request(method: str, path: str, data: dict | None = None) -> dict:
    """Make an authenticated request to the HA Supervisor API."""
    url = f"{SUPERVISOR_URL}{path}"
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(
        url,
        data=body,
        method=method,
        headers={
            "Authorization": f"Bearer {SUPERVISOR_TOKEN}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read().decode()
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode(errors="replace")
        raise RuntimeError(f"Supervisor API error {exc.code}: {body_text}") from exc
    except Exception as exc:
        raise RuntimeError(f"Supervisor API unreachable: {exc}") from exc


def get_addon_options() -> dict:
    resp = _supervisor_request("GET", "/addons/self/options/config")
    return resp.get("data", {})


def set_addon_options(options: dict) -> None:
    _supervisor_request("POST", "/addons/self/options", {"options": options})


def restart_addon() -> None:
    _supervisor_request("POST", "/addons/self/restart")


# ---------------------------------------------------------------------------
# Camera connectivity test (TCP)
# ---------------------------------------------------------------------------

def test_camera_tcp(url: str) -> dict:
    """
    Given an RTSP URL like rtsp://user:pass@192.168.1.100:554/stream1,
    attempt a TCP connection to host:port and return a result dict.
    """
    try:
        # Strip scheme and credentials
        stripped = url
        for prefix in ("rtsp://", "rtsps://", "http://", "https://"):
            if stripped.startswith(prefix):
                stripped = stripped[len(prefix):]
                break

        # Remove credentials
        if "@" in stripped:
            stripped = stripped.split("@", 1)[1]

        # Extract host:port
        if "/" in stripped:
            host_port = stripped.split("/")[0]
        else:
            host_port = stripped

        if ":" in host_port:
            host, port_str = host_port.rsplit(":", 1)
            port = int(port_str)
        else:
            host = host_port
            port = 554  # default RTSP

        sock = socket.create_connection((host, port), timeout=5)
        sock.close()
        return {"ok": True, "host": host, "port": port}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class WizardHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Quieter logging — only errors
        if args and len(args) >= 3 and str(args[1]) not in ("200", "204"):
            sys.stderr.write(f"[wizard] {fmt % args}\n")

    # ------------------------------------------------------------------
    def do_GET(self):
        ingress_path = os.environ.get("INGRESS_PATH", "")

        # Strip ingress path prefix
        path = self.path
        if ingress_path and path.startswith(ingress_path):
            path = path[len(ingress_path):]
        if not path or path == "/":
            path = "/index.html"

        # API: current add-on status/options
        if path == "/api/status":
            self._json_response(self._handle_status())
            return

        # Serve static files from wizard dir
        file_path = WIZARD_DIR / path.lstrip("/")
        if file_path.is_file():
            self._serve_file(file_path)
        else:
            # Fall back to index.html for SPA routing
            index = WIZARD_DIR / "index.html"
            if index.is_file():
                self._serve_file(index)
            else:
                self._send_error(404, "Wizard files not found")

    # ------------------------------------------------------------------
    def do_POST(self):
        ingress_path = os.environ.get("INGRESS_PATH", "")
        path = self.path
        if ingress_path and path.startswith(ingress_path):
            path = path[len(ingress_path):]

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""

        try:
            payload = json.loads(body) if body else {}
        except json.JSONDecodeError:
            self._json_response({"ok": False, "error": "Invalid JSON"}, status=400)
            return

        if path == "/api/save":
            self._json_response(self._handle_save(payload))
        elif path == "/api/test-camera":
            self._json_response(self._handle_test_camera(payload))
        elif path == "/api/verify":
            self._json_response(self._handle_verify())
        else:
            self._send_error(404, "Unknown endpoint")

    # ------------------------------------------------------------------
    def _handle_status(self) -> dict:
        try:
            opts = get_addon_options()
            # Check if setup is complete
            key_seed = opts.get("device_key_seed", "")
            configured = bool(key_seed and key_seed != "devkey:mvp")
            # Try reading device key from file too
            if not configured and DEVICE_KEY_FILE.exists():
                configured = True
            return {"ok": True, "configured": configured, "options": opts}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def _handle_save(self, payload: dict) -> dict:
        """
        Validate and persist the wizard-collected settings.

        Expected payload:
        {
          "mode": "frigate" | "standalone",
          "cameras": [{"name": str, "url": str, "zone_id": str}],
          "retention_days": int,
          "digest_time": "HH:MM",
          "pattern_alerts": bool,
          "device_key_seed": str  (optional; generated if omitted)
        }
        """
        try:
            mode = payload.get("mode", "frigate")
            cameras = payload.get("cameras", [])
            retention_days = int(payload.get("retention_days", 1))
            device_key_seed = payload.get("device_key_seed", "").strip()

            # Generate key if not provided
            if not device_key_seed:
                import secrets
                device_key_seed = secrets.token_hex(32)

            # Persist device key to file for the install script's backing-up message
            DEVICE_KEY_FILE.parent.mkdir(parents=True, exist_ok=True)
            DEVICE_KEY_FILE.write_text(device_key_seed + "\n")
            DEVICE_KEY_FILE.chmod(0o600)

            # Build add-on options
            options: dict = {
                "mode": mode,
                "device_key_seed": device_key_seed,
                "retention_days": max(1, min(365, retention_days)),
                "time_bucket_minutes": 10,
                "log_level": "info",
                "go2rtc_discovery": True,
                "go2rtc_url": "http://homeassistant.local:1984",
                "mqtt_publish": {
                    "enabled": True,
                    "host": "core-mosquitto",
                    "port": 1883,
                    "username": "",
                    "password": "",
                    "topic_prefix": "witness",
                    "discovery_prefix": "homeassistant",
                },
            }

            if mode == "frigate":
                options["frigate"] = {
                    "mqtt_host": "core-mosquitto",
                    "mqtt_port": 1883,
                    "mqtt_topic": "frigate/events",
                    "mqtt_username": "",
                    "mqtt_password": "",
                    "min_confidence": 0.5,
                    "cameras": [c["name"] for c in cameras if c.get("name")],
                    "labels": ["person", "car", "dog", "cat"],
                }
                options["cameras"] = []
            else:
                options["cameras"] = [
                    {
                        "name": c.get("name", f"camera_{i}"),
                        "url": c.get("url", ""),
                        "zone_id": c.get("zone_id", f"zone:{c.get('name', f'camera_{i}')}"),
                        "fps": int(c.get("fps", 10)),
                        "width": int(c.get("width", 1280)),
                        "height": int(c.get("height", 720)),
                        "enabled": True,
                    }
                    for i, c in enumerate(cameras)
                ]
                options["frigate"] = {
                    "mqtt_host": "core-mosquitto",
                    "mqtt_port": 1883,
                    "mqtt_topic": "frigate/events",
                    "mqtt_username": "",
                    "mqtt_password": "",
                    "min_confidence": 0.5,
                    "cameras": [],
                    "labels": [],
                }

            set_addon_options(options)

            return {
                "ok": True,
                "message": "Configuration saved. Restarting Privacy Witness Kernel…",
                "device_key_seed": device_key_seed,
            }
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def _handle_test_camera(self, payload: dict) -> dict:
        url = payload.get("url", "").strip()
        if not url:
            return {"ok": False, "error": "No URL provided"}
        return test_camera_tcp(url)

    def _handle_verify(self) -> dict:
        """Quick health check: is the witness API reachable?"""
        try:
            req = urllib.request.Request("http://127.0.0.1:8799/health", timeout=5)
            with urllib.request.urlopen(req) as resp:
                data = json.loads(resp.read().decode())
            return {"ok": True, "health": data}
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    # ------------------------------------------------------------------
    def _serve_file(self, path: Path):
        content_types = {
            ".html": "text/html; charset=utf-8",
            ".css": "text/css",
            ".js": "application/javascript",
            ".json": "application/json",
            ".svg": "image/svg+xml",
            ".png": "image/png",
            ".ico": "image/x-icon",
        }
        suffix = path.suffix.lower()
        content_type = content_types.get(suffix, "application/octet-stream")
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _json_response(self, data: dict, status: int = 200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, code: int, message: str):
        self._json_response({"ok": False, "error": message}, status=code)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    if not SUPERVISOR_TOKEN:
        sys.stderr.write(
            "[wizard] WARNING: SUPERVISOR_TOKEN not set. "
            "The wizard cannot save add-on options without it.\n"
        )

    server = http.server.HTTPServer(("0.0.0.0", INGRESS_PORT), WizardHandler)
    sys.stdout.write(f"[wizard] Listening on port {INGRESS_PORT}\n")
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
