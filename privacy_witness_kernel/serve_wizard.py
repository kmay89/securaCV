#!/usr/bin/env python3
"""
SecuraCV Setup Wizard HTTP Server

Serves the first-run setup wizard (and, once configured, the status panel)
on the app's ingress port. Started by the `panel` s6 service in every state;
the kernel services gate themselves on the setup run.sh performs.

Endpoints:
  GET  /            → wizard HTML
  GET  /static/*    → static assets
  GET  /api/health  → the Supervisor watchdog probe (200 iff the kernel's
                       own /health answers; 200 while the wizard is the only
                       thing expected to run)
  POST /api/save    → validate and save add-on options via HA Supervisor API
  POST /api/test-camera → test RTSP/IP camera TCP reachability
  GET  /api/status  → current add-on options + integration state (never a secret)
  GET  /api/preflight → check Mosquitto and Frigate prerequisites; read
                       Frigate's camera list from its mapped config
  GET  /api/discover-cameras → cameras discovered from go2rtc, pre-fillable
  POST /api/verify  → run a quick health check after setup
  POST /api/integration/install → copy the carried SecuraCV integration into
                       /config/custom_components (the explicit click; D15)
  POST /api/restart-ha → restart Home Assistant Core (user click only)

All URLs the wizard page requests are joined relative to the page's own
ingress path, so this server only ever sees bare paths ("/api/save").
Query strings are stripped before routing.
"""

import http.server
import importlib.util
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
# run.sh (the s6 setup step) creates this marker once the add-on is
# configured and the kernel services are expected to run. The watchdog probe
# reads it: before setup the panel alone is the add-on, and a watchdog that
# demanded a kernel then would restart the wizard forever.
CONFIGURED_MARKER = Path("/run/securacv/configured")
# Wizard preferences the add-on options schema has no field for (digest
# time). Persisted so a re-run of the wizard and the done screen reflect
# what the user actually chose.
WIZARD_PREFS_FILE = Path("/config/.securacv/wizard_prefs.json")
# Supervisor slug + store repository of the Frigate add-on. The first slug
# is the one the wizard writes a generated config for; the others are the
# same add-on's full-access / beta variants, recognized so preflight can
# read their camera list too.
FRIGATE_ADDON_SLUG = "ccab4aaf_frigate"
FRIGATE_ADDON_SLUGS = (
    FRIGATE_ADDON_SLUG,
    "ccab4aaf_frigate-fa",
    "ccab4aaf_frigate_beta",
    "ccab4aaf_frigate-fa-beta",
)
FRIGATE_REPO_URL = "https://github.com/blakeblackshear/frigate-hass-addons"
# The modern Frigate add-on reads its config from /addon_configs/<slug>/
# (mounted via the `all_app_configs` map at /addon_configs in config.yaml).
# The legacy path is only a fallback for old Frigate versions.
ADDON_CONFIGS_DIR = Path("/addon_configs")
FRIGATE_ADDON_CONFIG_DIR = ADDON_CONFIGS_DIR / FRIGATE_ADDON_SLUG
LEGACY_FRIGATE_CONF = Path("/config/frigate.yml")
# Daily-digest automation: the blueprint the image carries and run.sh places
# under /config/blueprints/automation/securacv, wired through the HA core
# API proxy under a stable automation object id.
BLUEPRINT_DIR = Path("/config/blueprints/automation/securacv")
DIGEST_BLUEPRINT_FILE = BLUEPRINT_DIR / "securacv_daily_digest.yaml"
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

# "Install it for me" is a link, not a Supervisor call: my.home-assistant.io
# redirects open the right page of the person's own Home Assistant, where
# their own admin session installs the add-on. The app therefore needs no
# `hassio_role` (D2), and the wizard has no lever over other add-ons.
MY_HA_LINKS = {
    "mosquitto": "https://my.home-assistant.io/redirect/supervisor_addon/?addon=core_mosquitto",
    "frigate": (
        "https://my.home-assistant.io/redirect/supervisor_addon/?addon="
        f"{FRIGATE_ADDON_SLUG}&repository_url="
        "https%3A%2F%2Fgithub.com%2Fblakeblackshear%2Ffrigate-hass-addons"
    ),
    "hacs": (
        "https://my.home-assistant.io/redirect/hacs_repository/?owner=kmay89"
        "&repository=securacv-homeassistant&category=integration"
    ),
    "integrations": "https://my.home-assistant.io/redirect/integrations/",
    "dashboards": "https://my.home-assistant.io/redirect/lovelace_dashboards/",
}

# The only legitimate clients of this port: the Supervisor's ingress proxy
# (requests that passed Home Assistant's own authentication always originate
# from 172.30.32.2; the Supervisor's watchdog probe arrives from the same
# address) and the add-on's own loopback. The server binds 0.0.0.0 on the
# Supervisor Docker network, so without this gate any co-resident container
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
# The carried integration (option G). integration_carry.py sits beside this
# file in the image (/usr/local/bin) and in the repo; load it by path so the
# add-on root needs no package.
# ---------------------------------------------------------------------------

def _load_integration_carry():
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location(
        "integration_carry", here / "integration_carry.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


integration_carry = _load_integration_carry()


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

class SupervisorForbidden(RuntimeError):
    """The Supervisor answered 403: the add-on has no role for that path.

    Mapped to a precise sentence so the wizard never shows a bare status
    code. Every Supervisor path this server still calls is within the default
    role (/addons/self/*, /services/*, /addons/<slug>/info, /discovery) or the
    core API proxy (homeassistant_api: true), so a 403 here means a Supervisor
    policy change or a misconfigured app, and the message says what to do.
    """


def _supervisor_request(
    method: str, path: str, data: dict | None = None, timeout: int = 10
):
    """Make an authenticated request to the HA Supervisor API.

    Returns the parsed JSON body — usually a dict, but the /core/api/* proxy
    passes core responses through verbatim (GET /core/api/services returns a
    list). A 403 raises SupervisorForbidden; every other failure RuntimeError.
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
        if exc.code == 403:
            raise SupervisorForbidden(
                f"Home Assistant refused this request (HTTP 403 on {path}): the "
                "SecuraCV app has no Supervisor role for it. The app asks for none "
                "— update the app, or do this step from Settings yourself."
            ) from exc
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


def _kernel_health_ok(timeout: int = 3) -> bool:
    """True iff the kernel's own unauthenticated GET /health answers 200."""
    try:
        req = urllib.request.Request(f"{KERNEL_API_URL}/health")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status == 200
    except Exception:
        return False


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
    only reads mode / retention / broker host / topic prefix / what-to-seal /
    camera names / HA-sensor enablement / the install_integration switch (see
    wizard/index.html), so we allowlist exactly those and drop everything
    else. This mirrors the deliberate secret-minimization already applied in
    `_discover_mqtt` / `_handle_preflight`.
    """
    frigate = opts.get("frigate") or {}
    mqtt_publish = opts.get("mqtt_publish") or {}
    cameras = frigate.get("cameras") or []
    return {
        "mode": opts.get("mode", ""),
        "retention_days": opts.get("retention_days"),
        "verify_interval_hours": opts.get("verify_interval_hours"),
        "install_integration": bool(opts.get("install_integration", True)),
        "frigate": {
            # host is not a secret; the broker password is never surfaced.
            "mqtt_host": str(frigate.get("mqtt_host", "") or ""),
            "topic_prefix": str(frigate.get("topic_prefix", "") or ""),
            "enable_reviews": bool(frigate.get("enable_reviews", False)),
            # Camera names are the operator's own place labels (contract §4).
            "cameras": [str(c) for c in cameras if isinstance(c, str)],
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


def _core_component_loaded(domain: str) -> bool | None:
    """Whether Core has `domain` loaded, via GET /core/api/components.
    None when Core cannot be asked (best effort; never a blocker)."""
    try:
        resp = _supervisor_request("GET", "/core/api/components")
    except Exception:
        return None
    if not isinstance(resp, list):
        return None
    return domain in resp


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
    """Persist the wizard's digest choices to /config/.securacv/.

    The add-on options schema deliberately has no fields for these (they
    configure an HA-side automation, not the kernel), but silently
    discarding them made the done screen dishonest. Best-effort: a failure
    is logged, never fatal to the save."""
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
# A small YAML key scanner (stdlib only — the image has no PyYAML).
#
# Enough to read the camera names out of Frigate's config and the input
# names out of a blueprint: the keys of ONE block mapping at a known path,
# told apart by indentation. It never evaluates values, never follows
# anchors, and answers None when the path is not there.
# ---------------------------------------------------------------------------

_YAML_KEY_RE = re.compile(r"""^(?P<indent>[ ]*)(?P<key>"[^"]*"|'[^']*'|[^\s#:'"][^:#]*?)\s*:(?:\s|$)""")


def _yaml_lines(text: str):
    """(indent, key_or_None, rest) for every meaningful line, in order."""
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if raw.lstrip().startswith("- "):
            # A sequence item: not a mapping key at this level.
            yield len(raw) - len(raw.lstrip(" ")), None, raw.strip()
            continue
        match = _YAML_KEY_RE.match(raw)
        if not match:
            yield len(raw) - len(raw.lstrip(" ")), None, raw.strip()
            continue
        key = match.group("key").strip().strip("'\"")
        rest = raw[match.end():].split("#", 1)[0].strip()
        yield len(match.group("indent")), key, rest


def yaml_child_keys(text: str, path: tuple) -> list | None:
    """The keys directly under the block mapping at `path`, or None when the
    path is absent. An inline value (`cameras: {}`) yields [] — a mapping
    with nothing in it, not a missing one.

    Each level of `path` is looked up at that mapping's own indentation
    (0 for the document, else the indentation of the first line under the
    parent key), so a same-named key nested deeper never matches."""
    lines = list(_yaml_lines(text))
    index = 0
    level_indent = 0
    parent_indent = -1
    for depth, want in enumerate(path):
        if depth > 0:
            if index >= len(lines) or lines[index][0] <= parent_indent:
                return None  # the parent key has no block beneath it
            level_indent = lines[index][0]
        found = False
        while index < len(lines):
            line_indent, key, rest = lines[index]
            if line_indent < level_indent:
                break  # left the mapping without finding `want`
            if line_indent == level_indent and key == want:
                found = True
                parent_indent = line_indent
                index += 1
                if rest:
                    # `key: {}` / `key: []` / `key: value` — inline, no children.
                    return [] if depth == len(path) - 1 else None
                break
            index += 1
        if not found:
            return None
    if index >= len(lines) or lines[index][0] <= parent_indent:
        return []
    child_indent = lines[index][0]
    children = []
    while index < len(lines):
        line_indent, key, _rest = lines[index]
        if line_indent <= parent_indent:
            break
        if line_indent == child_indent and key is not None:
            children.append(key)
        index += 1
    return children


# ---------------------------------------------------------------------------
# Frigate: find its config, read its camera list
# ---------------------------------------------------------------------------

def frigate_config_candidates() -> list:
    """Where a running Frigate's config can be, most likely first: the
    mapped add-on config directory of each known Frigate slug (config.yml,
    then config.yaml), then the legacy /config/frigate.yml."""
    candidates = []
    for slug in FRIGATE_ADDON_SLUGS:
        for name in ("config.yml", "config.yaml"):
            candidates.append(ADDON_CONFIGS_DIR / slug / name)
    candidates.append(LEGACY_FRIGATE_CONF)
    return candidates


def find_frigate_config():
    """The first existing Frigate config file, or None."""
    for candidate in frigate_config_candidates():
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            continue
    return None


def frigate_camera_names(path) -> list | None:
    """The camera names Frigate's config declares (the keys under its
    top-level `cameras:`), in file order. None when the file cannot be read
    or has no `cameras:` mapping — the wizard then falls back to "every
    camera Frigate reports" instead of guessing."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    names = yaml_child_keys(text, ("cameras",))
    if names is None:
        return None
    seen = []
    for name in names:
        if name and name not in seen:
            seen.append(name)
    return seen


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


def _blueprint_input_names(path) -> list | None:
    """The input names a blueprint declares (`blueprint.input` keys), or None
    when the file cannot be read as one."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    return yaml_child_keys(text, ("blueprint", "input"))


def _setup_digest_automation(digest_time: str) -> dict:
    """Create the daily-digest automation through the HA core API proxy.

    Returns {"created": bool, "detail": str} and the done screen shows the
    detail either way — the wizard never promises a digest it did not set
    up. Skips honestly when the blueprint file is missing, when it asks for
    an input this wizard cannot supply (a phone picked by device, say), when
    no mobile-app notify service exists, or when the time is invalid. Only
    inputs the blueprint declares are passed, so a blueprint revision that
    drops `notify_service` is not fed one."""
    if not DIGEST_BLUEPRINT_FILE.is_file():
        return {
            "created": False,
            "detail": (
                "digest blueprint not installed "
                f"(expected {DIGEST_BLUEPRINT_FILE}); restart the SecuraCV app "
                "(it places its blueprints there) or import the blueprint, then add "
                "the automation from Settings → Automations → Blueprints"
            ),
        }
    at_time = _normalize_digest_time(digest_time)
    if not at_time:
        return {"created": False, "detail": f"invalid digest time {digest_time!r}"}
    declared = _blueprint_input_names(DIGEST_BLUEPRINT_FILE)
    if declared is None:
        return {"created": False, "detail": "digest blueprint could not be read"}
    inputs: dict = {}
    if "digest_time" in declared:
        inputs["digest_time"] = at_time
    if "notify_service" in declared:
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
        inputs["notify_service"] = notify_service
    elif any(name.startswith("notify") for name in declared):
        return {
            "created": False,
            "detail": (
                "the digest blueprint asks you to pick the phone to notify; add "
                "the automation from Settings → Automations → Blueprints → "
                "SecuraCV Daily Digest"
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
                    "input": inputs,
                },
            },
        )
    except Exception as exc:
        return {"created": False, "detail": f"automation API error: {exc}"}
    via = f" via {inputs['notify_service']}" if "notify_service" in inputs else ""
    return {
        "created": True,
        "detail": f"daily digest at {at_time[:5]}{via}",
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
# Names: the camera slug rule (contract C8) and the go2rtc zone rule
# ---------------------------------------------------------------------------

# str.lower() also folds non-ASCII letters, and a few of them grow ("İ" ->
# "i̇", two code points), which would move the 64-character cut; jq's
# ascii_downcase touches A-Z only.
_ASCII_DOWNCASE = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def camera_slug(name: str) -> str:
    """The ONE camera-name rule, shared with frigate_bridge
    (src/transport/frigate.rs camera_slug, contract C8): A–Z lowercased
    (ASCII only), then every code point outside [a-z0-9] becomes "_" (no run
    collapsing, no trimming), then cut to 64. `Front Door` → `front_door`,
    `Front  Door` → `front__door`, `Café` → `caf_`. The wizard writes this
    slug to `frigate.cameras` AND names the camera by it in a generated
    Frigate config, so the bridge's allowlist and Frigate's `camera` field
    always meet. Zone ids keep their own rule (zones, not cameras)."""
    return re.sub(r"[^a-z0-9]", "_", str(name).translate(_ASCII_DOWNCASE))[:64]


def _go2rtc_streams_to_cameras(streams) -> list:
    """Transform a go2rtc /api/streams payload into wizard camera rows.

    Producers are objects carrying a "url" field (older go2rtc builds
    emitted plain strings; both are handled). A URL is a non-empty string
    and nothing else — a null or a number is skipped, never stringified —
    and streams without one are skipped. Zone IDs must match
    zone:[a-z0-9_-]{1,64}, so the name is lowercased (ASCII only, as jq's
    ascii_downcase does) BEFORE the character sweep.

    This is the REFERENCE for discover_cameras.sh, the jq copy the add-on
    runs at every start: tests/test_serve_wizard.py feeds both the same
    payloads and fails on any row they disagree about."""
    if not isinstance(streams, dict):
        return []
    cameras = []
    for name in sorted(streams):
        stream = streams.get(name)
        producers = stream.get("producers") if isinstance(stream, dict) else None
        urls = []
        for producer in producers if isinstance(producers, list) else []:
            url = producer.get("url") if isinstance(producer, dict) else producer
            if isinstance(url, str) and url:
                urls.append(url)
        if not urls:
            continue
        chosen = next((u for u in urls if u.startswith("rtsp://")), urls[0])
        slug = re.sub(r"[^a-z0-9_-]", "_", str(name).translate(_ASCII_DOWNCASE))[:64]
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

        # The Supervisor watchdog (config.yaml `watchdog:`) probes this path.
        if path == "/api/health":
            body, status = self._handle_health()
            self._json_response(body, status=status)
            return
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
        elif path == "/api/kernel/verify":
            self._json_response(_kernel_request("POST", "/verify"))
        elif path == "/api/test-camera":
            self._json_response(self._handle_test_camera(payload))
        elif path == "/api/verify":
            self._json_response(self._handle_verify())
        elif path == "/api/integration/install":
            self._json_response(self._handle_integration_install())
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
    def _handle_health(self) -> tuple:
        """The watchdog answer: (body, status).

        Configured (run.sh left its marker) → 200 only when the kernel's own
        /health answers, 503 otherwise, so a dead kernel gets the add-on
        restarted by the Supervisor. Not configured → 200: the wizard is the
        whole add-on then, and it is up."""
        if not CONFIGURED_MARKER.exists():
            return {"ok": True, "kernel": "not_configured", "panel": "ok"}, 200
        if _kernel_health_ok():
            return {"ok": True, "kernel": "ok", "panel": "ok"}, 200
        return {"ok": False, "kernel": "unreachable", "panel": "ok"}, 503

    def _integration_state(self, install_integration: bool) -> dict:
        """What the done screen and the status panel say about the
        integration: installed state/version, the version the image
        carries, whether Core has it loaded, and the HACS link."""
        state = integration_carry.inspect()
        return {
            "state": state["state"],
            "version": state["version"],
            "carried_version": integration_carry.read_version(integration_carry.DEFAULT_SRC),
            "install_integration": install_integration,
            "loaded": _core_component_loaded("securacv"),
            "hacs_url": MY_HA_LINKS["hacs"],
        }

    def _handle_status(self) -> dict:
        try:
            opts = get_addon_options()
            # Check if setup is complete
            key_seed = opts.get("device_key_seed", "")
            configured = bool(key_seed and key_seed != "devkey:mvp")
            # Try reading device key from file too
            if not configured and DEVICE_KEY_FILE.exists():
                configured = True
            public = _public_status_options(opts)
            # Never return the raw options: they carry device_key_seed and MQTT
            # passwords. Surface only the non-secret fields the panel renders.
            return {
                "ok": True,
                "configured": configured,
                "options": public,
                "integration": self._integration_state(public["install_integration"]),
                "blueprints": {"digest": DIGEST_BLUEPRINT_FILE.is_file()},
                "links": dict(MY_HA_LINKS),
            }
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

    def _handle_save(self, payload: dict) -> dict:
        """
        Validate and persist the wizard-collected settings.

        Expected payload:
        {
          "mode": "frigate" | "standalone",
          "cameras": [{"name": str, "url": str?, "zone_id": str?}],
          "frigate_detected": bool   (the cameras came from Frigate's own
                                      config: nothing to write for Frigate),
          "retention_days": int,
          "enable_reviews": bool     (seal only Frigate alerts),
          "digest_enabled": bool,
          "digest_time": "HH:MM",
          "device_key_seed": str  (optional; omitted = keep/generate)
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
            if not isinstance(cameras, list):
                cameras = []
            frigate_detected = bool(payload.get("frigate_detected", False))
            retention_days = int(payload.get("retention_days", 7))
            device_key_seed = str(payload.get("device_key_seed", "") or "").strip()

            # The sealed log's identity is bound to the existing seed, so a
            # reconfigure must never silently mint or swap it. Omitted seed +
            # existing key -> reuse the existing identity (the re-run wizard
            # sends none: the page never holds the current seed). A DIFFERENT
            # seed replaces an existing one only with proof of knowledge of
            # the current seed — otherwise any client that reaches this
            # endpoint could install a key it knows and forge the log.
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

            # The wizard owns the ceremony, the Configuration tab owns the
            # settings: start from the options as they are, overlay only the
            # keys the wizard collected, and leave verify_interval_hours,
            # install_integration, log_level, labels, min_confidence and the
            # rest as the person set them.
            try:
                current = get_addon_options() or {}
            except Exception:
                current = {}
            current_frigate = current.get("frigate") if isinstance(current.get("frigate"), dict) else {}
            options: dict = dict(current)
            options.update({
                "mode": mode,
                "device_key_seed": device_key_seed,
                "retention_days": max(1, min(365, retention_days)),
                "time_bucket_minutes": int(current.get("time_bucket_minutes") or 10),
                "log_level": str(current.get("log_level") or "info"),
                "go2rtc_discovery": bool(current.get("go2rtc_discovery", True)),
                "go2rtc_url": str(current.get("go2rtc_url") or GO2RTC_DEFAULT_URL),
                "mqtt_publish": {
                    "enabled": True,
                    "host": mqtt_host,
                    "port": mqtt_port,
                    "username": mqtt_username,
                    "password": mqtt_password,
                    "topic_prefix": "witness",
                    "discovery_prefix": "homeassistant",
                },
            })

            camera_slugs: list = []
            for cam in cameras:
                if not isinstance(cam, dict) or not cam.get("name"):
                    continue
                slug = camera_slug(cam["name"])
                if slug and slug not in camera_slugs:
                    camera_slugs.append(slug)

            labels = current_frigate.get("labels") if isinstance(current_frigate.get("labels"), list) else None
            min_confidence = current_frigate.get("min_confidence")
            if mode == "frigate":
                options["frigate"] = {
                    "mqtt_host": mqtt_host,
                    "mqtt_port": mqtt_port,
                    "mqtt_topic": "",
                    "topic_prefix": str(payload.get("frigate_topic_prefix", "") or "frigate"),
                    "enable_reviews": bool(payload.get("enable_reviews", False)),
                    "mqtt_username": mqtt_username,
                    "mqtt_password": mqtt_password,
                    "min_confidence": float(min_confidence) if min_confidence is not None else 0.5,
                    # The shared slug (C8): what frigate_bridge's allowlist
                    # compares against Frigate's `camera` field.
                    "cameras": camera_slugs,
                    "labels": labels if labels else ["person", "car", "dog", "cat"],
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
                    if isinstance(c, dict)
                ]
                options["frigate"] = {
                    "mqtt_host": mqtt_host,
                    "mqtt_port": mqtt_port,
                    "mqtt_topic": "",
                    "topic_prefix": "frigate",
                    "enable_reviews": False,
                    "mqtt_username": mqtt_username,
                    "mqtt_password": mqtt_password,
                    "min_confidence": float(min_confidence) if min_confidence is not None else 0.5,
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
            })

            # Daily digest: create the automation for real (through the core
            # API proxy) or report honestly why it was skipped.
            if digest_enabled:
                digest_result = _setup_digest_automation(digest_time)
            else:
                digest_result = {"created": False, "detail": "digest disabled in the wizard"}

            # Write a Frigate camera config only when the wizard collected
            # RTSP cameras itself. When the cameras came from a running
            # Frigate's own config there is nothing to write — Frigate
            # already has them — and nothing to merge later.
            frigate_note = ""
            rtsp_cameras = [c for c in cameras if isinstance(c, dict) and c.get("url")]
            if mode == "frigate" and rtsp_cameras and not frigate_detected:
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
                        rtsp_cameras,
                        retention_days,
                        broker_host,
                        broker_port,
                        needs_auth,
                        dest=target,
                    )
            elif mode == "frigate" and frigate_detected:
                frigate_note = (
                    "Frigate keeps its own camera config — nothing was written "
                    "to it. SecuraCV seals the events of: "
                    + (", ".join(camera_slugs) if camera_slugs else "every camera Frigate reports")
                    + "."
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
                "message": "Configuration saved. Restarting SecuraCV…",
                "device_key_file": str(DEVICE_KEY_FILE),
                "digest": digest_result,
                "frigate_config": frigate_note,
                "frigate_detected": frigate_detected,
                "cameras": camera_slugs,
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

        Cameras are named by the shared slug (camera_slug, C8) so the name
        Frigate publishes is the one frigate_bridge's allowlist holds.
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
            name = camera_slug(cam.get("name", "camera")) or "camera"
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
        """Check Mosquitto/Frigate prerequisites and MQTT auto-discovery.

        Frigate-aware: when a Frigate add-on is installed, its camera list is
        read from the config the `all_app_configs` map exposes, so the wizard
        can skip the RTSP screen and confirm instead of asking. Every check
        is paired with the my.home-assistant.io link that installs the
        missing piece in the person's own Home Assistant (no Supervisor
        install calls, no role)."""
        checks: dict = {}
        try:
            resp = _supervisor_request("GET", "/addons/core_mosquitto/info")
            checks["mosquitto"] = resp.get("data", {}).get("state") == "started"
        except Exception:
            checks["mosquitto"] = False
        checks["frigate"] = False
        checks["frigate_running"] = False
        checks["frigate_slug"] = ""
        for slug in FRIGATE_ADDON_SLUGS:
            try:
                resp = _supervisor_request("GET", f"/addons/{slug}/info")
            except Exception:
                continue
            state = resp.get("data", {}).get("state", "")
            if state in ("started", "stopped"):
                checks["frigate"] = True
                checks["frigate_running"] = state == "started"
                checks["frigate_slug"] = slug
                break
        # Frigate's own camera list, from the config it reads.
        config_path = find_frigate_config()
        names = frigate_camera_names(config_path) if config_path is not None else None
        checks["frigate_config_path"] = str(config_path) if config_path is not None else ""
        checks["frigate_config_found"] = config_path is not None
        checks["frigate_cameras"] = names if names is not None else []
        checks["frigate_cameras_read"] = names is not None
        # MQTT service discovery: report presence + non-secret details only
        # (the password stays server-side; run.sh re-discovers it at startup).
        mqtt = _discover_mqtt()
        checks["mqtt_service"] = mqtt["found"]
        checks["mqtt_host"] = mqtt["host"]
        checks["mqtt_port"] = mqtt["port"]
        checks["mqtt_auth"] = bool(mqtt["username"])
        return {"ok": True, "checks": checks, "links": dict(MY_HA_LINKS)}

    def _handle_integration_install(self) -> dict:
        """The explicit click (D15): copy the carried integration into
        /config/custom_components/securacv — first install or update of the
        app's own copy, never a HACS or edited copy, never a downgrade, and
        only while the `install_integration` option is on. Restarting Core
        is a separate click (`/api/restart-ha`), never a side effect here."""
        try:
            opts = get_addon_options()
        except Exception as exc:
            return {"ok": False, "error": str(exc)}
        if not bool(opts.get("install_integration", True)):
            return {
                "ok": False,
                "error": (
                    "install_integration is off in the app's Configuration tab; "
                    "install the SecuraCV integration from HACS instead"
                ),
                "hacs_url": MY_HA_LINKS["hacs"],
            }
        result = integration_carry.install()
        ok = result.get("action") in ("installed", "updated", "unchanged")
        out = {"ok": ok, **result}
        if not ok:
            out["error"] = result.get("detail", "install refused")
        return out

    def _handle_restart_ha(self) -> dict:
        """Restart Home Assistant Core — on the person's click, never
        otherwise. Goes through the core API proxy
        (POST /core/api/services/homeassistant/restart), which
        `homeassistant_api: true` opens without any Supervisor role."""
        try:
            _supervisor_request("POST", "/core/api/services/homeassistant/restart", {})
            return {"ok": True}
        except SupervisorForbidden as exc:
            return {"ok": False, "error": str(exc), "forbidden": True}
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
    # verify) must not freeze every other panel interaction — the
    # single-threaded server made the whole UI unresponsive whenever one
    # endpoint was busy.
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
