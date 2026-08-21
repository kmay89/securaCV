#!/usr/bin/env python3
"""scripts/tests/test_install_sh.py — the one-liner installer, exercised for real.

scripts/install.sh is the first thing many users ever run, so "probably works"
is not a standard. These tests run the ACTUAL script end-to-end against a
mocked world: a fake `ha` CLI on PATH that records every call, a loopback mock
of the Supervisor + core APIs, a local tarball standing in for the GitHub
download, and temp dirs standing in for /config and /addon_configs. No network
beyond loopback, no Home Assistant, no Docker daemon.

What they pin:
  * shell hygiene (bash -n, shellcheck);
  * the full HA-OS path: repositories registered via the Supervisor API,
    integration/blueprints/dashboards placed, the lovelace block appended to
    configuration.yaml exactly once even across TWO runs, the securacv config
    flow driven, and — the old installer's sin — no device_key file anywhere;
  * the standalone Docker path: the right compose file lands and is started.

Run:  python3 -m unittest scripts.tests.test_install_sh  (or plain pytest)
CI:   .github/workflows/lint.yml (Repo Lints)
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INSTALL_SH = REPO / "scripts" / "install.sh"

TOKEN = "test-token"
KERNEL_SLUG = "d0491a67_privacy_witness_kernel"
FRIGATE_REPO_URL = "https://github.com/blakeblackshear/frigate-hass-addons"
SECURACV_REPO_URL = "https://github.com/kmay89/securaCV"

# Exactly the working-tree paths install.sh consumes from the tarball.
TARBALL_PATHS = [
    "custom_components/securacv",
    "canary-local/tools/hub_seed_apply.py",
    "canary-local/devices/hub_seed.json",
    "homeassistant/frigate/config.yaml",
    "homeassistant/lovelace",
    "docs/blueprints",
    "docker/sidecar",
]


def build_tarball(dest: Path) -> None:
    """A GitHub-style tarball (single top-level dir) of the consumed paths."""
    top = "kmay89-securaCV-deadbee"

    def keep(info: tarfile.TarInfo):
        name = Path(info.name).name
        if name == "__pycache__" or name.endswith(".pyc"):
            return None
        return info

    with tarfile.open(dest, "w:gz") as tar:
        for rel in TARBALL_PATHS:
            tar.add(REPO / rel, arcname=f"{top}/{rel}", filter=keep)


# ---------------------------------------------------------------------------
# Mock Supervisor + core API
# ---------------------------------------------------------------------------


class MockSupervisor:
    def __init__(self):
        self.repos: list[str] = []
        self.kernel_options: dict = {}
        self.entries = [{"domain": "mqtt", "entry_id": "m1"}]
        self.automation_exists = False
        self.requests: list[tuple] = []  # (method, path, parsed-body-or-None)
        self.flow_counter = 0


def make_handler(state: MockSupervisor):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n) if n else b""
            if not raw:
                return None
            try:
                return json.loads(raw)
            except json.JSONDecodeError:
                return None

        def _send(self, status, obj):
            payload = json.dumps(obj).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            state.requests.append(("GET", self.path, None))
            if self.path == "/store/repositories":
                self._send(200, {"data": {"repositories": [{"source": u} for u in state.repos]}})
            elif self.path == f"/addons/{KERNEL_SLUG}/info":
                self._send(200, {"data": {"options": dict(state.kernel_options)}})
            elif self.path == "/core/api/":
                self._send(200, {"message": "API running."})
            elif self.path == "/core/api/config/config_entries/entry":
                self._send(200, state.entries)
            elif self.path == "/core/api/services":
                self._send(200, [
                    {"domain": "notify",
                     "services": {"persistent_notification": {}, "mobile_app_pixel": {}}},
                ])
            elif self.path == "/core/api/config/automation/config/securacv_daily_digest":
                if state.automation_exists:
                    self._send(200, {"id": "securacv_daily_digest"})
                else:
                    self._send(404, {"message": "Resource not found"})
            else:
                self._send(404, {"message": f"no route {self.path}"})

        def do_POST(self):
            body = self._body()
            state.requests.append(("POST", self.path, body))
            if self.path == "/store/repositories":
                url = (body or {}).get("repository", "")
                if url and url not in state.repos:
                    state.repos.append(url)
                self._send(200, {"result": "ok"})
            elif self.path == f"/addons/{KERNEL_SLUG}/options":
                state.kernel_options.update((body or {}).get("options", {}))
                self._send(200, {"result": "ok"})
            elif self.path == "/core/api/config/config_entries/flow":
                state.flow_counter += 1
                self._send(200, {"type": "form", "flow_id": f"flow{state.flow_counter}",
                                 "step_id": "user", "data_schema": []})
            elif self.path.startswith("/core/api/config/config_entries/flow/"):
                if body == {"setup_mode": "auto"}:
                    state.entries.append({"domain": "securacv", "entry_id": "sc1"})
                    self._send(200, {"type": "create_entry", "result": {"entry_id": "sc1"}})
                else:
                    self._send(200, {"type": "form", "step_id": "user", "errors": {}})
            elif self.path == "/core/api/config/automation/config/securacv_daily_digest":
                state.automation_exists = True
                self._send(200, {"result": "ok"})
            else:
                self._send(404, {"message": f"no route {self.path}"})

    return Handler


# ---------------------------------------------------------------------------
# Fake CLIs
# ---------------------------------------------------------------------------

FAKE_HA = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "${FAKE_HA_LOG:-/dev/null}"
case "${1:-} ${2:-}" in
  "core info")    exit 0 ;;
  "core check")   exit 0 ;;
  "core restart") exit 0 ;;
  "network info")
    cat <<'EOF'
docker:
  address: 172.30.32.0/23
  gateway: 172.30.32.1
host_internet: true
interfaces:
- interface: end0
  ipv4:
    address:
    - 192.168.1.23/24
    gateway: 192.168.1.1
    nameservers:
    - 192.168.1.1
EOF
    exit 0 ;;
  "addons info")    exit 1 ;;
  "addons install") exit 0 ;;
  "addons start")   exit 0 ;;
esac
exit 0
"""

# For the Docker-path test: an `ha` that exists but whose Supervisor is NOT
# reachable, so detection falls through to Docker (the (a)-fails->(b) branch).
FAKE_HA_UNREACHABLE = """#!/usr/bin/env bash
exit 1
"""

FAKE_DOCKER = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "${FAKE_DOCKER_LOG:-/dev/null}"
case "${1:-}" in
  info) exit 0 ;;
  compose)
    if [ "${2:-}" = "version" ]; then echo "Docker Compose version v2.27.0"; fi
    exit 0 ;;
  ps) [ -n "${FAKE_DOCKER_PS:-}" ] && printf '%s\\n' "$FAKE_DOCKER_PS"; exit 0 ;;
  inspect) [ -n "${FAKE_DOCKER_INSPECT:-}" ] && printf '%s\\n' "$FAKE_DOCKER_INSPECT"; exit 0 ;;
esac
exit 0
"""


def write_exec(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755)


class InstallShTestBase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="securacv-install-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.tarball = self.tmp / "source.tar.gz"
        build_tarball(self.tarball)
        self.fakebin = self.tmp / "bin"
        self.fakebin.mkdir()

    def start_supervisor(self) -> MockSupervisor:
        state = MockSupervisor()
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
        self.supervisor_port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.shutdown)
        self.addCleanup(server.server_close)
        return state

    def base_env(self) -> dict:
        env = dict(os.environ)
        for key in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
                    "ALL_PROXY", "all_proxy"):
            env.pop(key, None)
        env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost"
        env["PATH"] = f"{self.fakebin}:{env['PATH']}"
        env["SECURACV_TARBALL"] = str(self.tarball)
        return env

    def run_install(self, env: dict) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", str(INSTALL_SH)],
            env=env, capture_output=True, text=True, timeout=120,
            stdin=subprocess.DEVNULL,
        )


class TestShellHygiene(unittest.TestCase):
    def test_bash_syntax(self):
        result = subprocess.run(
            ["bash", "-n", str(INSTALL_SH)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("shellcheck"), "shellcheck not installed")
    def test_shellcheck_clean(self):
        result = subprocess.run(
            ["shellcheck", "--shell=bash", "--exclude=SC1091", str(INSTALL_SH)],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class TestHomeAssistantPath(InstallShTestBase):
    def setUp(self):
        super().setUp()
        self.state = self.start_supervisor()
        write_exec(self.fakebin / "ha", FAKE_HA)
        self.ha_log = self.tmp / "ha_calls.log"
        self.config_dir = self.tmp / "config"
        self.config_dir.mkdir()
        (self.config_dir / "configuration.yaml").write_text(
            "default_config:\n", encoding="utf-8"
        )
        self.addon_configs = self.tmp / "addon_configs"
        self.addon_configs.mkdir()

    def ha_env(self) -> dict:
        env = self.base_env()
        env["FAKE_HA_LOG"] = str(self.ha_log)
        env["HA_CONFIG_DIR"] = str(self.config_dir)
        env["ADDON_CONFIGS_DIR"] = str(self.addon_configs)
        env["SUPERVISOR_URL"] = f"http://127.0.0.1:{self.supervisor_port}"
        env["SUPERVISOR_TOKEN"] = TOKEN
        # The plan-driven provisioner writes to absolute paths from the
        # sha256-pinned plan (/addon_configs/...), which a test must not touch;
        # the bash fallback honors ADDON_CONFIGS_DIR, so pin it here.
        env["SECURACV_PROVISIONER"] = "bash"
        return env

    def test_full_run_then_idempotent_second_run(self):
        env = self.ha_env()
        first = self.run_install(env)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)

        # 1) Both app repositories were registered through the Supervisor API.
        repo_posts = [r for r in self.state.requests
                      if r[0] == "POST" and r[1] == "/store/repositories"]
        self.assertEqual(len(repo_posts), 2, repo_posts)
        self.assertEqual(
            {r[2]["repository"] for r in repo_posts},
            {FRIGATE_REPO_URL, SECURACV_REPO_URL},
        )

        # 2) The integration landed (manifest present under the config dir).
        manifest = self.config_dir / "custom_components" / "securacv" / "manifest.json"
        self.assertTrue(manifest.is_file())

        # 3) Every blueprint landed, none renamed.
        expected_blueprints = sorted(
            p.name for p in (REPO / "docs" / "blueprints").glob("*.yaml")
        )
        installed_blueprints = sorted(
            p.name for p in
            (self.config_dir / "blueprints" / "automation" / "securacv").glob("*.yaml")
        )
        self.assertEqual(installed_blueprints, expected_blueprints)

        # 4) Every dashboard landed in the dedicated dir.
        expected_dashboards = sorted(
            p.name for p in (REPO / "homeassistant" / "lovelace").glob("*.yaml")
        )
        installed_dashboards = sorted(
            p.name for p in (self.config_dir / "securacv" / "dashboards").glob("*.yaml")
        )
        self.assertEqual(installed_dashboards, expected_dashboards)

        # 5) configuration.yaml got the marked lovelace block exactly once,
        #    and the temporary .bak did not survive the operation.
        conf = (self.config_dir / "configuration.yaml").read_text(encoding="utf-8")
        self.assertEqual(conf.count("# BEGIN securacv-dashboards"), 1)
        self.assertEqual(conf.count("# END securacv-dashboards"), 1)
        self.assertIn("securacv-witness:", conf)
        self.assertIn("filename: securacv/dashboards/securacv-dashboard.yaml", conf)
        self.assertFalse((self.config_dir / "configuration.yaml.securacv.bak").exists())

        # 6) The curated Frigate config was seeded, byte-identical, in the
        #    app-config dir (NOT /config/frigate.yml).
        seeded = self.addon_configs / "ccab4aaf_frigate" / "config.yml"
        self.assertTrue(seeded.is_file())
        self.assertEqual(
            seeded.read_bytes(),
            (REPO / "homeassistant" / "frigate" / "config.yaml").read_bytes(),
        )
        self.assertFalse((self.config_dir / "frigate.yml").exists())

        # 7) The securacv config flow was driven: opened with the handler,
        #    answered with automatic mode.
        self.assertIn(
            ("POST", "/core/api/config/config_entries/flow",
             {"handler": "securacv", "show_advanced_options": False}),
            self.state.requests,
        )
        auto_answers = [r for r in self.state.requests
                        if r[0] == "POST"
                        and r[1].startswith("/core/api/config/config_entries/flow/")
                        and r[2] == {"setup_mode": "auto"}]
        self.assertEqual(len(auto_answers), 1)

        # 8) The kernel was switched to frigate mode via a merged options POST
        #    (jq path; without jq the installer degrades and says so instead).
        if shutil.which("jq"):
            self.assertEqual(self.state.kernel_options.get("mode"), "frigate")

        # 9) The old installer's sin is gone: no device_key file anywhere, and
        #    the transcript never mentions one.
        for root, _dirs, files in os.walk(self.config_dir):
            self.assertNotIn("device_key", files, f"device_key created under {root}")
        self.assertNotIn("device_key", first.stdout)

        # 10) The ha CLI was driven through the wrapper: restart happened, and
        #     the add-ons were installed and started.
        ha_calls = self.ha_log.read_text(encoding="utf-8").splitlines()
        self.assertIn("core restart", ha_calls)
        self.assertIn("addons install core_mosquitto", ha_calls)
        self.assertIn("addons start core_mosquitto", ha_calls)
        self.assertIn("addons install ccab4aaf_frigate", ha_calls)
        self.assertIn(f"addons install {KERNEL_SLUG}", ha_calls)
        # Order matters: Frigate must not start before its config is seeded,
        # and the kernel not before its mode is set (or it boots into the
        # wizard-only path). The start calls exist and come after the installs.
        self.assertIn("addons start ccab4aaf_frigate", ha_calls)
        self.assertIn(f"addons start {KERNEL_SLUG}", ha_calls)
        self.assertGreater(
            ha_calls.index("addons start ccab4aaf_frigate"),
            ha_calls.index("addons install ccab4aaf_frigate"),
        )

        # 11) The summary points at the real LAN address, not the Supervisor's
        #     internal 172.30.x.x network.
        self.assertIn("http://192.168.1.23:8123", first.stdout)
        self.assertNotIn("http://172.30.", first.stdout)

        # ---- SECOND RUN: everything already done must be skipped, and the
        # marked block must not be appended again. ----
        self.state.requests.clear()
        second = self.run_install(env)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)

        conf2 = (self.config_dir / "configuration.yaml").read_text(encoding="utf-8")
        self.assertEqual(conf2.count("# BEGIN securacv-dashboards"), 1)

        # No repository re-registration (the mock now reports them present).
        repo_posts2 = [r for r in self.state.requests
                       if r[0] == "POST" and r[1] == "/store/repositories"]
        self.assertEqual(repo_posts2, [])
        # No second securacv flow (the entry exists now).
        flow_posts2 = [r for r in self.state.requests
                       if r[0] == "POST" and r[1] == "/core/api/config/config_entries/flow"]
        self.assertEqual(flow_posts2, [])
        # And the skips are narrated, not silent.
        self.assertIn("already registered", second.stdout)
        self.assertIn("Dashboard already registered", second.stdout)

    def test_vendored_provisioner_runs_relocated(self):
        # The python path hands the tarball-vendored executor exactly these
        # flags; a dry-run proves the relocated copy + plan + assets resolve.
        extract = self.tmp / "extract"
        with tarfile.open(self.tarball) as tar:
            tar.extractall(extract)
        src_root = next(extract.iterdir())
        result = subprocess.run(
            [
                sys.executable,
                str(src_root / "canary-local" / "tools" / "hub_seed_apply.py"),
                "--plan", str(src_root / "canary-local" / "devices" / "hub_seed.json"),
                "--assets-root", str(src_root),
                "--dry-run",
                "--with", "pihole",
            ],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("register repository", result.stdout)
        self.assertIn("Pi-hole", result.stdout)  # --with pass-through reached the plan

    def test_degraded_run_without_token_still_places_files(self):
        env = self.ha_env()
        env.pop("SUPERVISOR_TOKEN", None)
        result = self.run_install(env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # It says what it cannot do…
        self.assertIn("SUPERVISOR_TOKEN is not set", result.stdout)
        # …touches no Supervisor endpoint…
        self.assertEqual(
            [r for r in self.state.requests if r[1].startswith("/store")], []
        )
        # …but still places every file it can, and lists the leftovers.
        self.assertTrue(
            (self.config_dir / "custom_components" / "securacv" / "manifest.json").is_file()
        )
        self.assertIn("Left for you to finish", result.stdout)

    def test_missing_tarball_fails_before_touching_anything(self):
        env = self.ha_env()
        env["SECURACV_TARBALL"] = str(self.tmp / "does-not-exist.tar.gz")
        result = self.run_install(env)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.config_dir / "custom_components").exists())


class TestDockerPath(InstallShTestBase):
    def setUp(self):
        super().setUp()
        # `ha` exists but its Supervisor does not answer -> detection must
        # fall through to the Docker path.
        write_exec(self.fakebin / "ha", FAKE_HA_UNREACHABLE)
        write_exec(self.fakebin / "docker", FAKE_DOCKER)
        self.docker_log = self.tmp / "docker_calls.log"
        self.home = self.tmp / "securacv-home"

    def test_standalone_smoke(self):
        env = self.base_env()
        env["FAKE_DOCKER_LOG"] = str(self.docker_log)
        env["SECURACV_HOME"] = str(self.home)
        result = self.run_install(env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        # No broker container in `docker ps` output -> the with-broker compose
        # file is chosen, verbatim.
        compose = self.home / "compose.yml"
        self.assertTrue(compose.is_file())
        self.assertEqual(
            compose.read_bytes(),
            (REPO / "docker" / "sidecar" / "quickstart-with-broker.compose.yml").read_bytes(),
        )

        docker_calls = self.docker_log.read_text(encoding="utf-8").splitlines()
        self.assertIn(f"compose -f {compose} up -d", docker_calls)
        self.assertIn(f"compose -f {compose} run --rm securacv doctor", docker_calls)
        # Narration: where events go, and how to watch without HA.
        self.assertIn("mosquitto_sub -h localhost -t 'witness/#' -v", result.stdout)
        self.assertIn("announced via MQTT discovery", result.stdout)

    def test_standalone_reuses_broker_on_named_network(self):
        """A broker on a user-defined network is reused: the compose gets its
        container name AND joins its network so the name actually resolves."""
        env = self.base_env()
        env["FAKE_DOCKER_LOG"] = str(self.docker_log)
        env["SECURACV_HOME"] = str(self.home)
        env["FAKE_DOCKER_PS"] = "frigate-mosquitto eclipse-mosquitto:2"
        env["FAKE_DOCKER_INSPECT"] = "frigate_default"
        result = self.run_install(env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        compose = (self.home / "compose.yml").read_text(encoding="utf-8")
        self.assertIn('FRIGATE_MQTT_HOST: "frigate-mosquitto"', compose)
        self.assertIn("name: frigate_default", compose)
        self.assertIn("external: true", compose)
        # It chose the reuse compose, not the bundled-broker one.
        self.assertNotIn("image: eclipse-mosquitto", compose)

    def test_standalone_default_bridge_broker_falls_back_to_bundled(self):
        """A broker on Docker's default bridge is honestly not reusable
        (names don't resolve across projects): bundle one and say why."""
        env = self.base_env()
        env["FAKE_DOCKER_LOG"] = str(self.docker_log)
        env["SECURACV_HOME"] = str(self.home)
        env["FAKE_DOCKER_PS"] = "lonely-mosquitto eclipse-mosquitto:2"
        env["FAKE_DOCKER_INSPECT"] = "bridge"
        result = self.run_install(env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        self.assertIn("default bridge", result.stdout)
        compose = (self.home / "compose.yml").read_bytes()
        self.assertEqual(
            compose,
            (REPO / "docker" / "sidecar" / "quickstart-with-broker.compose.yml").read_bytes(),
        )


if __name__ == "__main__":
    unittest.main()
