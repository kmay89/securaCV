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
    socket-bound __init__ — enough to call the pure proxy method."""
    return serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)


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
        return {"ok": True, "state": "NO_FLOCK"}

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
# do_POST body-length parsing — a malformed Content-Length header is fully
# attacker-controlled input and must produce a clean 400, not an unhandled
# ValueError traceback.
# ---------------------------------------------------------------------------


def _run_post(monkeypatch, content_length, body=b""):
    handler = serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)
    handler.path = "/api/verify"
    monkeypatch.delenv("INGRESS_PATH", raising=False)
    handler.headers = {"Content-Length": content_length}
    handler.rfile = io.BytesIO(body)

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


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
