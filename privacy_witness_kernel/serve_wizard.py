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
  POST /api/preflight/install → one-click install of a missing prerequisite
  GET  /api/discover-cameras → cameras discovered from go2rtc, pre-fillable
  POST /api/verify  → run a quick health check after setup
  POST /api/restart-ha → restart Home Assistant core

All URLs the wizard page requests are joined relative to the page's own
ingress path, so this server only ever sees bare paths ("/api/save").
Query strings are stripped before routing.
"""

import http.server
import ipaddress
import json
import os
import re
import socket
import sys
import threading
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
# Wizard preferences the add-on options schema has no field for (digest
# time, alert toggles). Persisted so a re-run of the wizard and the done
# screen reflect what the user actually chose.
WIZARD_PREFS_FILE = Path("/config/.securacv/wizard_prefs.json")
# Supervisor slug + store repository of the Frigate add-on.
FRIGATE_ADDON_SLUG = "ccab4aaf_frigate"
FRIGATE_REPO_URL = "https://github.com/blakeblackshear/frigate-hass-addons"
# The modern Frigate add-on reads its config from /addon_configs/<slug>/
# (mounted via `all_addon_configs:rw` in config.yaml). The legacy path is
# only a fallback for old Frigate versions.
FRIGATE_ADDON_CONFIG_DIR = Path("/addon_configs") / FRIGATE_ADDON_SLUG
LEGACY_FRIGATE_CONF = Path("/config/frigate.yml")
# Daily-digest automation: blueprint installed by scripts/install.sh, wired
# through the HA core API proxy under a stable automation object id.
DIGEST_BLUEPRINT_FILE = Path(
    "/config/blueprints/automation/securacv/securacv_daily_digest.yaml"
)
DIGEST_AUTOMATION_ID = "securacv_daily_digest"
# Default go2rtc endpoint when the add-on options carry none.
GO2RTC_DEFAULT_URL = "http://homeassistant.local:1984"
# Delay between flushing the /api/save response and firing the add-on
# restart. The restart kills this process, so the response MUST be on the
# wire first or the browser reports a failure for a successful save.
RESTART_DELAY_SECONDS = 1.0
# Largest POST body the wizard accepts. Every wizard payload is a small JSON
# object; the cap only exists to fail closed on absurd declared lengths.
MAX_POST_BODY_BYTES = 1024 * 1024

# The only legitimate clients of this port: the Supervisor's ingress proxy
# (requests that passed Home Assistant's own authentication always originate
# from 172.30.32.2) and the add-on's own loopback. The server binds 0.0.0.0 on
# the Supervisor Docker network, so without this gate any co-resident container
# could rewrite the add-on options — including installing a device_key_seed it
# knows and forging the sealed log from then on — or use the wizard's proxies.
# This is the same adversary `_public_status_options` hardens the read path
# against; the write path must refuse it outright.
INGRESS_PROXY_IP = "172.30.32.2"


def _trusted_client_ips() -> frozenset:
    extra = os.environ.get("WIZARD_TRUSTED_CLIENT_IPS", "")
    ips = {INGRESS_PROXY_IP, "127.0.0.1", "::1"}
    ips.update(ip.strip() for ip in extra.split(",") if ip.strip())
    return frozenset(ips)


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
        # `ipaddress` is strict (canonical dotted-quad only), but the OS socket
        # resolver accepts non-canonical numeric forms — "127.1", "0", octal
        # "0177.0.0.1", hex "0x7f.0.0.1", decimal "2130706433" — and maps them
        # to real IPv4 addresses. A guard that only consults `ipaddress` treats
        # those as unresolved hostnames and lets them through, while the actual
        # connect() reaches loopback: a parser differential an attacker uses to
        # bypass this guard. Normalize anything `inet_aton` accepts (the same
        # forms the OS will) to canonical form and re-classify. A genuine LAN
        # hostname fails inet_aton and stays an unresolved name (not blocked).
        try:
            ip = ipaddress.ip_address(socket.inet_ntoa(socket.inet_aton(name)))
        except (OSError, ValueError):
            return False
    return ip.is_loopback or ip.is_link_local or ip.is_unspecified


# ---------------------------------------------------------------------------
# Supervisor API helpers
# ---------------------------------------------------------------------------

def _supervisor_request(
    method: str, path: str, data: dict | None = None, timeout: int = 10
):
    """Make an authenticated request to the HA Supervisor API.

    `timeout` is overridable because store installs legitimately take
    minutes (image pulls on a Pi). Returns the parsed JSON body — usually a
    dict, but the /core/api/* proxy passes core responses through verbatim
    (GET /core/api/services returns a list).
    """
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
        with urllib.request.urlopen(req, timeout=timeout) as resp:
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
# Deferred add-on restart (save-race fix)
# ---------------------------------------------------------------------------

def _restart_addon_soon() -> None:
    """Fire the add-on restart AFTER the /api/save response has left the
    socket. The restart tears this process down, so triggering it before
    the response is written makes the browser report a network failure for
    a save that succeeded. A short daemon Timer decouples the two."""

    def _do_restart() -> None:
        try:
            restart_addon()
        except Exception as exc:
            sys.stderr.write(f"[wizard] deferred add-on restart failed: {exc}\n")

    timer = threading.Timer(RESTART_DELAY_SECONDS, _do_restart)
    timer.daemon = True
    timer.start()


# ---------------------------------------------------------------------------
# Wizard preferences (settings the add-on options schema has no field for)
# ---------------------------------------------------------------------------

def _persist_wizard_prefs(prefs: dict) -> None:
    """Persist the wizard's digest/alert choices to /config/.securacv/.

    The add-on options schema deliberately has no fields for these (they
    configure HA-side automations, not the kernel), but silently discarding
    them made the done screen dishonest. Best-effort: a failure is logged,
    never fatal to the save."""
    try:
        WIZARD_PREFS_FILE.parent.mkdir(parents=True, exist_ok=True)
        WIZARD_PREFS_FILE.write_text(
            json.dumps(prefs, indent=2, sort_keys=True) + "\n"
        )
    except OSError as exc:
        sys.stderr.write(f"[wizard] could not persist wizard prefs: {exc}\n")


def load_wizard_prefs() -> dict:
    """Read back persisted wizard preferences ({} when absent/corrupt)."""
    try:
        data = json.loads(WIZARD_PREFS_FILE.read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


# ---------------------------------------------------------------------------
# Daily digest automation (created for real, or honestly skipped)
# ---------------------------------------------------------------------------

def _detect_notify_service() -> str:
    """Find a mobile-app notify service via the core API proxy.

    Returns e.g. "notify.mobile_app_pixel_8" or "" when none exists. The
    Home Assistant companion app registers its service as
    notify.mobile_app_<device>; sorted order keeps the pick deterministic."""
    try:
        resp = _supervisor_request("GET", "/core/api/services")
    except Exception:
        return ""
    if not isinstance(resp, list):
        return ""
    for domain in resp:
        if not isinstance(domain, dict) or domain.get("domain") != "notify":
            continue
        services = domain.get("services") or {}
        for name in sorted(services):
            if name.startswith("mobile_app"):
                return f"notify.{name}"
    return ""


def _normalize_digest_time(value: str) -> str:
    """Normalize the wizard's HH:MM time input to HH:MM:SS, or "" if the
    value is not a valid time (fail closed rather than create a broken
    automation)."""
    value = str(value or "").strip()
    if re.fullmatch(r"([01]\d|2[0-3]):[0-5]\d", value):
        return value + ":00"
    if re.fullmatch(r"([01]\d|2[0-3]):[0-5]\d:[0-5]\d", value):
        return value
    return ""


def _setup_digest_automation(digest_time: str) -> dict:
    """Create the daily-digest automation through the HA core API proxy.

    Returns {"created": bool, "detail": str} and the done screen shows the
    detail either way — the wizard never promises a digest it did not set
    up. Skips honestly when the blueprint file is missing (installer not
    run), no mobile-app notify service exists, or the time is invalid."""
    if not DIGEST_BLUEPRINT_FILE.is_file():
        return {
            "created": False,
            "detail": (
                "digest blueprint not installed "
                f"(expected {DIGEST_BLUEPRINT_FILE}); run the SecuraCV "
                "installer or import the blueprint manually, then add the "
                "automation from Settings → Automations → Blueprints"
            ),
        }
    at_time = _normalize_digest_time(digest_time)
    if not at_time:
        return {"created": False, "detail": f"invalid digest time {digest_time!r}"}
    notify_service = _detect_notify_service()
    if not notify_service:
        return {
            "created": False,
            "detail": (
                "no mobile-app notify service found — install the Home "
                "Assistant companion app on your phone, then create the "
                "digest automation from Settings → Automations → Blueprints"
            ),
        }
    try:
        _supervisor_request(
            "POST",
            f"/core/api/config/automation/config/{DIGEST_AUTOMATION_ID}",
            {
                "alias": "SecuraCV daily digest",
                "description": "Created by the SecuraCV setup wizard.",
                "use_blueprint": {
                    "path": "securacv/securacv_daily_digest.yaml",
                    "input": {
                        "digest_time": at_time,
                        "notify_service": notify_service,
                    },
                },
            },
        )
    except Exception as exc:
        return {"created": False, "detail": f"automation API error: {exc}"}
    return {
        "created": True,
        "detail": f"daily digest at {at_time[:5]} via {notify_service}",
    }


# ---------------------------------------------------------------------------
# Frigate config destination (modern add-on path vs. legacy fallback)
# ---------------------------------------------------------------------------

def _frigate_config_destination() -> tuple:
    """Pick where the generated Frigate config goes.

    The modern Frigate add-on reads /addon_configs/ccab4aaf_frigate/config.yml
    and IGNORES /config/frigate.yml. Returns (path_or_None, note):

    * add-on config dir exists, no config yet    → write config.yml there
    * add-on config dir has config.yml/.yaml     → NEVER overwrite; write
      config.yml.new beside it and say so
    * no add-on config dir (Frigate not installed / legacy mount) → legacy
      /config/frigate.yml, with a note naming both locations
    * legacy file already exists with real content → leave it, write nothing

    The note is surfaced verbatim on the wizard's done screen."""
    if FRIGATE_ADDON_CONFIG_DIR.is_dir():
        for existing_name in ("config.yml", "config.yaml"):
            existing = FRIGATE_ADDON_CONFIG_DIR / existing_name
            if existing.exists():
                new_path = FRIGATE_ADDON_CONFIG_DIR / "config.yml.new"
                return new_path, (
                    f"Frigate already has a config at {existing} — it was "
                    f"left untouched. The wizard's suggested camera config "
                    f"was written to {new_path}; merge or rename it if you "
                    f"want to use it, then restart Frigate."
                )
        target = FRIGATE_ADDON_CONFIG_DIR / "config.yml"
        return target, (
            f"Frigate camera config written to {target} — Frigate reads it "
            f"on its next start."
        )
    if LEGACY_FRIGATE_CONF.exists() and "PLACEHOLDER" not in LEGACY_FRIGATE_CONF.read_text():
        return None, (
            f"Existing Frigate config at {LEGACY_FRIGATE_CONF} was left "
            f"untouched. Note that the current Frigate add-on reads "
            f"{FRIGATE_ADDON_CONFIG_DIR / 'config.yml'}, not this legacy path."
        )
    return LEGACY_FRIGATE_CONF, (
        f"Frigate add-on config directory {FRIGATE_ADDON_CONFIG_DIR} not "
        f"found, so the camera config was written to the legacy path "
        f"{LEGACY_FRIGATE_CONF}. The current Frigate add-on reads "
        f"{FRIGATE_ADDON_CONFIG_DIR / 'config.yml'} — after installing "
        f"Frigate, copy the file there."
    )


# ---------------------------------------------------------------------------
# go2rtc camera discovery (pre-fills the wizard's camera rows)
# ---------------------------------------------------------------------------

def _go2rtc_streams_to_cameras(streams) -> list:
    """Transform a go2rtc /api/streams payload into wizard camera rows.

    Producers are objects carrying a "url" field (older go2rtc builds
    emitted plain strings; both are handled). Streams without a usable
    producer URL are skipped. Zone IDs must match zone:[a-z0-9_-]{1,64},
    so the name is lowercased BEFORE the character sweep."""
    if not isinstance(streams, dict):
        return []
    cameras = []
    for name in sorted(streams):
        stream = streams.get(name)
        producers = (stream or {}).get("producers") if isinstance(stream, dict) else None
        urls = []
        for producer in producers or []:
            if isinstance(producer, dict):
                url = str(producer.get("url") or "")
            else:
                url = str(producer or "")
            if url:
                urls.append(url)
        if not urls:
            continue
        chosen = next((u for u in urls if u.startswith("rtsp://")), urls[0])
        slug = re.sub(r"[^a-z0-9_-]", "_", str(name).lower())[:64]
        cameras.append({
            "name": str(name),
            "url": chosen,
            "zone_id": f"zone:{slug}",
        })
    return cameras


def _discover_go2rtc_cameras() -> list:
    """Query go2rtc (URL from the add-on options; operator-controlled, never
    from the browser) and return pre-fillable camera rows. Empty list on any
    failure — discovery is a convenience, never a blocker."""
    go2rtc_url = GO2RTC_DEFAULT_URL
    try:
        opts = get_addon_options()
        go2rtc_url = str(opts.get("go2rtc_url") or GO2RTC_DEFAULT_URL)
    except Exception:
        pass
    try:
        req = urllib.request.Request(go2rtc_url.rstrip("/") + "/api/streams")
        with urllib.request.urlopen(req, timeout=5) as resp:
            streams = json.loads(resp.read().decode())
    except Exception:
        return []
    return _go2rtc_streams_to_cameras(streams)


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
    def _refuse_untrusted_client(self) -> bool:
        """True (after sending 403) unless the connection came through the
        Supervisor ingress proxy or the add-on's own loopback."""
        client_ip = (self.client_address or ("",))[0]
        if client_ip in _trusted_client_ips():
            return False
        sys.stderr.write(
            f"[wizard] refused direct connection from {client_ip} "
            "(only Supervisor ingress and loopback are trusted)\n"
        )
        self._json_response(
            {"ok": False, "error": "forbidden: use the Home Assistant ingress panel"},
            status=403,
        )
        return True

    # ------------------------------------------------------------------
    def do_GET(self):
        if self._refuse_untrusted_client():
            return
        # Strip the query string BEFORE routing: paths are matched by exact
        # string compare, so "/api/status?ts=1" must still route to the
        # status handler (it previously fell through to a 404/static file).
        # Under HA ingress the Supervisor already delivers the bare path —
        # no ingress prefix ever reaches this server.
        path, _, query = self.path.partition("?")
        if not path or path == "/":
            path = "/index.html"

        # API: current add-on status/options
        if path == "/api/status":
            self._json_response(self._handle_status())
            return
        if path == "/api/preflight":
            self._json_response(self._handle_preflight())
            return
        if path == "/api/discover-cameras":
            self._json_response({"ok": True, "cameras": _discover_go2rtc_cameras()})
            return
        if path == "/api/kernel/digest":
            self._json_response(_kernel_request("GET", "/digest"))
            return

        # One-click "Download my events": proxy the kernel's signed export
        # bundle through to the browser as a file download. The window is the
        # user's privacy scope, so the proxy fails closed: anything outside
        # the exact {last, start, end} allowlist rejects the request instead
        # of silently broadening the export.
        if path == "/api/kernel/export":
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
        if self._refuse_untrusted_client():
            return
        # Query strings are stripped before the exact-match routing below,
        # same as do_GET.
        path, _, _query = self.path.partition("?")

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
            # ORDER MATTERS: write + flush the response BEFORE scheduling the
            # add-on restart. The restart kills this process; firing it first
            # made the browser report a network failure for a successful save.
            result = self._handle_save(payload)
            self._json_response(result)
            if result.get("ok"):
                _restart_addon_soon()
        elif path == "/api/preflight/install":
            self._json_response(self._handle_preflight_install(payload))
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
          "digest_enabled": bool,
          "digest_time": "HH:MM",
          "pattern_alerts": bool,
          "integrity_alerts": bool,
          "device_key_seed": str  (optional; generated if omitted)
        }

        Does NOT restart the add-on itself: do_POST flushes the response
        first and then schedules the restart (_restart_addon_soon), so the
        browser sees the success before this process dies.

        The response reports what really happened — where the Frigate config
        landed (frigate_config) and whether the daily-digest automation was
        created (digest.created / digest.detail) — so the done screen never
        promises something that was silently skipped.
        """
        try:
            mode = payload.get("mode", "frigate")
            cameras = payload.get("cameras", [])
            retention_days = int(payload.get("retention_days", 1))
            device_key_seed = payload.get("device_key_seed", "").strip()

            # The sealed log's identity is bound to the existing seed, so a
            # reconfigure must never silently mint or swap it. Omitted seed +
            # existing key -> reuse the existing identity. A DIFFERENT seed
            # replaces an existing one only with proof of knowledge of the
            # current seed — otherwise any client that reaches this endpoint
            # could install a key it knows and forge the log from then on.
            existing_seed = ""
            if DEVICE_KEY_FILE.exists():
                existing_seed = DEVICE_KEY_FILE.read_text().strip()

            if not device_key_seed:
                if existing_seed:
                    device_key_seed = existing_seed
                else:
                    import secrets
                    device_key_seed = secrets.token_hex(32)
            elif existing_seed and device_key_seed != existing_seed:
                confirm = str(payload.get("current_device_key_seed", "") or "").strip()
                if confirm != existing_seed:
                    return {
                        "ok": False,
                        "error": (
                            "a device key already exists; replacing it would "
                            "orphan the sealed log. Pass the current seed as "
                            "current_device_key_seed to authorize the change."
                        ),
                    }

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

            # Persist the wizard-only preferences the options schema has no
            # field for, so re-runs and the done screen stay truthful.
            digest_enabled = bool(payload.get("digest_enabled", True))
            digest_time = str(payload.get("digest_time", "") or "")
            _persist_wizard_prefs({
                "digest_enabled": digest_enabled,
                "digest_time": digest_time,
                "pattern_alerts": bool(payload.get("pattern_alerts", False)),
                "integrity_alerts": bool(payload.get("integrity_alerts", True)),
            })

            # Daily digest: create the automation for real (through the core
            # API proxy) or report honestly why it was skipped.
            if digest_enabled:
                digest_result = _setup_digest_automation(digest_time)
            else:
                digest_result = {"created": False, "detail": "digest disabled in the wizard"}

            # Write the Frigate camera config where Frigate actually reads
            # it (never overwriting an existing config — see
            # _frigate_config_destination for the full decision).
            frigate_note = ""
            if mode == "frigate" and cameras:
                target, frigate_note = _frigate_config_destination()
                if target is not None:
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
                        dest=target,
                    )

            # NOTE: the add-on restart is deliberately NOT triggered here —
            # do_POST schedules it (_restart_addon_soon) after the response is
            # flushed.
            #
            # The seed is deliberately NOT echoed back: the wizard UI already
            # holds the seed it submitted (it generates one client-side), and
            # a server-generated seed is recoverable from the 0600 key file —
            # while an echoed copy would hand the device identity to anything
            # that can read this response.
            return {
                "ok": True,
                "message": "Configuration saved. Restarting Privacy Witness Kernel…",
                "device_key_file": str(DEVICE_KEY_FILE),
                "digest": digest_result,
                "frigate_config": frigate_note,
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
        dest: Path | None = None,
    ) -> None:
        """Generate a Frigate config from wizard-collected camera data.

        `dest` comes from _frigate_config_destination (modern add-on config
        dir when mounted, legacy /config/frigate.yml otherwise).

        Credentials are never written in clear text: when the broker needs
        auth, the config uses Frigate's documented `{}` env substitution —
        the Frigate HA add-on injects FRIGATE_MQTT_USER/FRIGATE_MQTT_PASSWORD
        from the Supervisor MQTT service automatically. External-broker
        users replace the placeholders with their own credentials.
        """
        # Emit a string as a single-quoted YAML scalar (doubling embedded
        # quotes), the same treatment the camera URL below gets. `broker_host`
        # is wizard-supplied and was previously interpolated raw — a value
        # carrying a newline or YAML metacharacters could inject arbitrary keys
        # into frigate.yml. Also take only the first line, defense in depth.
        def yaml_scalar(value: str) -> str:
            first_line = str(value).splitlines()[0] if str(value) else ""
            return "'" + first_line.replace("'", "''") + "'"

        lines = [
            "# Frigate NVR configuration — generated by SecuraCV setup wizard",
            "",
            "mqtt:",
            "  enabled: true",
            f"  host: {yaml_scalar(broker_host)}",
            f"  port: {int(broker_port)}",
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
        (dest if dest is not None else LEGACY_FRIGATE_CONF).write_text(
            "\n".join(lines) + "\n"
        )

    def _handle_test_camera(self, payload: dict) -> dict:
        url = payload.get("url", "").strip()
        if not url:
            return {"ok": False, "error": "No URL provided"}
        return test_camera_tcp(url)

    def _handle_verify(self) -> dict:
        """Quick health check: is the witness API reachable?"""
        try:
            req = urllib.request.Request("http://127.0.0.1:8799/health")
            with urllib.request.urlopen(req, timeout=5) as resp:
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
            resp = _supervisor_request("GET", f"/addons/{FRIGATE_ADDON_SLUG}/info")
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

    def _handle_preflight_install(self, payload: dict) -> dict:
        """One-click install of a missing prerequisite (Mosquitto/Frigate).

        The target is a fixed enum, never a URL or slug from the browser:
        every request this method makes goes through _supervisor_request,
        whose base is the hardcoded http://supervisor — the wizard cannot
        be steered at any other host (SSRF posture matches the rest of the
        server). Anything but the two known targets is refused before any
        network activity.

        Store installs pull container images, so the timeouts are long;
        the wizard UI narrates while this request is in flight.
        """
        target = payload.get("target", "")
        if target == "mosquitto":
            steps = []
            try:
                _supervisor_request(
                    "POST", "/store/addons/core_mosquitto/install", timeout=600
                )
                steps.append("installed core_mosquitto")
                _supervisor_request(
                    "POST", "/addons/core_mosquitto/start", timeout=120
                )
                steps.append("started core_mosquitto")
            except Exception as exc:
                return {"ok": False, "target": target, "steps": steps,
                        "error": str(exc)}
            return {
                "ok": True,
                "target": target,
                "steps": steps,
                "message": (
                    "Mosquitto MQTT broker installed and started. Broker "
                    "credentials are auto-discovered — nothing to type."
                ),
            }
        if target == "frigate":
            steps = []
            # Adding a repository that is already registered fails harmlessly;
            # the install below is the real test, so this step is best-effort.
            try:
                _supervisor_request(
                    "POST", "/store/repositories",
                    {"repository": FRIGATE_REPO_URL}, timeout=120,
                )
                steps.append("added Frigate add-on repository")
            except Exception:
                steps.append("Frigate add-on repository already present")
            try:
                _supervisor_request(
                    "POST", f"/store/addons/{FRIGATE_ADDON_SLUG}/install",
                    timeout=600,
                )
                steps.append(f"installed {FRIGATE_ADDON_SLUG}")
            except Exception as exc:
                return {"ok": False, "target": target, "steps": steps,
                        "error": str(exc)}
            return {
                "ok": True,
                "target": target,
                "steps": steps,
                "message": (
                    "Frigate NVR installed. It is not started yet — finish "
                    "this wizard first (it writes Frigate's camera config), "
                    "then start Frigate from Settings → Apps."
                ),
            }
        return {"ok": False, "error": "unknown install target"}

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
        # Push the response onto the wire NOW: /api/save schedules an add-on
        # restart right after this returns, and the browser must have the
        # bytes before the process goes down.
        try:
            self.wfile.flush()
        except OSError:
            pass

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

    # ThreadingHTTPServer, not HTTPServer: a slow request (a 60 s chain
    # verify, a minutes-long store install) must not freeze every other
    # panel interaction — the single-threaded server made the whole UI
    # unresponsive whenever one endpoint was busy.
    server = http.server.ThreadingHTTPServer(("0.0.0.0", INGRESS_PORT), WizardHandler)
    server.daemon_threads = True
    sys.stdout.write(f"[wizard] Listening on port {INGRESS_PORT}\n")
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
