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


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
