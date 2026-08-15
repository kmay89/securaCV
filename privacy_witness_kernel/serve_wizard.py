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
  GET  /api/preflight → check Mosquitto and Frigate prerequisites
  POST /api/verify  → run a quick health check after setup
  POST /api/restart-ha → restart Home Assistant core
"""

import http.server
import ipaddress
import json
import os
import re
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
API_TOKEN_FILE = Path("/config/api_token")
KERNEL_API_URL = "http://127.0.0.1:8799"
# Largest POST body the wizard accepts. Every wizard payload is a small JSON
# object; the cap only exists to fail closed on absurd declared lengths.
MAX_POST_BODY_BYTES = 1024 * 1024


# ---------------------------------------------------------------------------
# SSRF guard for user-supplied hosts (device pairing, camera reachability test)
# ---------------------------------------------------------------------------

# Names that resolve to internal Supervisor/host services. Blocking these keeps
# the pairing proxy and the camera-reachability test from being turned into a
# reachability/timing oracle for the add-on's own network neighborhood. The
# legitimate target of both is a LAN device (typically RFC1918), so private
# ranges are intentionally NOT blocked — only loopback, link-local, the
# unspecified address, and the internal service names.
_BLOCKED_HOST_NAMES = frozenset({
    "localhost", "ip6-localhost", "ip6-loopback",
    "supervisor", "hassio", "host.docker.internal",
})


def _is_blocked_host(host: str) -> bool:
    """True if `host` (a bare hostname or IP literal, no port) targets the
    add-on's own loopback/link-local/internal surface rather than a LAN device.

    Literal IPs are classified structurally (loopback / link-local /
    unspecified); known internal service names are blocked by name. DNS
    rebinding to a loopback address is a residual gap (we do not resolve here,
    to avoid blocking legitimate unresolved-at-config LAN names), but the
    device path this proxies to is a fixed, non-sensitive set.
    """
    if not host:
        return True
    name = host.strip().lower().rstrip(".")
    if name in _BLOCKED_HOST_NAMES:
        return True
    try:
        ip = ipaddress.ip_address(name)
    except ValueError:
        return False
    return ip.is_loopback or ip.is_link_local or ip.is_unspecified


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


def _canary_request(address: str, token: str, method: str, path: str,
                    data: dict | None = None) -> dict:
    """Make an authenticated request to a Canary device's REST API.

    Mirrors _supervisor_request but targets a per-request device address
    on the trusted LAN, authenticating with the device's bearer token.

    The address + token are supplied transiently by the pairing wizard
    (they are NOT persisted in the add-on config). Communication is
    plaintext HTTP on the local network — see the security note in
    spec/canary_mesh_network_v0.md §8.

    Returns the device's JSON response on success, or a dict shaped like
    {"ok": False, "error": ...} on any transport/HTTP error so the
    wizard front-end can branch on `ok` uniformly.

    The bearer token is NEVER logged.
    """
    if not address:
        return {"ok": False, "error": "Missing device address"}
    # SSRF hardening: the address is user-supplied (the LAN device the user is
    # pairing), but it must be a bare host[:port] (hostname or IPv4, optional
    # port). Reject anything carrying a scheme, path, query, fragment, or
    # embedded credentials so a crafted value can't redirect the request away
    # from the hardcoded `path`.
    if not re.fullmatch(r"[A-Za-z0-9.\-]+(?::\d{1,5})?", address):
        return {"ok": False, "error": "Invalid device address"}
    # SSRF guard: refuse the add-on's own loopback/link-local/internal surface
    # so this authenticated proxy can't be used as an internal reachability
    # oracle. The host is the part before an optional :port (the regex above
    # allows at most one colon and no colons inside the host).
    if _is_blocked_host(address.split(":", 1)[0]):
        return {"ok": False, "error": "Invalid device address"}
    url = f"http://{address}{path}"
    body = json.dumps(data).encode() if data is not None else None
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read().decode()
            return json.loads(raw) if raw else {"ok": True}
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode(errors="replace")
        return {"ok": False, "error": f"Device HTTP error {exc.code}: {body_text}"}
    except Exception as exc:
        return {"ok": False, "error": f"Device unreachable: {exc}"}


def _kernel_request(method: str, path: str) -> dict:
    """Proxy a request to the local witness API (same container).

    Re-reads the capability token file on every call — the API rotates the
    token each 10-minute bucket, so caching it would 401 after rotation.
    The token is never logged or returned to the browser.
    """
    try:
        token = API_TOKEN_FILE.read_text().strip()
    except OSError as exc:
        return {"ok": False, "error": f"API token unavailable: {exc}"}
    if not token:
        return {"ok": False, "error": "API token file is empty"}
    req = urllib.request.Request(
        f"{KERNEL_API_URL}{path}",
        method=method,
        headers={"x-witness-token": token},
    )
    try:
        # Generous timeout: POST /verify walks the whole sealed log.
        with urllib.request.urlopen(req, timeout=60) as resp:
            raw = resp.read().decode()
            return {"ok": True, "data": json.loads(raw) if raw else {}}
    except urllib.error.HTTPError as exc:
        # The kernel API returns {"error": "..."} bodies; surface them so
        # the panel shows an actionable message, not just a status code.
        detail = ""
        try:
            err_json = json.loads(exc.read().decode(errors="replace"))
            if isinstance(err_json, dict) and err_json.get("error"):
                detail = f": {err_json['error']}"
        except Exception:
            pass
        return {"ok": False, "error": f"kernel API error {exc.code}{detail}"}
    except Exception as exc:
        return {"ok": False, "error": f"kernel API unreachable: {exc}"}


def _kernel_download(path: str):
    """Proxy a raw download from the witness API (same container).

    Returns ((body_bytes, content_disposition), None) on success or
    (None, error_message) on failure. Same token handling as
    `_kernel_request`: re-read per call, never logged, never sent to the
    browser — the browser only ever sees the wizard's own endpoint.
    """
    try:
        token = API_TOKEN_FILE.read_text().strip()
    except OSError as exc:
        return None, f"API token unavailable: {exc}"
    if not token:
        return None, "API token file is empty"
    req = urllib.request.Request(
        f"{KERNEL_API_URL}{path}",
        method="GET",
        headers={"x-witness-token": token},
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            disposition = resp.headers.get("Content-Disposition", "")
            return (resp.read(), disposition), None
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            err_json = json.loads(exc.read().decode(errors="replace"))
            if isinstance(err_json, dict) and err_json.get("error"):
                detail = f": {err_json['error']}"
        except Exception:
            pass
        return None, f"kernel API error {exc.code}{detail}"
    except Exception as exc:
        return None, f"kernel API unreachable: {exc}"


def _export_query_suffix(query: str):
    """Validate the export window query string, failing closed.

    Returns ``(suffix, error)``. Only the exact window parameters ``last``,
    ``start`` and ``end`` may pass through to the kernel; any other name (a
    typo like ``las=24h``, an empty name, or an unknown key) rejects the whole
    request. Silently dropping the stray pair would forward a windowless
    ``/export/bundle`` and broaden the export to everything retained — the
    window is the user's privacy scope for the download.

    Allowlisted pairs are forwarded verbatim: the kernel stays the final
    validator (it rejects conflicting or malformed windows with 400
    ``bad_window``). An empty query forwards the bare path — the full export
    is the documented operator path.
    """
    if not query:
        return "", None
    for pair in query.split("&"):
        name = pair.split("=", 1)[0]
        if name not in ("last", "start", "end"):
            return None, f"unknown query parameter: {name or '(empty)'}"
    return f"?{query}", None


def get_addon_options() -> dict:
    resp = _supervisor_request("GET", "/addons/self/options/config")
    return resp.get("data", {})


def _public_status_options(opts: dict) -> dict:
    """Project the add-on options down to the non-secret fields the wizard
    panel actually renders.

    `/api/status` is reachable over the add-on's ingress port, which is bound
    on all interfaces inside the Supervisor's Docker network. The full options
    object carries the root `device_key_seed` (the sealed-log integrity secret)
    and MQTT passwords; returning it wholesale hands those to the browser (and
    to any co-resident container that reaches the port directly). The panel
    only reads mode / retention / broker host / topic prefix / HA-sensor
    enablement (see wizard/index.html), so we allowlist exactly those and drop
    everything else. This mirrors the deliberate secret-minimization already
    applied in `_discover_mqtt` / `_handle_preflight`.
    """
    frigate = opts.get("frigate") or {}
    mqtt_publish = opts.get("mqtt_publish") or {}
    return {
        "mode": opts.get("mode", ""),
        "retention_days": opts.get("retention_days"),
        "frigate": {
            # host is not a secret; the broker password is never surfaced.
            "mqtt_host": str(frigate.get("mqtt_host", "") or ""),
            "topic_prefix": str(frigate.get("topic_prefix", "") or ""),
        },
        "mqtt_publish": {
            "enabled": bool(mqtt_publish.get("enabled", True)),
        },
    }


def set_addon_options(options: dict) -> None:
    _supervisor_request("POST", "/addons/self/options", {"options": options})


def restart_addon() -> None:
    _supervisor_request("POST", "/addons/self/restart")


def _discover_mqtt() -> dict:
    """Query the Supervisor services API for the shared MQTT broker.

    Returns {found, host, port, username}. Requires the add-on to declare
    `services: ["mqtt:want"]` in config.yaml. The broker password is
    deliberately NOT returned: the wizard never needs it (run.sh
    re-discovers credentials at startup), so the secret never enters this
    process's data flow, a response, or a generated file.
    """
    try:
        resp = _supervisor_request("GET", "/services/mqtt")
        data = resp.get("data", {})
        if data.get("host"):
            return {
                "found": True,
                "host": str(data.get("host", "")),
                "port": int(data.get("port") or 1883),
                "username": str(data.get("username") or ""),
            }
    except Exception:
        pass
    return {"found": False, "host": "", "port": 1883, "username": ""}


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

        # SSRF guard: don't let a "test camera" become an internal port scanner
        # aimed at the add-on's own loopback/link-local/service surface.
        if _is_blocked_host(host):
            return {"ok": False, "error": "Camera address not allowed"}
        if not (0 < port < 65536):
            return {"ok": False, "error": "Invalid camera port"}

        try:
            sock = socket.create_connection((host, port), timeout=5)
            sock.close()
        except Exception:
            # Generic message on purpose: distinguishing refused vs. timed-out
            # vs. unreachable would leak an open/closed/filtered oracle.
            return {"ok": False, "host": host, "port": port,
                    "error": "Camera not reachable at this address"}
        return {"ok": True, "host": host, "port": port}
    except Exception:
        return {"ok": False, "error": "Invalid camera URL"}


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
        if path == "/api/preflight":
            self._json_response(self._handle_preflight())
            return
        if path == "/api/kernel/digest":
            self._json_response(_kernel_request("GET", "/digest"))
            return

        # One-click "Download my events": proxy the kernel's signed export
        # bundle through to the browser as a file download. The window is the
        # user's privacy scope, so the proxy fails closed: anything outside
        # the exact {last, start, end} allowlist rejects the request instead
        # of silently broadening the export.
        base, _, query = path.partition("?")
        if base == "/api/kernel/export":
            suffix, query_err = _export_query_suffix(query)
            if query_err is not None:
                self._json_response(
                    {"ok": False, "error": "bad_window", "detail": query_err},
                    status=400,
                )
                return
            payload, err = _kernel_download(f"/export/bundle{suffix}")
            if err:
                self._json_response({"ok": False, "error": err}, status=502)
                return
            body, disposition = payload
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header(
                "Content-Disposition",
                disposition or 'attachment; filename="securacv-events.json"',
            )
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return

        # Serve static files from wizard dir. The request path is
        # attacker-controlled, so resolve it and refuse anything that
        # escapes WIZARD_DIR (e.g. via ../ segments or symlinks).
        base_dir = os.path.realpath(WIZARD_DIR)
        requested = os.path.realpath(os.path.join(base_dir, path.lstrip("/")))
        if requested.startswith(base_dir + os.sep) and os.path.isfile(requested):
            self._serve_file(Path(requested))
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

        # Content-Length is attacker-controlled: reject junk and negatives
        # with a clean 400 (not an unhandled ValueError), and cap the body so
        # a huge declared length can't balloon memory. Wizard bodies are tiny.
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            self._json_response(
                {"ok": False, "error": "invalid Content-Length"}, status=400
            )
            return
        if length < 0:
            self._json_response(
                {"ok": False, "error": "invalid Content-Length"}, status=400
            )
            return
        if length > MAX_POST_BODY_BYTES:
            self._json_response(
                {"ok": False, "error": "request body too large"}, status=413
            )
            return
        body = self.rfile.read(length) if length else b""

        try:
            payload = json.loads(body) if body else {}
        except json.JSONDecodeError:
            self._json_response({"ok": False, "error": "Invalid JSON"}, status=400)
            return

        if path == "/api/save":
            self._json_response(self._handle_save(payload))
        elif path == "/api/kernel/verify":
            self._json_response(_kernel_request("POST", "/verify"))
        elif path == "/api/test-camera":
            self._json_response(self._handle_test_camera(payload))
        elif path == "/api/verify":
            self._json_response(self._handle_verify())
        elif path == "/api/restart-ha":
            self._json_response(self._handle_restart_ha())
        # ── "Add another Canary" pairing-wizard device proxies ──────────
        # Each reads {address, token} from the body and forwards to the
        # named device on the trusted LAN. address+token are transient
        # (never persisted, never logged). All four pairing steps forward
        # a POST; status/peers forward a GET (GET-with-body is awkward in
        # this server, so the wizard POSTs {address, token} here and we
        # translate to the device's GET).
        elif path == "/api/mesh/pair/start":
            self._json_response(self._handle_mesh_proxy(payload, "POST", "/api/mesh/pair/start"))
        elif path == "/api/mesh/pair/join":
            self._json_response(self._handle_mesh_proxy(payload, "POST", "/api/mesh/pair/join"))
        elif path == "/api/mesh/pair/confirm":
            self._json_response(self._handle_mesh_proxy(payload, "POST", "/api/mesh/pair/confirm"))
        elif path == "/api/mesh/pair/cancel":
            self._json_response(self._handle_mesh_proxy(payload, "POST", "/api/mesh/pair/cancel"))
        elif path == "/api/mesh/status":
            self._json_response(self._handle_mesh_proxy(payload, "GET", "/api/mesh"))
        elif path == "/api/mesh/peers":
            self._json_response(self._handle_mesh_proxy(payload, "GET", "/api/mesh/peers"))
        else:
            self._send_error(404, "Unknown endpoint")

    # ------------------------------------------------------------------
    def _handle_mesh_proxy(self, payload: dict, method: str, device_path: str) -> dict:
        """Forward a mesh/pairing call to a Canary device.

        Resolves {address, token} from the wizard request body (NOT from
        add-on config — these come from the wizard form per-request). For
        POST forwards an empty JSON body to the device (the device reads
        nothing else from the body); for GET no body is sent. The bearer
        token is never logged.
        """
        address = payload.get("address", "")
        token = payload.get("token", "")
        if not address:
            return {"ok": False, "error": "Missing device address"}
        data = {} if method == "POST" else None
        return _canary_request(address, token, method, device_path, data)

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
            # Never return the raw options: they carry device_key_seed and MQTT
            # passwords. Surface only the non-secret fields the panel renders.
            return {
                "ok": True,
                "configured": configured,
                "options": _public_status_options(opts),
            }
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

            # Persist device key to file for the install script's backing-up
            # message. Created 0600 from the start — write-then-chmod would
            # leave a umask-dependent window where the key is world-readable.
            DEVICE_KEY_FILE.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(
                DEVICE_KEY_FILE,
                os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                0o600,
            )
            with os.fdopen(fd, "w") as key_file:
                key_file.write(device_key_seed + "\n")
            DEVICE_KEY_FILE.chmod(0o600)  # tighten pre-existing files too

            # Broker settings: an explicit payload override (external broker)
            # is persisted verbatim; otherwise save empty values, which mean
            # "auto-discover from the Supervisor MQTT service at startup" in
            # run.sh — credentials are then always fresh and never stored.
            mqtt_override = payload.get("mqtt") or {}
            mqtt_host = str(mqtt_override.get("host", "") or "").strip()
            mqtt_port = int(mqtt_override.get("port") or 1883)
            mqtt_username = str(mqtt_override.get("username", "") or "")
            mqtt_password = str(mqtt_override.get("password", "") or "")

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
                    "host": mqtt_host,
                    "port": mqtt_port,
                    "username": mqtt_username,
                    "password": mqtt_password,
                    "topic_prefix": "witness",
                    "discovery_prefix": "homeassistant",
                },
            }

            if mode == "frigate":
                options["frigate"] = {
                    "mqtt_host": mqtt_host,
                    "mqtt_port": mqtt_port,
                    "mqtt_topic": "",
                    "topic_prefix": str(payload.get("frigate_topic_prefix", "") or "frigate"),
                    "enable_reviews": bool(payload.get("enable_reviews", False)),
                    "mqtt_username": mqtt_username,
                    "mqtt_password": mqtt_password,
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
                    "mqtt_host": mqtt_host,
                    "mqtt_port": mqtt_port,
                    "mqtt_topic": "",
                    "topic_prefix": "frigate",
                    "enable_reviews": False,
                    "mqtt_username": mqtt_username,
                    "mqtt_password": mqtt_password,
                    "min_confidence": 0.5,
                    "cameras": [],
                    "labels": [],
                }

            set_addon_options(options)

            # Write Frigate camera config so Frigate knows about the cameras
            if mode == "frigate" and cameras:
                frigate_conf = Path("/config/frigate.yml")
                if not frigate_conf.exists() or "PLACEHOLDER" in frigate_conf.read_text():
                    discovered = _discover_mqtt()
                    broker_host = mqtt_host or discovered["host"] or "core-mosquitto"
                    broker_port = mqtt_port if mqtt_host else (discovered["port"] or 1883)
                    # Frigate needs broker credentials too, or it can't
                    # publish the events SecuraCV ingests. The generated
                    # config uses env-substitution placeholders (no secret
                    # is written) — see _write_frigate_config.
                    needs_auth = bool(
                        mqtt_username or (not mqtt_host and discovered["username"])
                    )
                    self._write_frigate_config(
                        cameras,
                        retention_days,
                        broker_host,
                        broker_port,
                        needs_auth,
                    )

            # Restart the add-on to apply changes
            try:
                restart_addon()
            except Exception:
                pass

            return {
                "ok": True,
                "message": "Configuration saved. Restarting Privacy Witness Kernel…",
                "device_key_seed": device_key_seed,
            }
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def _write_frigate_config(
        self,
        cameras: list,
        retention_days: int,
        broker_host: str = "core-mosquitto",
        broker_port: int = 1883,
        needs_auth: bool = False,
    ) -> None:
        """Generate a Frigate config from wizard-collected camera data.

        Credentials are never written in clear text: when the broker needs
        auth, the config uses Frigate's documented `{}` env substitution —
        the Frigate HA add-on injects FRIGATE_MQTT_USER/FRIGATE_MQTT_PASSWORD
        from the Supervisor MQTT service automatically. External-broker
        users replace the placeholders with their own credentials.
        """
        lines = [
            "# Frigate NVR configuration — generated by SecuraCV setup wizard",
            "",
            "mqtt:",
            "  enabled: true",
            f"  host: {broker_host}",
            f"  port: {broker_port}",
        ]
        if needs_auth:
            lines += [
                "  # The Frigate HA add-on injects these from the Supervisor MQTT",
                "  # service. For an external broker, replace them with your own.",
                "  user: '{FRIGATE_MQTT_USER}'",
                "  password: '{FRIGATE_MQTT_PASSWORD}'",
            ]
        lines += [
            "",
            "cameras:",
        ]
        for cam in cameras:
            name = cam.get("name", "camera").lower().replace(" ", "_")
            name = "".join(c for c in name if c.isalnum() or c == "_")
            url = cam.get("url", "")
            if not url:
                continue
            # Sanitize URL: take only the first line to prevent YAML injection
            # via embedded newlines corrupting the generated config.
            url = url.splitlines()[0].strip() if url else ""
            # The URL becomes an ffmpeg input. Restrict the scheme to the camera
            # protocols Frigate expects so a crafted value can't select an
            # arbitrary ffmpeg protocol handler; skip anything else.
            if not re.match(r"^(?:rtsp|rtsps|http|https)://", url, re.IGNORECASE):
                continue
            # Emit as a single-quoted YAML scalar (doubling any embedded quote)
            # so the value can't break out of the scalar and inject YAML keys.
            yaml_url = "'" + url.replace("'", "''") + "'"
            lines += [
                f"  {name}:",
                "    ffmpeg:",
                "      inputs:",
                f"        - path: {yaml_url}",
                "          roles:",
                "            - detect",
                "            - record",
                "    detect:",
                "      width: 1280",
                "      height: 720",
                "      fps: 5",
                "    record:",
                "      enabled: true",
                "      retain:",
                f"        days: {retention_days}",
                "        mode: motion",
                "",
            ]
        lines += [
            "detectors:",
            "  cpu1:",
            "    type: cpu",
            "    num_threads: 2",
        ]
        Path("/config/frigate.yml").write_text("\n".join(lines) + "\n")

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

    def _handle_preflight(self) -> dict:
        """Check Mosquitto/Frigate prerequisites and MQTT auto-discovery."""
        checks = {}
        try:
            resp = _supervisor_request("GET", "/addons/core_mosquitto/info")
            checks["mosquitto"] = resp.get("data", {}).get("state") == "started"
        except Exception:
            checks["mosquitto"] = False
        try:
            resp = _supervisor_request("GET", "/addons/blakeblackshear_frigate.frigate/info")
            state = resp.get("data", {}).get("state", "")
            checks["frigate"] = state in ("started", "stopped")
            checks["frigate_running"] = state == "started"
        except Exception:
            checks["frigate"] = False
            checks["frigate_running"] = False
        # MQTT service discovery: report presence + non-secret details only
        # (the password stays server-side; run.sh re-discovers it at startup).
        mqtt = _discover_mqtt()
        checks["mqtt_service"] = mqtt["found"]
        checks["mqtt_host"] = mqtt["host"]
        checks["mqtt_port"] = mqtt["port"]
        checks["mqtt_auth"] = bool(mqtt["username"])
        return {"ok": True, "checks": checks}

    def _handle_restart_ha(self) -> dict:
        """Restart Home Assistant core."""
        try:
            _supervisor_request("POST", "/core/restart")
            return {"ok": True}
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
