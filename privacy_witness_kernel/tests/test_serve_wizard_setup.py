"""Tests for the Frigate-aware setup wizard server (serve_wizard.py).

Beside test_serve_wizard.py (the proxy, SSRF, gate and export tests), this
file covers the add-on's product behaviors:
  * /api/health, the Supervisor watchdog probe;
  * the stdlib YAML key scanner that reads Frigate's camera list and a
    blueprint's input names;
  * the Frigate-aware preflight (cameras from the mapped config, add-on
    variants, my-HA links instead of Supervisor install calls);
  * the shared camera slug (contract C8) in frigate.cameras and the
    generated Frigate config;
  * what the save merges and what it leaves to the Configuration tab;
  * Core restart through the core API proxy, on a click, and the 403 mapping;
  * the explicit integration-install click (D15) and /api/status.

serve_wizard lives at the add-on root (not a package), so it is loaded by
file path; the server only runs under `if __name__ == "__main__"`.
"""

import importlib.util
import io
import json
import urllib.error
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).resolve().parent.parent / "serve_wizard.py"
_spec = importlib.util.spec_from_file_location("serve_wizard_setup", _MODULE_PATH)
serve_wizard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(serve_wizard)


class _FakeResponse:
    status = 200

    def __init__(self, payload: bytes):
        self._payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self):
        return self._payload


def _bare_handler():
    """A WizardHandler without BaseHTTPRequestHandler's socket-bound
    __init__, with a trusted (ingress) client address."""
    handler = serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)
    handler.client_address = (serve_wizard.INGRESS_PROXY_IP, 40000)
    return handler


_SECRET_OPTS = {
    "mode": "frigate",
    "retention_days": 7,
    "device_key_seed": "cafebabe" * 8,
    "frigate": {
        "mqtt_host": "10.0.0.7",
        "topic_prefix": "frigate",
        "mqtt_password": "frigate-broker-pw",
        "cameras": ["front_door"],
    },
    "mqtt_publish": {"enabled": True, "password": "publish-broker-pw"},
    "cameras": [{"name": "front", "url": "rtsp://admin:hunter2@10.0.0.9/s"}],
}


@pytest.fixture
def save_env(monkeypatch, tmp_path):
    """Sandbox _handle_save: capture saved options, no Supervisor, no restart,
    device key file under tmp_path. `captured["current"]` is what the
    server reads back as the options as they are."""
    captured: dict = {}
    monkeypatch.setattr(serve_wizard, "DEVICE_KEY_FILE", tmp_path / "device_key")
    monkeypatch.setattr(serve_wizard, "WIZARD_PREFS_FILE", tmp_path / "wizard_prefs.json")
    monkeypatch.setattr(
        serve_wizard, "set_addon_options", lambda opts: captured.update(options=opts)
    )
    monkeypatch.setattr(serve_wizard, "restart_addon", lambda: None)
    monkeypatch.setattr(
        serve_wizard, "get_addon_options", lambda: dict(captured.get("current") or {})
    )
    monkeypatch.setattr(
        serve_wizard, "_discover_mqtt",
        lambda: {"found": True, "host": "core-mosquitto", "port": 1883, "username": "addons"},
    )
    monkeypatch.setattr(
        serve_wizard, "_setup_digest_automation",
        lambda t: {"created": False, "detail": "stubbed"},
    )
    return captured


# ---------------------------------------------------------------------------
# /api/health — the Supervisor watchdog probe (config.yaml `watchdog:`).
# Configured: 200 only when the kernel's own /health answers, else 503, so a
# dead kernel gets the app restarted. Not yet configured: 200, because the
# wizard is the whole app then and a watchdog demanding a kernel would
# restart the wizard forever.
# ---------------------------------------------------------------------------


def _run_health_get(monkeypatch, configured: bool, kernel_ok: bool, tmp_path):
    marker = tmp_path / "configured"
    if configured:
        marker.write_text("")
    monkeypatch.setattr(serve_wizard, "CONFIGURED_MARKER", marker)
    monkeypatch.setattr(serve_wizard, "_kernel_health_ok", lambda timeout=3: kernel_ok)
    handler = _bare_handler()
    handler.path = "/api/health"
    sent = {"json": None, "status": None}
    handler._json_response = lambda data, status=200: sent.update(json=data, status=status)
    handler.do_GET()
    return sent


def test_health_is_200_only_when_the_kernel_answers(monkeypatch, tmp_path):
    sent = _run_health_get(monkeypatch, configured=True, kernel_ok=True, tmp_path=tmp_path)
    assert sent["status"] == 200
    assert sent["json"] == {"ok": True, "kernel": "ok", "panel": "ok"}
    sent = _run_health_get(monkeypatch, configured=True, kernel_ok=False, tmp_path=tmp_path)
    assert sent["status"] == 503
    assert sent["json"]["ok"] is False
    assert sent["json"]["kernel"] == "unreachable"


def test_health_is_200_while_only_the_wizard_runs(monkeypatch, tmp_path):
    sent = _run_health_get(monkeypatch, configured=False, kernel_ok=False, tmp_path=tmp_path)
    assert sent["status"] == 200
    assert sent["json"]["kernel"] == "not_configured"


def test_health_refuses_untrusted_clients(monkeypatch, tmp_path):
    # The watchdog arrives from the Supervisor (the ingress proxy address);
    # a co-resident container gets the same 403 as for every other route.
    handler = serve_wizard.WizardHandler.__new__(serve_wizard.WizardHandler)
    handler.client_address = ("172.30.33.9", 40000)
    handler.path = "/api/health"
    sent = {"status": None}
    handler._json_response = lambda data, status=200: sent.update(status=status)
    monkeypatch.delenv("WIZARD_TRUSTED_CLIENT_IPS", raising=False)
    handler.do_GET()
    assert sent["status"] == 403


def test_kernel_health_ok_probes_the_kernel_health_route(monkeypatch):
    captured = {}

    def _fake(req, timeout=None):
        captured["url"] = req.full_url
        captured["timeout"] = timeout
        return _FakeResponse(b'{"status":"ok"}')

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _fake)
    assert serve_wizard._kernel_health_ok() is True
    assert captured["url"] == "http://127.0.0.1:8799/health"
    assert captured["timeout"] == 3

    def _boom(req, timeout=None):
        raise OSError("refused")

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _boom)
    assert serve_wizard._kernel_health_ok() is False


# ---------------------------------------------------------------------------
# The YAML key scanner behind the Frigate camera list and the blueprint
# input names (stdlib only: the image has no PyYAML).
# ---------------------------------------------------------------------------


_FRIGATE_CONFIG = """# Frigate config
mqtt:
  enabled: true
  host: core-mosquitto
go2rtc:
  streams:
    cameras: {}   # a nested key with the same name must not match
cameras:
  front_door:
    ffmpeg:
      inputs:
        - path: rtsp://10.0.0.9/s
  "Back Yard":
    detect:
      enabled: true
  # driveway_old:
  driveway:   # trailing comment
    enabled: false
detectors:
  cpu:
    type: cpu
"""


def test_yaml_child_keys_reads_one_mapping_level():
    assert serve_wizard.yaml_child_keys(_FRIGATE_CONFIG, ("cameras",)) == [
        "front_door", "Back Yard", "driveway"]
    assert serve_wizard.yaml_child_keys(_FRIGATE_CONFIG, ("mqtt",)) == ["enabled", "host"]
    assert serve_wizard.yaml_child_keys(_FRIGATE_CONFIG, ("go2rtc", "streams")) == ["cameras"]


def test_yaml_child_keys_edge_shapes():
    assert serve_wizard.yaml_child_keys("cameras: {}\nx: 1\n", ("cameras",)) == []
    assert serve_wizard.yaml_child_keys("cameras:\n  - a\n  - b\n", ("cameras",)) == []
    assert serve_wizard.yaml_child_keys("mqtt:\n  host: x\n", ("cameras",)) is None
    assert serve_wizard.yaml_child_keys("", ("cameras",)) is None
    four = "cameras:\n    a:\n        x: 1\n    b:\n        y: 2\nz: 1\n"
    assert serve_wizard.yaml_child_keys(four, ("cameras",)) == ["a", "b"]
    assert serve_wizard.yaml_child_keys("blueprint:\n  name: x\n", ("blueprint", "input")) is None
    assert serve_wizard.yaml_child_keys("blueprint: {}\n", ("blueprint", "input")) is None


def test_blueprint_input_names_from_the_shipped_digest_blueprint():
    shipped = _MODULE_PATH.parent.parent / "docs" / "blueprints" / "securacv_daily_digest.yaml"
    names = serve_wizard._blueprint_input_names(shipped)
    assert names is not None
    assert "digest_time" in names


def test_frigate_camera_names_reads_the_config_and_dedupes(tmp_path):
    cfg = tmp_path / "config.yml"
    cfg.write_text(_FRIGATE_CONFIG + "cameras_extra:\n  x: 1\n")
    assert serve_wizard.frigate_camera_names(cfg) == ["front_door", "Back Yard", "driveway"]
    assert serve_wizard.frigate_camera_names(tmp_path / "missing.yml") is None
    cfg.write_text("mqtt:\n  host: x\n")
    assert serve_wizard.frigate_camera_names(cfg) is None


def test_find_frigate_config_prefers_the_mapped_addon_dir(monkeypatch, tmp_path):
    addon_configs = tmp_path / "addon_configs"
    legacy = tmp_path / "config" / "frigate.yml"
    monkeypatch.setattr(serve_wizard, "ADDON_CONFIGS_DIR", addon_configs)
    monkeypatch.setattr(serve_wizard, "LEGACY_FRIGATE_CONF", legacy)
    assert serve_wizard.find_frigate_config() is None
    legacy.parent.mkdir(parents=True)
    legacy.write_text("cameras: {}\n")
    assert serve_wizard.find_frigate_config() == legacy
    fa = addon_configs / "ccab4aaf_frigate-fa"
    fa.mkdir(parents=True)
    (fa / "config.yaml").write_text("cameras: {}\n")
    assert serve_wizard.find_frigate_config() == fa / "config.yaml"
    primary = addon_configs / "ccab4aaf_frigate"
    primary.mkdir()
    (primary / "config.yml").write_text("cameras: {}\n")
    assert serve_wizard.find_frigate_config() == primary / "config.yml"
    # The candidate order is the contract: primary slug first, legacy last.
    candidates = serve_wizard.frigate_config_candidates()
    assert candidates[0] == primary / "config.yml"
    assert candidates[-1] == legacy


# ---------------------------------------------------------------------------
# Frigate-aware preflight: the camera list comes from Frigate's config, the
# Frigate add-on variants are recognized, and every missing piece is paired
# with its my-HA link.
# ---------------------------------------------------------------------------


def test_preflight_reads_frigate_cameras_from_its_config(monkeypatch, tmp_path):
    addon_configs = tmp_path / "addon_configs"
    frigate_dir = addon_configs / "ccab4aaf_frigate"
    frigate_dir.mkdir(parents=True)
    (frigate_dir / "config.yml").write_text(_FRIGATE_CONFIG)
    monkeypatch.setattr(serve_wizard, "ADDON_CONFIGS_DIR", addon_configs)
    monkeypatch.setattr(serve_wizard, "LEGACY_FRIGATE_CONF", tmp_path / "nowhere.yml")

    def _fake_supervisor(method, path, data=None, timeout=10):
        if path == "/services/mqtt":
            return {"data": {"host": "core-mosquitto", "port": 1883,
                             "username": "addons", "password": "hunter2"}}
        if path == "/addons/core_mosquitto/info":
            return {"data": {"state": "started"}}
        if path == "/addons/ccab4aaf_frigate/info":
            return {"data": {"state": "started"}}
        raise RuntimeError("Supervisor API error 404: not installed")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake_supervisor)
    result = _bare_handler()._handle_preflight()
    checks = result["checks"]
    assert checks["frigate"] is True and checks["frigate_running"] is True
    assert checks["frigate_slug"] == "ccab4aaf_frigate"
    assert checks["frigate_config_found"] is True
    assert checks["frigate_config_path"] == str(frigate_dir / "config.yml")
    assert checks["frigate_cameras"] == ["front_door", "Back Yard", "driveway"]
    assert checks["frigate_cameras_read"] is True
    assert result["links"]["frigate"] == serve_wizard.MY_HA_LINKS["frigate"]
    assert "hunter2" not in json.dumps(result)


def test_preflight_recognizes_the_full_access_frigate_variant(monkeypatch, tmp_path):
    monkeypatch.setattr(serve_wizard, "ADDON_CONFIGS_DIR", tmp_path / "addon_configs")
    monkeypatch.setattr(serve_wizard, "LEGACY_FRIGATE_CONF", tmp_path / "nowhere.yml")

    def _fake_supervisor(method, path, data=None, timeout=10):
        if path == "/addons/ccab4aaf_frigate-fa/info":
            return {"data": {"state": "stopped"}}
        raise RuntimeError("Supervisor API error 404")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake_supervisor)
    checks = _bare_handler()._handle_preflight()["checks"]
    assert checks["frigate"] is True and checks["frigate_running"] is False
    assert checks["frigate_slug"] == "ccab4aaf_frigate-fa"
    assert checks["frigate_config_found"] is False
    assert checks["frigate_cameras"] == []
    assert checks["frigate_cameras_read"] is False
    assert checks["mosquitto"] is False


def test_preflight_only_reads_info_paths(monkeypatch, tmp_path):
    # Every Supervisor path preflight touches is one the default role
    # allows: /addons/<slug>/info and /services/mqtt. Nothing else.
    monkeypatch.setattr(serve_wizard, "ADDON_CONFIGS_DIR", tmp_path / "addon_configs")
    monkeypatch.setattr(serve_wizard, "LEGACY_FRIGATE_CONF", tmp_path / "nowhere.yml")
    paths = []

    def _fake_supervisor(method, path, data=None, timeout=10):
        paths.append((method, path))
        raise RuntimeError("nope")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake_supervisor)
    _bare_handler()._handle_preflight()
    for method, path in paths:
        assert method == "GET"
        assert path.endswith("/info") or path == "/services/mqtt", path


# ---------------------------------------------------------------------------
# Camera names: the shared slug (contract C8) everywhere the wizard names a
# camera — frigate.cameras, and the generated Frigate config.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("name", "slug"),
    [
        ("Front Door", "front_door"),
        ("front-door", "front_door"),
        ("Front  Door", "front__door"),
        ("Garage_2", "garage_2"),
        ("Café", "caf_"),
        ("porch", "porch"),
        ("BACK yard!", "back_yard_"),
        ("İstanbul", "_stanbul"),
        ("x" * 80, "x" * 64),
    ],
)
def test_camera_slug_rule(name, slug):
    assert serve_wizard.camera_slug(name) == slug


def test_save_frigate_cameras_are_slugged(save_env):
    handler = _bare_handler()
    result = handler._handle_save({
        "mode": "frigate",
        "frigate_detected": True,
        "cameras": [{"name": "Front Door"}, {"name": "front-door"}, {"name": "Back Yard"},
                    {"name": ""}, {"url": "rtsp://no-name"}, "not a row"],
    })
    assert result["ok"] is True
    assert save_env["options"]["frigate"]["cameras"] == ["front_door", "back_yard"]
    assert result["cameras"] == ["front_door", "back_yard"]
    assert result["frigate_detected"] is True


def test_save_with_frigate_detected_writes_no_frigate_config(save_env, monkeypatch):
    def _must_not_write(*a, **k):  # pragma: no cover
        raise AssertionError("Frigate config written although Frigate owns it")

    monkeypatch.setattr(serve_wizard.WizardHandler, "_write_frigate_config", _must_not_write)
    monkeypatch.setattr(serve_wizard, "_frigate_config_destination", _must_not_write)
    handler = _bare_handler()
    result = handler._handle_save({
        "mode": "frigate",
        "frigate_detected": True,
        "cameras": [{"name": "front_door", "url": "rtsp://10.0.0.9/s"}],
    })
    assert result["ok"] is True
    assert "nothing was written" in result["frigate_config"]
    assert "front_door" in result["frigate_config"]


def test_save_with_frigate_detected_and_no_cameras_names_every_camera(save_env):
    result = _bare_handler()._handle_save({"mode": "frigate", "frigate_detected": True,
                                           "cameras": []})
    assert result["ok"] is True
    assert save_env["options"]["frigate"]["cameras"] == []
    assert "every camera Frigate reports" in result["frigate_config"]


def test_save_with_rtsp_cameras_writes_frigate_config_by_slug(save_env, monkeypatch, tmp_path):
    written = {}
    monkeypatch.setattr(
        serve_wizard, "_frigate_config_destination",
        lambda: (tmp_path / "config.yml", "written"),
    )
    real_write = serve_wizard.WizardHandler._write_frigate_config

    def _capture(self, cameras, *a, **k):
        written["cameras"] = cameras
        return real_write(self, cameras, *a, **k)

    monkeypatch.setattr(serve_wizard.WizardHandler, "_write_frigate_config", _capture)
    handler = _bare_handler()
    result = handler._handle_save({
        "mode": "frigate",
        "cameras": [{"name": "Front Door", "url": "rtsp://10.0.0.9/s"}],
        "retention_days": 7,
    })
    assert result["ok"] is True
    assert [c["name"] for c in written["cameras"]] == ["Front Door"]
    text = (tmp_path / "config.yml").read_text()
    assert "  front_door:\n" in text
    assert "Front Door" not in text
    assert save_env["options"]["frigate"]["cameras"] == ["front_door"]


def test_frigate_config_camera_names_use_the_shared_slug(monkeypatch):
    written = {}
    monkeypatch.setattr(
        serve_wizard.Path, "write_text",
        lambda self, text: written.update(text=text), raising=False,
    )
    handler = _bare_handler()
    handler._write_frigate_config(
        [{"name": "Front  Door", "url": "rtsp://10.0.0.9/a"},
         {"name": "Café", "url": "rtsp://10.0.0.9/b"},
         {"name": "BACK yard!", "url": "rtsp://10.0.0.9/c"}],
        retention_days=1,
    )
    text = written["text"]
    for slug in ("front__door", "caf_", "back_yard_"):
        assert f"  {slug}:\n" in text
    for raw in ("Front  Door", "Café", "BACK yard!"):
        assert raw not in text


# ---------------------------------------------------------------------------
# Settings the wizard collects: what to seal (events vs. Frigate alerts) and
# the witness-log retention; what it must leave alone (the Configuration
# tab's own settings).
# ---------------------------------------------------------------------------


def test_save_enable_reviews_and_retention(save_env):
    handler = _bare_handler()
    result = handler._handle_save({
        "mode": "frigate", "cameras": [], "enable_reviews": True, "retention_days": 30,
    })
    assert result["ok"] is True
    opts = save_env["options"]
    assert opts["frigate"]["enable_reviews"] is True
    assert opts["retention_days"] == 30


def test_save_defaults_retention_to_seven_days(save_env):
    result = _bare_handler()._handle_save({"mode": "frigate", "cameras": []})
    assert result["ok"] is True
    assert save_env["options"]["retention_days"] == 7


def test_save_preserves_configuration_tab_settings(save_env):
    save_env["current"] = {
        "mode": "standalone",
        "verify_interval_hours": 6,
        "install_integration": False,
        "log_level": "debug",
        "time_bucket_minutes": 15,
        "go2rtc_url": "http://ccab4aaf-frigate:1984",
        "frigate": {"labels": ["person"], "min_confidence": 0.7, "cameras": ["old"]},
    }
    handler = _bare_handler()
    result = handler._handle_save({"mode": "frigate", "cameras": [{"name": "New"}]})
    assert result["ok"] is True
    opts = save_env["options"]
    assert opts["mode"] == "frigate"
    assert opts["verify_interval_hours"] == 6
    assert opts["install_integration"] is False
    assert opts["log_level"] == "debug"
    assert opts["time_bucket_minutes"] == 15
    assert opts["go2rtc_url"] == "http://ccab4aaf-frigate:1984"
    assert opts["frigate"]["labels"] == ["person"]
    assert opts["frigate"]["min_confidence"] == 0.7
    assert opts["frigate"]["cameras"] == ["new"]


def test_save_survives_an_unreadable_current_options(save_env, monkeypatch):
    def _boom():
        raise RuntimeError("Supervisor API unreachable")

    monkeypatch.setattr(serve_wizard, "get_addon_options", _boom)
    result = _bare_handler()._handle_save({"mode": "frigate", "cameras": []})
    assert result["ok"] is True
    assert save_env["options"]["frigate"]["labels"] == ["person", "car", "dog", "cat"]


# ---------------------------------------------------------------------------
# Restarting Core: only on the person's click, only through the core API
# proxy (no Supervisor role), and a 403 says what it means.
# ---------------------------------------------------------------------------


def test_restart_ha_uses_the_core_api_proxy(monkeypatch):
    calls = []

    def _fake(method, path, data=None, timeout=10):
        calls.append((method, path, data))
        return []

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake)
    result = _bare_handler()._handle_restart_ha()
    assert result == {"ok": True}
    assert calls == [("POST", "/core/api/services/homeassistant/restart", {})]


def test_supervisor_403_maps_to_a_precise_message(monkeypatch):
    def _http_err(req, timeout=None):
        raise urllib.error.HTTPError(
            req.full_url, 403, "Forbidden", {}, io.BytesIO(b'{"result":"error"}')
        )

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _http_err)
    with pytest.raises(serve_wizard.SupervisorForbidden) as excinfo:
        serve_wizard._supervisor_request("POST", "/core/api/services/homeassistant/restart", {})
    assert "403" in str(excinfo.value)
    assert "role" in str(excinfo.value)
    # ...and the restart handler relays it with the forbidden flag.
    result = _bare_handler()._handle_restart_ha()
    assert result["ok"] is False and result["forbidden"] is True
    assert "role" in result["error"]


def test_supervisor_other_errors_stay_runtime_errors(monkeypatch):
    def _http_err(req, timeout=None):
        raise urllib.error.HTTPError(req.full_url, 400, "Bad", {}, io.BytesIO(b"nope"))

    monkeypatch.setattr(serve_wizard.urllib.request, "urlopen", _http_err)
    with pytest.raises(RuntimeError) as excinfo:
        serve_wizard._supervisor_request("GET", "/services/mqtt")
    assert not isinstance(excinfo.value, serve_wizard.SupervisorForbidden)
    assert "400" in str(excinfo.value)


# ---------------------------------------------------------------------------
# /api/integration/install — the explicit click (D15). The copy itself is
# integration_carry's (tested in test_integration_carry.py); here: the
# option gate, the result mapping, and that no restart rides along.
# ---------------------------------------------------------------------------


def test_integration_install_refused_when_option_is_off(monkeypatch):
    monkeypatch.setattr(serve_wizard, "get_addon_options", lambda: {"install_integration": False})

    def _must_not_call(*a, **k):  # pragma: no cover
        raise AssertionError("copied although install_integration is off")

    monkeypatch.setattr(serve_wizard.integration_carry, "install", _must_not_call)
    result = _bare_handler()._handle_integration_install()
    assert result["ok"] is False
    assert "install_integration" in result["error"]
    assert result["hacs_url"] == serve_wizard.MY_HA_LINKS["hacs"]


def test_integration_install_relays_the_carry_result(monkeypatch):
    monkeypatch.setattr(serve_wizard, "get_addon_options", lambda: {"install_integration": True})
    calls = []
    monkeypatch.setattr(
        serve_wizard.integration_carry, "install",
        lambda: calls.append("install") or {"action": "installed", "detail": "ok",
                                              "installed_version": "0.9.0"},
    )
    restart_calls = []
    monkeypatch.setattr(serve_wizard, "_supervisor_request",
                        lambda *a, **k: restart_calls.append(a))
    result = _bare_handler()._handle_integration_install()
    assert result["ok"] is True and result["action"] == "installed"
    assert calls == ["install"]
    assert restart_calls == []  # the restart is a separate click

    monkeypatch.setattr(
        serve_wizard.integration_carry, "install",
        lambda: {"action": "refused", "detail": "a HACS copy is installed"},
    )
    result = _bare_handler()._handle_integration_install()
    assert result["ok"] is False
    assert result["error"] == "a HACS copy is installed"


def test_integration_install_route_is_reachable(monkeypatch):
    handler = _bare_handler()
    handler.path = "/api/integration/install"
    handler.headers = {"Content-Length": "2"}
    handler.rfile = io.BytesIO(b"{}")
    sent = {"json": None}
    handler._json_response = lambda data, status=200: sent.update(json=data)
    monkeypatch.setattr(
        serve_wizard.WizardHandler, "_handle_integration_install",
        lambda self: {"ok": True, "action": "unchanged"},
    )
    handler.do_POST()
    assert sent["json"] == {"ok": True, "action": "unchanged"}


# ---------------------------------------------------------------------------
# /api/status carries what the done screen and panel decide on: the
# install_integration switch, the integration's state and versions, whether
# Core has it loaded, the digest blueprint's presence and the my-HA links —
# and still never a secret.
# ---------------------------------------------------------------------------


def test_status_reports_integration_state_and_links(monkeypatch, tmp_path):
    opts = dict(_SECRET_OPTS, install_integration=True, verify_interval_hours=1)
    monkeypatch.setattr(serve_wizard, "get_addon_options", lambda: opts)
    monkeypatch.setattr(serve_wizard, "DEVICE_KEY_FILE", tmp_path / "key")
    monkeypatch.setattr(serve_wizard, "DIGEST_BLUEPRINT_FILE", tmp_path / "digest.yaml")
    monkeypatch.setattr(
        serve_wizard, "_supervisor_request",
        lambda m, p, *a, **k: ["mqtt", "securacv"] if p == "/core/api/components" else {},
    )
    monkeypatch.setattr(
        serve_wizard.integration_carry, "inspect",
        lambda dest=None: {"state": "managed", "version": "0.9.0", "path": "x"},
    )
    monkeypatch.setattr(serve_wizard.integration_carry, "read_version", lambda tree: "0.9.0")
    result = _bare_handler()._handle_status()
    assert result["ok"] is True
    assert result["integration"] == {
        "state": "managed", "version": "0.9.0", "carried_version": "0.9.0",
        "install_integration": True, "loaded": True,
        "hacs_url": serve_wizard.MY_HA_LINKS["hacs"],
    }
    assert result["blueprints"] == {"digest": False}
    assert result["links"]["dashboards"].endswith("/lovelace_dashboards/")
    assert result["options"]["install_integration"] is True
    assert result["options"]["verify_interval_hours"] == 1
    assert result["options"]["frigate"]["enable_reviews"] is False
    assert result["options"]["frigate"]["cameras"] == ["front_door"]
    assert "cafebabe" not in json.dumps(result)
    assert "hunter2" not in json.dumps(result)
    assert "frigate-broker-pw" not in json.dumps(result)


def test_core_component_loaded_is_best_effort(monkeypatch):
    monkeypatch.setattr(serve_wizard, "_supervisor_request", lambda *a, **k: ["mqtt"])
    assert serve_wizard._core_component_loaded("securacv") is False
    monkeypatch.setattr(serve_wizard, "_supervisor_request", lambda *a, **k: {"not": "a list"})
    assert serve_wizard._core_component_loaded("securacv") is None

    def _boom(*a, **k):
        raise RuntimeError("down")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _boom)
    assert serve_wizard._core_component_loaded("securacv") is None


def test_public_status_options_carries_camera_names_but_no_secrets():
    pub = serve_wizard._public_status_options(
        dict(_SECRET_OPTS, frigate=dict(_SECRET_OPTS["frigate"], cameras=["front_door", 7])))
    assert pub["frigate"]["cameras"] == ["front_door"]
    assert "mqtt_password" not in pub["frigate"]
    assert pub["install_integration"] is True  # the option's default


# ---------------------------------------------------------------------------
# The digest automation passes only the inputs the blueprint declares, and
# skips honestly when it asks for something the wizard cannot supply.
# ---------------------------------------------------------------------------


def test_digest_automation_passes_only_declared_inputs(monkeypatch, tmp_path):
    blueprint = tmp_path / "securacv_daily_digest.yaml"
    blueprint.write_text(
        "blueprint:\n  name: d\n  input:\n    digest_time:\n      name: t\n"
        "    digest_sensor:\n      name: s\n"
    )
    monkeypatch.setattr(serve_wizard, "DIGEST_BLUEPRINT_FILE", blueprint)
    calls = []

    def _fake(method, path, data=None, timeout=10):
        if path == "/core/api/services":  # pragma: no cover - not asked
            raise AssertionError("notify service looked up although not declared")
        calls.append(data)
        return {}

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _fake)
    result = serve_wizard._setup_digest_automation("08:00")
    assert result["created"] is True
    assert calls[0]["use_blueprint"]["input"] == {"digest_time": "08:00:00"}
    assert "via" not in result["detail"]


def test_digest_automation_skips_when_blueprint_wants_a_device(monkeypatch, tmp_path):
    blueprint = tmp_path / "securacv_daily_digest.yaml"
    blueprint.write_text(
        "blueprint:\n  name: d\n  input:\n    digest_time:\n      name: t\n"
        "    notify_device:\n      name: phone\n"
    )
    monkeypatch.setattr(serve_wizard, "DIGEST_BLUEPRINT_FILE", blueprint)

    def _must_not_post(*a, **k):  # pragma: no cover
        raise AssertionError("automation POSTed with an input the wizard cannot fill")

    monkeypatch.setattr(serve_wizard, "_supervisor_request", _must_not_post)
    result = serve_wizard._setup_digest_automation("08:00")
    assert result["created"] is False
    assert "Blueprints" in result["detail"]
