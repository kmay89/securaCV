"""Unit tests for the add-on pairing-wizard device proxy (serve_wizard).

Mirrors the plain-function + monkeypatch style of
custom_components/securacv/tests/test_signature.py.

Covers the PR-8 "Add another Canary" bridge:
  * _canary_request builds the Bearer header + correct URL/method/body.
  * _handle_mesh_proxy resolves {address, token} from the request body.
  * Errors when the device is unreachable surface as {"ok": False, ...}.
  * The bearer token is NEVER written to a log/print.

serve_wizard lives at the add-on root (not a package), so we load it by
file path. It only runs the HTTP server under `if __name__ == "__main__"`,
so importing it is side-effect free.
"""

import importlib.util
import io
import json
import time
import urllib.error
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).resolve().parent.parent / "serve_wizard.py"
_spec = importlib.util.spec_from_file_location("serve_wizard", _MODULE_PATH)
serve_wizard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(serve_wizard)


class _FakeResponse:
    """Minimal context-manager stand-in for urllib's HTTPResponse."""

    def __init__(self, payload: bytes):
        self._payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self):
        return self._payload


def _capture_urlopen(captured: dict, payload: bytes = b'{"ok": true}'):
    """Return a fake urlopen that records the Request it was handed."""

    def _fake(req, timeout=None):
        captured["req"] = req
        captured["timeout"] = timeout
        return _FakeResponse(payload)

    return _fake


# ---------------------------------------------------------------------------
# _canary_request
# ---------------------------------------------------------------------------

def test_canary_request_builds_bearer_header_and_url(monkeypatch):
    captured: dict = {}
    monkeypatch.setattr(
        serve_wizard.urllib.request, "urlopen", _capture_urlopen(captured)
    )

    result = serve_wizard._canary_request(
        "192.168.1.50", "cv_secret_token", "POST", "/api/mesh/pair/start", {}
    )

    assert result == {"ok": True}
    req = captured["req"]
    assert req.full_url == "http://192.168.1.50/api/mesh/pair/start"
    assert req.method == "POST"
    assert req.headers["Authorization"] == "Bearer cv_secret_token"
    # POST sends a JSON body; GET would send None.
    assert req.data == b"{}"
    assert captured["timeout"] == 10


def test_canary_request_get_has_no_body(monkeypatch):
    captured: dict = {}
    monkeypatch.setattr(
        serve_wizard.urllib.request,
        "urlopen",
        _capture_urlopen(captured, payload=b'{"ok": true, "state": "ACTIVE"}'),
    )

    result = serve_wizard._canary_request(
        "canary.local", "tok", "GET", "/api/mesh", None
    )

    assert result["state"] == "ACTIVE"
    req = captured["req"]
    assert req.full_url == "http://canary.local/api/mesh"
    assert req.method == "GET"
    assert req.data is None


def test_canary_request_missing_address():
    result = serve_wizard._canary_request("", "tok", "GET", "/api/mesh", None)
    assert result["ok"] is False
    assert "address" in result["error"].lower()


@pytest.mark.parametrize(
    "address",
    [
        "evil.com/api/other",   # embedded path -> would override hardcoded path
        "user@evil.com",        # embedded credentials / host confusion
        "http://evil.com",      # embedded scheme
        "host:99999x",          # malformed port
        "1.2.3.4 5.6.7.8",      # whitespace
    ],
)
def test_canary_request_rejects_malformed_address(address, monkeypatch):
    # SSRF hardening: a malformed address must be rejected BEFORE any request
    # is made, so urlopen should never be called.
    def _must_not_call(*a, **k):  # pragma: no cover - asserts it isn't reached
        raise AssertionError("urlopen called for a rejected address")

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _must_not_call)
    result = serve_wizard._canary_request(address, "tok", "GET", "/api/mesh", None)
    assert result["ok"] is False
    assert "address" in result["error"].lower()


@pytest.mark.parametrize(
    "address",
    [
        "127.0.0.1",          # canonical loopback
        "127.1",              # short-form loopback (inet_aton accepts, ipaddress rejects)
        "0",                  # 0.0.0.0 (unspecified)
        "2130706433",         # decimal loopback
        "0x7f.0.0.1",         # hex octet loopback
        "0177.0.0.1",         # octal octet loopback
        "localhost",          # blocked service name
        "supervisor",         # internal Supervisor name
        "169.254.10.10",      # link-local
    ],
)
def test_canary_request_blocks_loopback_including_noncanonical_forms(address, monkeypatch):
    # The OS socket resolver accepts non-canonical numeric IPv4 forms and maps
    # them to loopback; the SSRF guard must block them even though the strict
    # `ipaddress` parser rejects them as non-IPs. urlopen must never be reached.
    def _must_not_call(*a, **k):  # pragma: no cover - asserts it isn't reached
        raise AssertionError(f"urlopen called for blocked address {address!r}")

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _must_not_call)
    result = serve_wizard._canary_request(address, "tok", "GET", "/api/mesh", None)
    assert result["ok"] is False
    assert "address" in result["error"].lower()


def test_is_blocked_host_allows_legitimate_lan_targets():
    # Real LAN devices (private ranges) and unresolved hostnames must NOT be
    # blocked — the guard targets only loopback/link-local/internal surfaces.
    for host in ("192.168.1.50", "10.0.0.5", "172.16.0.9", "camera-1", "nvr.local"):
        assert serve_wizard._is_blocked_host(host) is False, host


@pytest.mark.parametrize("address", ["192.168.1.50", "canary.local", "10.0.0.5:80"])
def test_canary_request_accepts_bare_host(address, monkeypatch):
    captured: dict = {}
    monkeypatch.setattr(
        serve_wizard.urllib.request, "urlopen", _capture_urlopen(captured)
    )
    serve_wizard._canary_request(address, "tok", "GET", "/api/mesh", None)
    assert captured["req"].full_url == f"http://{address}/api/mesh"


def test_canary_request_omits_auth_header_when_no_token(monkeypatch):
    captured: dict = {}
    monkeypatch.setattr(
        serve_wizard.urllib.request, "urlopen", _capture_urlopen(captured)
    )
    serve_wizard._canary_request("h", "", "POST", "/api/mesh/pair/cancel", {})
    assert "Authorization" not in captured["req"].headers


def test_canary_request_device_unreachable(monkeypatch):
    def _boom(req, timeout=None):
        raise OSError("connection refused")

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _boom)
    result = serve_wizard._canary_request(
        "192.168.1.99", "tok", "GET", "/api/mesh", None
    )
    assert result["ok"] is False
    assert "unreachable" in result["error"].lower()


def test_canary_request_http_error(monkeypatch):
    def _http_err(req, timeout=None):
        raise urllib.error.HTTPError(
            req.full_url, 401, "Unauthorized", {}, io.BytesIO(b"nope")
        )

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _http_err)
    result = serve_wizard._canary_request(
        "h", "tok", "POST", "/api/mesh/pair/start", {}
    )
    assert result["ok"] is False
    assert "401" in result["error"]


# ---------------------------------------------------------------------------
# _handle_mesh_proxy — resolves address/token from the body
# ---------------------------------------------------------------------------

def _bare_handler():
    """A WizardHandler instance without running BaseHTTPRequestHandler's
    socket-bound __init__ — enough to call the pure proxy method. Defaults to a
    trusted (Supervisor-ingress) client address so do_GET/do_POST-driving tests
    pass the ingress trust gate; the gate itself is tested via _gate_handler
    with explicit untrusted IPs."""
    handler = serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)
    handler.client_address = (serve_wizard.INGRESS_PROXY_IP, 40000)
    return handler


def test_handle_verify_reaches_the_health_endpoint(monkeypatch):
    """The health probe must actually run: a wrong urllib kwarg used to raise
    inside the try and surface as "kernel unreachable" for every caller."""
    captured: dict = {}
    monkeypatch.setattr(
        serve_wizard.urllib.request,
        "urlopen",
        _capture_urlopen(captured, payload=b'{"status": "ok"}'),
    )

    result = _bare_handler()._handle_verify()

    assert result == {"ok": True, "health": {"status": "ok"}}
    assert captured["req"].full_url == "http://127.0.0.1:8799/health"
    assert captured["timeout"] == 5


def test_handle_mesh_proxy_forwards_address_and_token(monkeypatch):
    captured: dict = {}

    def _fake_canary(address, token, method, path, data=None):
        captured.update(
            address=address, token=token, method=method, path=path, data=data
        )
        return {"ok": True}

    monkeypatch.setattr(serve_wizard, "_canary_request", _fake_canary)

    handler = _bare_handler()
    result = handler._handle_mesh_proxy(
        {"address": "10.0.0.5", "token": "cv_abc"}, "POST", "/api/mesh/pair/join"
    )

    assert result == {"ok": True}
    assert captured["address"] == "10.0.0.5"
    assert captured["token"] == "cv_abc"
    assert captured["method"] == "POST"
    assert captured["path"] == "/api/mesh/pair/join"
    assert captured["data"] == {}  # POST sends an empty body to the device


def test_handle_mesh_proxy_get_sends_no_body(monkeypatch):
    captured: dict = {}

    def _fake_canary(address, token, method, path, data=None):
        captured["data"] = data
        return {"ok": True, "state": "NO_OPERA"}

    monkeypatch.setattr(serve_wizard, "_canary_request", _fake_canary)
    handler = _bare_handler()
    handler._handle_mesh_proxy({"address": "h", "token": "t"}, "GET", "/api/mesh")
    assert captured["data"] is None


def test_handle_mesh_proxy_missing_address():
    handler = _bare_handler()
    result = handler._handle_mesh_proxy({"token": "t"}, "POST", "/api/mesh/pair/start")
    assert result["ok"] is False
    assert "address" in result["error"].lower()


# ---------------------------------------------------------------------------
# Token must never be logged
# ---------------------------------------------------------------------------

def test_token_not_logged(monkeypatch, capsys):
    """Drive _canary_request through both success + error paths and assert
    the secret token never appears on stdout or stderr."""
    secret = "cv_super_secret_do_not_log"

    captured: dict = {}
    monkeypatch.setattr(
        serve_wizard.urllib.request, "urlopen", _capture_urlopen(captured)
    )
    serve_wizard._canary_request("h", secret, "POST", "/api/mesh/pair/start", {})

    def _boom(req, timeout=None):
        raise OSError("refused")

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _boom)
    err_result = serve_wizard._canary_request("h", secret, "GET", "/api/mesh", None)

    out = capsys.readouterr()
    assert secret not in out.out
    assert secret not in out.err
    # And the token must not be echoed back in the error payload either.
    assert secret not in json.dumps(err_result)


# ---------------------------------------------------------------------------
# MQTT service discovery (Supervisor /services/mqtt)
# ---------------------------------------------------------------------------

def test_discover_mqtt_found(monkeypatch):
    def _fake_supervisor(method, path, data=None):
        assert method == "GET"
        assert path == "/services/mqtt"
        return {
            "data": {
                "host": "core-mosquitto",
                "port": 1883,
                "username": "addons",
                "password": "hunter2",
            }
        }

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake_supervisor)
    mqtt = serve_wizard._discover_mqtt()
    assert mqtt == {
        "found": True,
        "host": "core-mosquitto",
        "port": 1883,
        "username": "addons",
    }
    # The broker password must never enter the wizard's data flow — it is
    # not needed (run.sh re-discovers at startup) and returning it would
    # taint everything derived from this dict.
    assert "password" not in mqtt


def test_discover_mqtt_unavailable(monkeypatch):
    def _boom(method, path, data=None):
        raise RuntimeError("Supervisor API error 400: no mqtt service")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _boom)
    mqtt = serve_wizard._discover_mqtt()
    assert mqtt["found"] is False
    assert mqtt["host"] == ""


def test_preflight_reports_mqtt_service_without_password(monkeypatch):
    """The preflight response must surface discovery status but NEVER the
    broker password (it would end up in the browser)."""

    def _fake_supervisor(method, path, data=None):
        if path == "/services/mqtt":
            return {"data": {"host": "core-mosquitto", "port": 1883,
                             "username": "addons", "password": "hunter2"}}
        if path == "/addons/core_mosquitto/info":
            return {"data": {"state": "started"}}
        raise RuntimeError("not installed")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake_supervisor)
    handler = _bare_handler()
    result = handler._handle_preflight()

    assert result["ok"] is True
    checks = result["checks"]
    assert checks["mosquitto"] is True
    assert checks["mqtt_service"] is True
    assert checks["mqtt_host"] == "core-mosquitto"
    assert checks["mqtt_auth"] is True
    assert "hunter2" not in json.dumps(result)


# ---------------------------------------------------------------------------
# _handle_save broker resolution
# ---------------------------------------------------------------------------

@pytest.fixture
def save_env(monkeypatch, tmp_path):
    """Sandbox _handle_save: capture saved options, no Supervisor, no restart,
    device key file under tmp_path."""
    captured: dict = {}
    monkeypatch.setattr(serve_wizard, "DEVICE_KEY_FILE", tmp_path / "device_key")
    # Keep the save hermetic: wizard prefs land in tmp_path, never /config.
    monkeypatch.setattr(
        serve_wizard, "WIZARD_PREFS_FILE", tmp_path / "wizard_prefs.json"
    )
    monkeypatch.setattr(
        serve_wizard, "set_addon_options", lambda opts: captured.update(options=opts)
    )
    monkeypatch.setattr(serve_wizard, "restart_addon", lambda: None)
    monkeypatch.setattr(
        serve_wizard, "_discover_mqtt",
        lambda: {"found": True, "host": "core-mosquitto", "port": 1883,
                 "username": "addons"},
    )
    return captured


def test_save_defaults_to_auto_broker(save_env):
    """Without an explicit override the saved broker host must be empty,
    meaning 'auto-discover at startup' — credentials are never persisted."""
    handler = _bare_handler()
    result = handler._handle_save({"mode": "frigate", "cameras": []})

    assert result["ok"] is True
    opts = save_env["options"]
    assert opts["frigate"]["mqtt_host"] == ""
    assert opts["frigate"]["mqtt_password"] == ""
    assert opts["frigate"]["mqtt_topic"] == ""
    assert opts["frigate"]["topic_prefix"] == "frigate"
    assert opts["frigate"]["enable_reviews"] is False
    assert opts["mqtt_publish"]["enabled"] is True
    assert opts["mqtt_publish"]["host"] == ""
    # The discovered password must not leak into the saved options.
    assert "hunter2" not in json.dumps(opts)


def test_save_honors_explicit_broker_override(save_env):
    handler = _bare_handler()
    result = handler._handle_save({
        "mode": "frigate",
        "cameras": [],
        "mqtt": {"host": "10.0.0.7", "port": 8883,
                 "username": "ext", "password": "extpass"},
        "frigate_topic_prefix": "frigate_house",
        "enable_reviews": True,
    })

    assert result["ok"] is True
    opts = save_env["options"]
    assert opts["frigate"]["mqtt_host"] == "10.0.0.7"
    assert opts["frigate"]["mqtt_port"] == 8883
    assert opts["frigate"]["mqtt_username"] == "ext"
    assert opts["frigate"]["mqtt_password"] == "extpass"
    assert opts["frigate"]["topic_prefix"] == "frigate_house"
    assert opts["frigate"]["enable_reviews"] is True
    assert opts["mqtt_publish"]["host"] == "10.0.0.7"


def test_save_generates_and_persists_device_key(save_env, monkeypatch, tmp_path):
    handler = _bare_handler()
    result = handler._handle_save({"mode": "frigate", "cameras": []})

    assert result["ok"] is True
    key_file = serve_wizard.DEVICE_KEY_FILE
    assert key_file.exists()
    seed = key_file.read_text().strip()
    assert len(seed) == 64
    assert seed == save_env["options"]["device_key_seed"]
    assert (key_file.stat().st_mode & 0o777) == 0o600


# ---------------------------------------------------------------------------
# _kernel_download (Download my events proxy)
# ---------------------------------------------------------------------------


class _FakeDownloadResponse(_FakeResponse):
    """Adds the headers attribute `_kernel_download` reads for the filename."""

    def __init__(self, payload: bytes, disposition: str):
        super().__init__(payload)
        self.headers = {"Content-Disposition": disposition}


def test_kernel_download_sets_token_header_and_passes_disposition(monkeypatch, tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("tok-123\n")
    monkeypatch.setattr(serve_wizard, "API_TOKEN_FILE", token_file)

    captured = {}

    def _fake(req, timeout=None):
        captured["req"] = req
        return _FakeDownloadResponse(
            b'{"artifact":{}}', 'attachment; filename="securacv-events-600.json"'
        )

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _fake)

    payload, err = serve_wizard._kernel_download("/export/bundle?last=24h")
    assert err is None
    body, disposition = payload
    assert body == b'{"artifact":{}}'
    assert disposition == 'attachment; filename="securacv-events-600.json"'
    req = captured["req"]
    assert req.get_full_url().endswith("/export/bundle?last=24h")
    # Token forwarded to the kernel, never anywhere else.
    assert req.get_header("X-witness-token") == "tok-123"


def test_kernel_download_reports_missing_token(monkeypatch, tmp_path):
    monkeypatch.setattr(serve_wizard, "API_TOKEN_FILE", tmp_path / "missing")
    payload, err = serve_wizard._kernel_download("/export/bundle")
    assert payload is None
    assert "token unavailable" in err


def test_kernel_download_surfaces_kernel_error(monkeypatch, tmp_path):
    token_file = tmp_path / "api_token"
    token_file.write_text("tok-123\n")
    monkeypatch.setattr(serve_wizard, "API_TOKEN_FILE", token_file)

    def _fake(req, timeout=None):
        raise urllib.error.HTTPError(
            req.get_full_url(), 400, "Bad Request", None, io.BytesIO(b'{"error":"bad_window"}')
        )

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _fake)
    payload, err = serve_wizard._kernel_download("/export/bundle?last=nope")
    assert payload is None
    assert "400" in err and "bad_window" in err


# ---------------------------------------------------------------------------
# /api/kernel/export proxy — the window is the user's privacy scope, so the
# proxy must fail closed: unknown parameters reject the request instead of
# being dropped (a dropped window silently broadens the export to everything).
# ---------------------------------------------------------------------------


def _run_export_get(monkeypatch, path):
    """Drive do_GET through the export route with all socket IO stubbed."""
    handler = _bare_handler()
    handler.path = path
    handler.client_address = (serve_wizard.INGRESS_PROXY_IP, 40000)

    sent = {"json": None, "status": None}

    def _fake_json(data, status=200):
        sent["json"] = data
        sent["status"] = status

    handler._json_response = _fake_json

    forwarded = []

    def _fake_download(kernel_path):
        forwarded.append(kernel_path)
        return (b"{}", 'attachment; filename="x.json"'), None

    monkeypatch.setattr(serve_wizard, "_kernel_download", _fake_download)
    # The success path writes raw headers/body; stub the socket surface.
    handler.send_response = lambda *a, **k: None
    handler.send_header = lambda *a, **k: None
    handler.end_headers = lambda: None
    handler.wfile = io.BytesIO()

    handler.do_GET()
    return sent, forwarded


def test_export_proxy_rejects_misspelled_window_param(monkeypatch):
    sent, forwarded = _run_export_get(monkeypatch, "/api/kernel/export?las=24h")
    assert forwarded == []  # nothing may reach the kernel
    assert sent["status"] == 400
    assert sent["json"]["ok"] is False
    assert sent["json"]["error"] == "bad_window"
    assert "las" in sent["json"]["detail"]


def test_export_proxy_rejects_unknown_param(monkeypatch):
    sent, forwarded = _run_export_get(monkeypatch, "/api/kernel/export?unknown=1")
    assert forwarded == []
    assert sent["status"] == 400
    assert sent["json"]["error"] == "bad_window"


def test_export_proxy_rejects_mixed_known_and_unknown(monkeypatch):
    sent, forwarded = _run_export_get(
        monkeypatch, "/api/kernel/export?last=24h&unknown=1"
    )
    assert forwarded == []
    assert sent["status"] == 400
    assert sent["json"]["error"] == "bad_window"


def test_export_proxy_forwards_window_intact(monkeypatch):
    sent, forwarded = _run_export_get(monkeypatch, "/api/kernel/export?last=24h")
    assert forwarded == ["/export/bundle?last=24h"]
    assert sent["json"] is None  # no error response


def test_export_proxy_forwards_start_end_verbatim(monkeypatch):
    # Conflicting/malformed combos stay the kernel's call: the proxy forwards
    # allowlisted names verbatim and relays the kernel's bad_window verdict.
    sent, forwarded = _run_export_get(
        monkeypatch, "/api/kernel/export?start=600&end=1200"
    )
    assert forwarded == ["/export/bundle?start=600&end=1200"]
    assert sent["json"] is None


def test_export_proxy_no_query_is_full_export(monkeypatch):
    # Documented operator path: no window at all = full export.
    sent, forwarded = _run_export_get(monkeypatch, "/api/kernel/export")
    assert forwarded == ["/export/bundle"]
    assert sent["json"] is None


def test_export_query_suffix_rejects_empty_param_name():
    suffix, err = serve_wizard._export_query_suffix("&last=24h")
    assert suffix is None
    assert err is not None


# ---------------------------------------------------------------------------
# do_POST body-length parsing — a malformed Content-Length header is fully
# attacker-controlled input and must produce a clean 400, not an unhandled
# ValueError traceback.
# ---------------------------------------------------------------------------


def _run_post(monkeypatch, content_length, body=b""):
    handler = serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)
    handler.path = "/api/verify"
    handler.headers = {"Content-Length": content_length}
    handler.rfile = io.BytesIO(body)
    # Simulate a request that arrived through the Supervisor ingress proxy —
    # direct connections from other IPs are refused before parsing.
    handler.client_address = (serve_wizard.INGRESS_PROXY_IP, 40000)

    sent = {"json": None, "status": None}

    def _fake_json(data, status=200):
        sent["json"] = data
        sent["status"] = status

    handler._json_response = _fake_json
    monkeypatch.setattr(
        serve_wizard.WizardHandler, "_handle_verify", lambda self: {"ok": True}
    )
    handler.do_POST()
    return sent


def test_post_rejects_non_numeric_content_length(monkeypatch):
    sent = _run_post(monkeypatch, "abc")
    assert sent["status"] == 400
    assert sent["json"]["ok"] is False


def test_post_rejects_negative_content_length(monkeypatch):
    sent = _run_post(monkeypatch, "-5")
    assert sent["status"] == 400
    assert sent["json"]["ok"] is False


def test_post_rejects_oversized_content_length(monkeypatch):
    sent = _run_post(monkeypatch, str(64 * 1024 * 1024))
    assert sent["status"] == 413
    assert sent["json"]["ok"] is False


def test_post_with_valid_length_still_works(monkeypatch):
    sent = _run_post(monkeypatch, "2", body=b"{}")
    assert sent["status"] == 200
    assert sent["json"] == {"ok": True}


# ---------------------------------------------------------------------------
# /api/status must never surface the device key seed or MQTT passwords. The
# port is reachable on the Supervisor Docker network, so the full options
# object would hand the root integrity secret to any co-resident container.
# ---------------------------------------------------------------------------


_SECRET_OPTS = {
    "mode": "frigate",
    "retention_days": 7,
    "device_key_seed": "cafebabe" * 8,  # 64-hex root secret
    "frigate": {
        "mqtt_host": "10.0.0.7",
        "topic_prefix": "frigate",
        "mqtt_password": "frigate-broker-pw",
    },
    "mqtt_publish": {"enabled": True, "password": "publish-broker-pw"},
    "cameras": [{"name": "front", "url": "rtsp://admin:hunter2@10.0.0.9/s"}],
}


def test_public_status_options_drops_secrets():
    pub = serve_wizard._public_status_options(_SECRET_OPTS)
    blob = json.dumps(pub)
    assert "cafebabe" not in blob            # device_key_seed
    assert "frigate-broker-pw" not in blob   # frigate mqtt password
    assert "publish-broker-pw" not in blob   # publish mqtt password
    assert "hunter2" not in blob             # camera URL credential
    # But the fields the panel renders survive.
    assert pub["mode"] == "frigate"
    assert pub["retention_days"] == 7
    assert pub["frigate"]["mqtt_host"] == "10.0.0.7"
    assert pub["frigate"]["topic_prefix"] == "frigate"
    assert pub["mqtt_publish"]["enabled"] is True


def test_handle_status_never_returns_seed(monkeypatch):
    monkeypatch.setattr(serve_wizard, "get_addon_options", lambda: _SECRET_OPTS)
    monkeypatch.setattr(serve_wizard, "DEVICE_KEY_FILE", Path("/nonexistent/key"))
    handler = _bare_handler()
    result = handler._handle_status()
    assert result["ok"] is True
    assert result["configured"] is True     # seed present, so configured
    assert "cafebabe" not in json.dumps(result)
    assert "device_key_seed" not in result["options"]


# ---------------------------------------------------------------------------
# SSRF guard: the pairing proxy and camera-reachability test must refuse the
# add-on's own loopback/link-local/internal surface, but still allow LAN
# (RFC1918) devices — the legitimate target.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "host", ["127.0.0.1", "169.254.169.254", "0.0.0.0", "localhost", "supervisor"],
)
def test_is_blocked_host_blocks_internal(host):
    assert serve_wizard._is_blocked_host(host) is True


@pytest.mark.parametrize("host", ["192.168.1.50", "10.0.0.9", "172.16.0.4", "camera.local"])
def test_is_blocked_host_allows_lan(host):
    assert serve_wizard._is_blocked_host(host) is False


@pytest.mark.parametrize("address", ["127.0.0.1", "127.0.0.1:8799", "supervisor", "169.254.1.1"])
def test_canary_request_rejects_internal_targets(address, monkeypatch):
    def _must_not_call(*a, **k):  # pragma: no cover - asserts it isn't reached
        raise AssertionError("urlopen called for a blocked internal target")

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _must_not_call)
    result = serve_wizard._canary_request(address, "tok", "GET", "/api/mesh", None)
    assert result["ok"] is False
    assert "address" in result["error"].lower()


def test_test_camera_tcp_blocks_internal_without_connecting(monkeypatch):
    def _must_not_call(*a, **k):  # pragma: no cover
        raise AssertionError("create_connection called for a blocked host")

    monkeypatch.setattr(serve_wizard.socket, "create_connection", _must_not_call)
    result = serve_wizard.test_camera_tcp("rtsp://127.0.0.1:554/stream")
    assert result["ok"] is False


def test_test_camera_tcp_allows_lan_host(monkeypatch):
    calls = {}

    class _Sock:
        def close(self):
            calls["closed"] = True

    def _fake_conn(addr, timeout=None):
        calls["addr"] = addr
        return _Sock()

    monkeypatch.setattr(serve_wizard.socket, "create_connection", _fake_conn)
    result = serve_wizard.test_camera_tcp("rtsp://admin:pw@192.168.1.9:554/s")
    assert result["ok"] is True
    assert calls["addr"] == ("192.168.1.9", 554)


# ---------------------------------------------------------------------------
# Generated Frigate config: the camera URL becomes an ffmpeg input, so a
# non-camera scheme is dropped and the value is emitted as a quoted scalar.
# ---------------------------------------------------------------------------


def test_write_frigate_config_rejects_bad_scheme(monkeypatch, tmp_path):
    written = {}
    monkeypatch.setattr(
        serve_wizard.Path, "write_text",
        lambda self, text: written.update(text=text), raising=False,
    )
    handler = _bare_handler()
    handler._write_frigate_config(
        [{"name": "evil", "url": "file:///etc/passwd"},
         {"name": "good", "url": "rtsp://10.0.0.9/stream"}],
        retention_days=1,
    )
    text = written["text"]
    assert "file:///etc/passwd" not in text
    assert "'rtsp://10.0.0.9/stream'" in text  # quoted scalar


def test_write_frigate_config_quotes_broker_host_against_yaml_injection(monkeypatch):
    # A wizard-supplied broker host carrying a newline + YAML keys must not
    # inject configuration into frigate.yml — it is emitted as a single-line
    # quoted scalar, mirroring the camera-URL treatment.
    written = {}
    monkeypatch.setattr(
        serve_wizard.Path, "write_text",
        lambda self, text: written.update(text=text), raising=False,
    )
    handler = _bare_handler()
    handler._write_frigate_config(
        [{"name": "cam", "url": "rtsp://10.0.0.9/stream"}],
        retention_days=1,
        broker_host="evil\nlogger:\n  level: debug",
    )
    text = written["text"]
    assert "logger:" not in text, "YAML key injected via broker host"
    assert "  host: 'evil'" in text
    try:
        import yaml
        parsed = yaml.safe_load(text)
        assert parsed["mqtt"]["host"] == "evil"
        assert "logger" not in parsed
    except ImportError:
        pass  # structural assertions above already prove containment


# ---------------------------------------------------------------------------
# Ingress trust gate: the wizard port is reachable on the Supervisor Docker
# network, so a co-resident container must not be able to call ANY endpoint
# directly — only the Supervisor ingress proxy (172.30.32.2) and loopback.
# ---------------------------------------------------------------------------


def _gate_handler(client_ip):
    handler = serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)
    handler.client_address = (client_ip, 40000)
    handler.path = "/api/status"
    handler.headers = {"Content-Length": "0"}
    handler.rfile = io.BytesIO(b"")
    sent = {"json": None, "status": None}

    def _fake_json(payload, status=200):
        sent["json"] = payload
        sent["status"] = status

    handler._json_response = _fake_json
    return handler, sent


@pytest.mark.parametrize("client_ip", ["172.30.33.5", "10.0.0.9", "192.168.1.20"])
def test_untrusted_client_gets_403_on_post(monkeypatch, client_ip):
    handler, sent = _gate_handler(client_ip)
    handler.path = "/api/save"
    monkeypatch.delenv("INGRESS_PATH", raising=False)
    monkeypatch.delenv("WIZARD_TRUSTED_CLIENT_IPS", raising=False)
    handler.do_POST()
    assert sent["status"] == 403
    assert sent["json"]["ok"] is False


def test_untrusted_client_gets_403_on_get(monkeypatch):
    handler, sent = _gate_handler("172.30.33.5")
    monkeypatch.delenv("INGRESS_PATH", raising=False)
    monkeypatch.delenv("WIZARD_TRUSTED_CLIENT_IPS", raising=False)
    handler.do_GET()
    assert sent["status"] == 403


@pytest.mark.parametrize("client_ip", [serve_wizard.INGRESS_PROXY_IP, "127.0.0.1"])
def test_trusted_client_passes_the_gate(monkeypatch, client_ip):
    handler, sent = _gate_handler(client_ip)
    monkeypatch.delenv("INGRESS_PATH", raising=False)
    monkeypatch.setattr(
        serve_wizard.WizardHandler, "_handle_status", lambda self: {"ok": True}
    )
    handler.do_GET()
    assert sent["status"] == 200
    assert sent["json"] == {"ok": True}


def test_extra_trusted_ip_via_env(monkeypatch):
    handler, sent = _gate_handler("10.9.9.9")
    monkeypatch.setenv("WIZARD_TRUSTED_CLIENT_IPS", "10.9.9.9")
    monkeypatch.delenv("INGRESS_PATH", raising=False)
    monkeypatch.setattr(
        serve_wizard.WizardHandler, "_handle_status", lambda self: {"ok": True}
    )
    handler.do_GET()
    assert sent["status"] == 200


# ---------------------------------------------------------------------------
# Device-key seed lifecycle: reconfigure must never silently mint or swap the
# device identity the sealed log is bound to, and the seed is never echoed.
# ---------------------------------------------------------------------------


def test_save_response_never_echoes_the_seed(save_env):
    handler = _bare_handler()
    result = handler._handle_save({"mode": "frigate", "cameras": []})
    assert result["ok"] is True
    assert "device_key_seed" not in result
    assert result["device_key_file"] == str(serve_wizard.DEVICE_KEY_FILE)


def test_reconfigure_without_seed_reuses_existing_identity(save_env):
    handler = _bare_handler()
    assert handler._handle_save({"mode": "frigate", "cameras": []})["ok"] is True
    original = serve_wizard.DEVICE_KEY_FILE.read_text().strip()

    assert handler._handle_save({"mode": "frigate", "cameras": []})["ok"] is True
    assert serve_wizard.DEVICE_KEY_FILE.read_text().strip() == original
    assert save_env["options"]["device_key_seed"] == original


def test_replacing_existing_seed_requires_proof_of_current_seed(save_env):
    handler = _bare_handler()
    assert handler._handle_save({"mode": "frigate", "cameras": []})["ok"] is True
    original = serve_wizard.DEVICE_KEY_FILE.read_text().strip()

    # Attacker-style replacement: knows a new seed, not the current one.
    result = handler._handle_save(
        {"mode": "frigate", "cameras": [], "device_key_seed": "ab" * 32}
    )
    assert result["ok"] is False
    assert "current_device_key_seed" in result["error"]
    assert serve_wizard.DEVICE_KEY_FILE.read_text().strip() == original

    # Legitimate replacement: proves knowledge of the current seed.
    result = handler._handle_save(
        {
            "mode": "frigate",
            "cameras": [],
            "device_key_seed": "ab" * 32,
            "current_device_key_seed": original,
        }
    )
    assert result["ok"] is True
    assert serve_wizard.DEVICE_KEY_FILE.read_text().strip() == "ab" * 32
# Query-string routing: paths are matched by exact string compare, so the
# query must be stripped BEFORE routing ("/api/status?ts=1" previously fell
# through to the static-file handler).
# ---------------------------------------------------------------------------


def _routed_get(monkeypatch, path, handler_name, response):
    handler = _bare_handler()
    handler.path = path
    sent = {"json": None, "status": None}
    handler._json_response = (
        lambda data, status=200: sent.update(json=data, status=status)
    )
    monkeypatch.setattr(
        serve_wizard.WizardHandler, handler_name, lambda self: response
    )
    handler.do_GET()
    return sent


def test_get_status_routes_with_query_string(monkeypatch):
    sent = _routed_get(
        monkeypatch, "/api/status?ts=1755760000", "_handle_status",
        {"ok": True, "configured": True, "options": {}},
    )
    assert sent["json"] == {"ok": True, "configured": True, "options": {}}


def test_get_preflight_routes_with_query_string(monkeypatch):
    sent = _routed_get(
        monkeypatch, "/api/preflight?cachebust=abc", "_handle_preflight",
        {"ok": True, "checks": {}},
    )
    assert sent["json"] == {"ok": True, "checks": {}}


def test_get_discover_cameras_routes_with_query_string(monkeypatch):
    rows = [{"name": "FrontDoor", "url": "rtsp://10.0.0.9/s", "zone_id": "zone:frontdoor"}]
    monkeypatch.setattr(serve_wizard, "_discover_go2rtc_cameras", lambda: rows)
    handler = _bare_handler()
    handler.path = "/api/discover-cameras?x=1"
    sent = {"json": None}
    handler._json_response = lambda data, status=200: sent.update(json=data)
    handler.do_GET()
    assert sent["json"] == {"ok": True, "cameras": rows}


def test_post_routes_with_query_string(monkeypatch):
    # Reuses the do_POST driver: /api/verify with a query must still route.
    handler = _bare_handler()
    handler.path = "/api/verify?nocache=1"
    handler.headers = {"Content-Length": "2"}
    handler.rfile = io.BytesIO(b"{}")
    sent = {"json": None, "status": None}
    handler._json_response = (
        lambda data, status=200: sent.update(json=data, status=status)
    )
    monkeypatch.setattr(
        serve_wizard.WizardHandler, "_handle_verify", lambda self: {"ok": True}
    )
    handler.do_POST()
    assert sent["json"] == {"ok": True}


# ---------------------------------------------------------------------------
# /api/preflight/install — one-click prerequisite install. The target is a
# fixed two-value enum; anything else must be refused BEFORE any Supervisor
# request, and the accepted targets may only compose the known Supervisor
# paths (the SSRF posture of the rest of the server: no browser-supplied
# host, slug, or URL ever reaches the network).
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "target",
    [
        "",  # missing
        "core_ssh",  # arbitrary add-on slug
        "http://evil.example/install",  # URL smuggling
        "../addons/self/uninstall",  # path traversal
        "mosquitto ",  # near miss must not fuzzy-match
        None,
        {"nested": "mosquitto"},
    ],
)
def test_install_refuses_non_supervisor_targets(target, monkeypatch):
    def _must_not_call(*a, **k):  # pragma: no cover - asserts it isn't reached
        raise AssertionError("Supervisor request made for a refused target")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _must_not_call)
    handler = _bare_handler()
    result = handler._handle_preflight_install({"target": target})
    assert result["ok"] is False
    assert "unknown install target" in result["error"]


def _capture_supervisor(calls, fail_paths=()):
    def _fake(method, path, data=None, timeout=10):
        calls.append({"method": method, "path": path, "data": data,
                      "timeout": timeout})
        if path in fail_paths:
            raise RuntimeError(f"Supervisor API error 400: {path}")
        return {}

    return _fake


def test_install_mosquitto_installs_then_starts(monkeypatch):
    calls = []
    monkeypatch.setattr(
        serve_wizard, "_supervisor_request", _capture_supervisor(calls)
    )
    handler = _bare_handler()
    result = handler._handle_preflight_install({"target": "mosquitto"})

    assert result["ok"] is True
    assert [(c["method"], c["path"]) for c in calls] == [
        ("POST", "/store/addons/core_mosquitto/install"),
        ("POST", "/addons/core_mosquitto/start"),
    ]
    # Store installs pull images: the install call must carry a long timeout.
    assert calls[0]["timeout"] >= 300
    assert result["steps"] == ["installed core_mosquitto", "started core_mosquitto"]


def test_install_frigate_registers_repo_then_installs_pinned_slug(monkeypatch):
    calls = []
    monkeypatch.setattr(
        serve_wizard, "_supervisor_request", _capture_supervisor(calls)
    )
    handler = _bare_handler()
    result = handler._handle_preflight_install({"target": "frigate"})

    assert result["ok"] is True
    assert [(c["method"], c["path"]) for c in calls] == [
        ("POST", "/store/repositories"),
        ("POST", "/store/addons/ccab4aaf_frigate/install"),
    ]
    assert calls[0]["data"] == {
        "repository": "https://github.com/blakeblackshear/frigate-hass-addons"
    }
    assert calls[1]["timeout"] >= 300


def test_install_frigate_tolerates_repo_already_registered(monkeypatch):
    calls = []
    monkeypatch.setattr(
        serve_wizard, "_supervisor_request",
        _capture_supervisor(calls, fail_paths=("/store/repositories",)),
    )
    handler = _bare_handler()
    result = handler._handle_preflight_install({"target": "frigate"})
    assert result["ok"] is True  # repo add is best-effort; install is the test
    assert any("already present" in s for s in result["steps"])


def test_install_mosquitto_reports_failure_honestly(monkeypatch):
    calls = []
    monkeypatch.setattr(
        serve_wizard, "_supervisor_request",
        _capture_supervisor(calls, fail_paths=("/store/addons/core_mosquitto/install",)),
    )
    handler = _bare_handler()
    result = handler._handle_preflight_install({"target": "mosquitto"})
    assert result["ok"] is False
    assert "error" in result
    # No start attempt after a failed install.
    assert [(c["method"], c["path"]) for c in calls] == [
        ("POST", "/store/addons/core_mosquitto/install"),
    ]


# ---------------------------------------------------------------------------
# Save-race fix: the /api/save response must be written BEFORE the add-on
# restart is scheduled (the restart kills this process — firing it first made
# the browser report a network failure for a save that succeeded).
# ---------------------------------------------------------------------------


def _run_save_post(monkeypatch, save_result):
    handler = _bare_handler()
    handler.path = "/api/save"
    handler.headers = {"Content-Length": "2"}
    handler.rfile = io.BytesIO(b"{}")

    events = []
    handler._json_response = (
        lambda data, status=200: events.append(("response", data))
    )
    monkeypatch.setattr(
        serve_wizard.WizardHandler, "_handle_save",
        lambda self, payload: save_result,
    )
    monkeypatch.setattr(
        serve_wizard, "_restart_addon_soon",
        lambda: events.append(("restart_scheduled",)),
    )
    handler.do_POST()
    return events


def test_save_response_precedes_restart_scheduling(monkeypatch):
    events = _run_save_post(monkeypatch, {"ok": True})
    assert events == [("response", {"ok": True}), ("restart_scheduled",)]


def test_failed_save_never_schedules_restart(monkeypatch):
    events = _run_save_post(monkeypatch, {"ok": False, "error": "nope"})
    assert events == [("response", {"ok": False, "error": "nope"})]


def test_restart_addon_soon_is_deferred_not_synchronous(monkeypatch):
    calls = []
    monkeypatch.setattr(serve_wizard, "restart_addon", lambda: calls.append("restart"))
    monkeypatch.setattr(serve_wizard, "RESTART_DELAY_SECONDS", 0.05)

    serve_wizard._restart_addon_soon()
    # Synchronously: nothing yet — the response gets its head start.
    assert calls == []
    deadline = time.monotonic() + 5
    while not calls and time.monotonic() < deadline:
        time.sleep(0.01)
    assert calls == ["restart"]


# ---------------------------------------------------------------------------
# Wizard preferences persistence (/config/.securacv/wizard_prefs.json)
# ---------------------------------------------------------------------------


def test_persist_and_load_wizard_prefs_roundtrip(monkeypatch, tmp_path):
    prefs_file = tmp_path / ".securacv" / "wizard_prefs.json"
    monkeypatch.setattr(serve_wizard, "WIZARD_PREFS_FILE", prefs_file)
    prefs = {
        "digest_enabled": True,
        "digest_time": "07:30",
        "pattern_alerts": False,
        "integrity_alerts": True,
    }
    serve_wizard._persist_wizard_prefs(prefs)
    assert json.loads(prefs_file.read_text()) == prefs
    assert serve_wizard.load_wizard_prefs() == prefs


def test_load_wizard_prefs_fails_closed(monkeypatch, tmp_path):
    monkeypatch.setattr(serve_wizard, "WIZARD_PREFS_FILE", tmp_path / "missing.json")
    assert serve_wizard.load_wizard_prefs() == {}
    bad = tmp_path / "bad.json"
    bad.write_text("{not json")
    monkeypatch.setattr(serve_wizard, "WIZARD_PREFS_FILE", bad)
    assert serve_wizard.load_wizard_prefs() == {}
    non_dict = tmp_path / "list.json"
    non_dict.write_text("[1, 2]")
    monkeypatch.setattr(serve_wizard, "WIZARD_PREFS_FILE", non_dict)
    assert serve_wizard.load_wizard_prefs() == {}


def test_save_persists_prefs_and_reports_digest_result(save_env, monkeypatch, tmp_path):
    monkeypatch.setattr(
        serve_wizard, "_setup_digest_automation",
        lambda t: {"created": True, "detail": f"daily digest at {t}"},
    )
    handler = _bare_handler()
    result = handler._handle_save({
        "mode": "frigate",
        "cameras": [],
        "digest_enabled": True,
        "digest_time": "07:30",
        "pattern_alerts": True,
        "integrity_alerts": False,
    })
    assert result["ok"] is True
    prefs = json.loads(serve_wizard.WIZARD_PREFS_FILE.read_text())
    assert prefs == {
        "digest_enabled": True,
        "digest_time": "07:30",
        "pattern_alerts": True,
        "integrity_alerts": False,
    }
    assert result["digest"]["created"] is True


def test_save_with_digest_disabled_skips_automation(save_env, monkeypatch):
    def _must_not_call(_time):  # pragma: no cover - asserts it isn't reached
        raise AssertionError("digest automation created despite being disabled")

    monkeypatch.setattr(serve_wizard, "_setup_digest_automation", _must_not_call)
    handler = _bare_handler()
    result = handler._handle_save(
        {"mode": "frigate", "cameras": [], "digest_enabled": False}
    )
    assert result["ok"] is True
    assert result["digest"]["created"] is False
    assert "disabled" in result["digest"]["detail"]


# ---------------------------------------------------------------------------
# Daily-digest automation composition (POST via the core API proxy)
# ---------------------------------------------------------------------------


_SERVICES_WITH_MOBILE_APP = [
    {"domain": "light", "services": {"turn_on": {}}},
    {"domain": "notify", "services": {
        "persistent_notification": {},
        "mobile_app_pixel_8": {},
        "mobile_app_iphone": {},
    }},
]


@pytest.fixture
def digest_env(monkeypatch, tmp_path):
    """Blueprint file present + a mobile-app notify service discoverable."""
    blueprint = tmp_path / "securacv_daily_digest.yaml"
    blueprint.write_text("blueprint: {}\n")
    monkeypatch.setattr(serve_wizard, "DIGEST_BLUEPRINT_FILE", blueprint)

    calls = []

    def _fake(method, path, data=None, timeout=10):
        if path == "/core/api/services":
            return _SERVICES_WITH_MOBILE_APP
        calls.append({"method": method, "path": path, "data": data})
        return {}

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake)
    return calls


def test_digest_automation_post_composition(digest_env):
    result = serve_wizard._setup_digest_automation("08:00")
    assert result["created"] is True

    assert len(digest_env) == 1
    call = digest_env[0]
    assert call["method"] == "POST"
    assert call["path"] == "/core/api/config/automation/config/securacv_daily_digest"
    blueprint = call["data"]["use_blueprint"]
    assert blueprint["path"] == "securacv/securacv_daily_digest.yaml"
    assert blueprint["input"]["digest_time"] == "08:00:00"
    # Deterministic pick: first mobile_app service in sorted order.
    assert blueprint["input"]["notify_service"] == "notify.mobile_app_iphone"
    # The done screen shows the detail — it must name the real time+service.
    assert "08:00" in result["detail"]
    assert "notify.mobile_app_iphone" in result["detail"]


def test_digest_automation_skips_when_blueprint_missing(monkeypatch, tmp_path):
    monkeypatch.setattr(
        serve_wizard, "DIGEST_BLUEPRINT_FILE", tmp_path / "absent.yaml"
    )

    def _must_not_post(*a, **k):  # pragma: no cover
        raise AssertionError("automation POSTed without a blueprint")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _must_not_post)
    result = serve_wizard._setup_digest_automation("08:00")
    assert result["created"] is False
    assert "blueprint" in result["detail"]


def test_digest_automation_skips_without_mobile_app_notify(monkeypatch, tmp_path):
    blueprint = tmp_path / "securacv_daily_digest.yaml"
    blueprint.write_text("blueprint: {}\n")
    monkeypatch.setattr(serve_wizard, "DIGEST_BLUEPRINT_FILE", blueprint)

    posts = []

    def _fake(method, path, data=None, timeout=10):
        if path == "/core/api/services":
            return [{"domain": "notify", "services": {"persistent_notification": {}}}]
        posts.append(path)
        return {}

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake)
    result = serve_wizard._setup_digest_automation("08:00")
    assert result["created"] is False
    assert "companion app" in result["detail"]
    assert posts == []  # no automation without a notify target


@pytest.mark.parametrize(
    ("raw", "normalized"),
    [
        ("08:00", "08:00:00"),
        ("23:59", "23:59:00"),
        ("06:15:30", "06:15:30"),
        ("8:00", ""),      # not zero-padded
        ("24:00", ""),     # invalid hour
        ("nope", ""),
        ("", ""),
    ],
)
def test_normalize_digest_time(raw, normalized):
    assert serve_wizard._normalize_digest_time(raw) == normalized


def test_digest_automation_rejects_invalid_time(digest_env):
    result = serve_wizard._setup_digest_automation("25:99")
    assert result["created"] is False
    assert digest_env == []  # nothing POSTed


# ---------------------------------------------------------------------------
# Frigate config destination: modern add-on dir when mounted, legacy path as
# fallback — and NEVER overwrite an existing config.
# ---------------------------------------------------------------------------


@pytest.fixture
def frigate_paths(monkeypatch, tmp_path):
    addon_dir = tmp_path / "addon_configs" / "ccab4aaf_frigate"
    legacy = tmp_path / "config" / "frigate.yml"
    legacy.parent.mkdir(parents=True)
    monkeypatch.setattr(serve_wizard, "FRIGATE_ADDON_CONFIG_DIR", addon_dir)
    monkeypatch.setattr(serve_wizard, "LEGACY_FRIGATE_CONF", legacy)
    return addon_dir, legacy


def test_frigate_dest_addon_dir_fresh(frigate_paths):
    addon_dir, _legacy = frigate_paths
    addon_dir.mkdir(parents=True)
    target, note = serve_wizard._frigate_config_destination()
    assert target == addon_dir / "config.yml"
    assert str(target) in note


@pytest.mark.parametrize("existing_name", ["config.yml", "config.yaml"])
def test_frigate_dest_never_overwrites_existing_addon_config(
    frigate_paths, existing_name
):
    addon_dir, _legacy = frigate_paths
    addon_dir.mkdir(parents=True)
    existing = addon_dir / existing_name
    existing.write_text("mqtt:\n  enabled: true\n")

    target, note = serve_wizard._frigate_config_destination()
    assert target == addon_dir / "config.yml.new"
    assert "left untouched" in note
    assert str(existing) in note
    # And the pre-existing config really is untouched by a full save-path
    # write to the returned target.
    handler = _bare_handler()
    handler._write_frigate_config(
        [{"name": "cam", "url": "rtsp://10.0.0.9/s"}], retention_days=1, dest=target
    )
    assert existing.read_text() == "mqtt:\n  enabled: true\n"
    assert target.exists()


def test_frigate_dest_legacy_fallback_names_both_paths(frigate_paths):
    addon_dir, legacy = frigate_paths  # addon_dir NOT created
    target, note = serve_wizard._frigate_config_destination()
    assert target == legacy
    # The note must name both locations so the user can move the file after
    # installing Frigate.
    assert str(legacy) in note
    assert str(addon_dir / "config.yml") in note


def test_frigate_dest_leaves_real_legacy_config_alone(frigate_paths):
    _addon_dir, legacy = frigate_paths
    legacy.write_text("mqtt:\n  host: mine\n")
    target, note = serve_wizard._frigate_config_destination()
    assert target is None  # write nothing
    assert "left" in note and "untouched" in note


def test_frigate_dest_placeholder_legacy_config_is_replaceable(frigate_paths):
    _addon_dir, legacy = frigate_paths
    legacy.write_text("# PLACEHOLDER — replaced by the wizard\n")
    target, _note = serve_wizard._frigate_config_destination()
    assert target == legacy


# ---------------------------------------------------------------------------
# go2rtc discovery transform (mirrors discover_cameras.sh): producers are
# objects carrying "url" (legacy plain strings also accepted), streams with
# no usable URL are skipped, and zone IDs are lowercased BEFORE the
# character sweep so "FrontDoor" -> zone:frontdoor.
# ---------------------------------------------------------------------------


def test_go2rtc_transform_object_producers_and_zone_case():
    streams = {
        "FrontDoor": {"producers": [{"url": "rtsp://10.0.0.9/stream"}]},
        "no_producers": {"producers": []},
        "no_urls": {"producers": [{"other": "field"}]},
        "legacy_strings": {"producers": ["rtsp://10.0.0.8/s"]},
        "prefers_rtsp": {"producers": [
            {"url": "webrtc://10.0.0.7/x"},
            {"url": "rtsp://10.0.0.7/s"},
        ]},
    }
    cameras = serve_wizard._go2rtc_streams_to_cameras(streams)
    by_name = {c["name"]: c for c in cameras}
    assert "no_producers" not in by_name
    assert "no_urls" not in by_name
    assert by_name["FrontDoor"]["url"] == "rtsp://10.0.0.9/stream"
    assert by_name["FrontDoor"]["zone_id"] == "zone:frontdoor"
    assert by_name["legacy_strings"]["url"] == "rtsp://10.0.0.8/s"
    assert by_name["prefers_rtsp"]["url"] == "rtsp://10.0.0.7/s"


def test_go2rtc_transform_rejects_non_dict_payload():
    assert serve_wizard._go2rtc_streams_to_cameras(["not", "a", "dict"]) == []
    assert serve_wizard._go2rtc_streams_to_cameras(None) == []


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
