#!/usr/bin/env python3
"""scripts/tests/test_ha_onboard.py — contract tests for scripts/ha_onboard.py.

ha_onboard.py is the piece of the one-liner installer that talks to the Home
Assistant core API (through the Supervisor proxy): waiting for core after a
restart, creating the SecuraCV config entry by driving its config flow, and
wiring the daily-digest automation to a phone. These tests run it against an
in-process mock of that API — no Home Assistant, no network beyond loopback —
and pin the exact requests it makes, because those requests ARE the contract
with the config-flow work happening on the integration side.

Run:  python3 -m unittest scripts.tests.test_ha_onboard  (or plain pytest)
CI:   .github/workflows/lint.yml (Repo Lints)
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ONBOARD = REPO / "scripts" / "ha_onboard.py"

TOKEN = "test-token"

# /core/api/services payloads. The zed/alpha pair pins "first alphabetically":
# a naive "first in the response" would pick zed.
SERVICES_WITH_MOBILE = [
    {"domain": "light", "services": {"turn_on": {}}},
    {
        "domain": "notify",
        "services": {
            "persistent_notification": {},
            "mobile_app_zed_phone": {},
            "mobile_app_alpha_tablet": {},
        },
    },
]
SERVICES_NO_MOBILE = [
    {"domain": "notify", "services": {"persistent_notification": {}}},
]

AUTOMATION_PATH = "/core/api/config/automation/config/securacv_daily_digest"


class MockCore:
    """Mutable state + request log for the mock supervisor/core API."""

    def __init__(self):
        self.core_status = 200
        self.entries = []  # list of {"domain": ...} config entries
        self.services = SERVICES_WITH_MOBILE
        self.automation_exists = False
        self.requests = []  # (method, path, parsed-body-or-None)
        self.auth_headers = []
        self.flow_counter = 0


def make_handler(state: MockCore):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):  # keep test output quiet
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
            state.auth_headers.append(self.headers.get("Authorization"))
            if self.path == "/core/api/":
                self._send(state.core_status, {"message": "API running."})
            elif self.path == "/core/api/config/config_entries/entry":
                self._send(200, state.entries)
            elif self.path == "/core/api/services":
                self._send(200, state.services)
            elif self.path == AUTOMATION_PATH:
                if state.automation_exists:
                    self._send(200, {"id": "securacv_daily_digest"})
                else:
                    self._send(404, {"message": "Resource not found"})
            else:
                self._send(404, {"message": f"no route {self.path}"})

        def do_POST(self):
            body = self._body()
            state.requests.append(("POST", self.path, body))
            state.auth_headers.append(self.headers.get("Authorization"))
            if self.path == "/core/api/config/config_entries/flow":
                state.flow_counter += 1
                self._send(
                    200,
                    {
                        "type": "form",
                        "flow_id": f"flow{state.flow_counter}",
                        "step_id": "user",
                        "data_schema": [],
                    },
                )
            elif self.path.startswith("/core/api/config/config_entries/flow/"):
                if body == {"setup_mode": "auto"}:
                    # The config-flow contract: submitting auto never returns
                    # another form — it always creates the entry (or aborts
                    # already_configured, which the second-run test exercises
                    # via the entry list instead).
                    state.entries.append({"domain": "securacv", "entry_id": "sc1"})
                    self._send(200, {"type": "create_entry", "result": {"entry_id": "sc1"}})
                else:
                    self._send(200, {"type": "form", "step_id": "user", "errors": {}})
            elif self.path == AUTOMATION_PATH:
                state.automation_exists = True
                self._send(200, {"result": "ok"})
            else:
                self._send(404, {"message": f"no route {self.path}"})

    return Handler


class OnboardTestBase(unittest.TestCase):
    def setUp(self):
        self.state = MockCore()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(self.state))
        self.port = self.server.server_address[1]
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.shutdown)
        self.addCleanup(self.server.server_close)

        # A config dir carrying the digest blueprint the installer places —
        # finish checks for it before creating the automation.
        self.config_dir = Path(tempfile.mkdtemp(prefix="securacv-onboard-"))
        bp_dir = self.config_dir / "blueprints" / "automation" / "securacv"
        bp_dir.mkdir(parents=True)
        (bp_dir / "securacv_daily_digest.yaml").write_text("blueprint:\n", encoding="utf-8")
        self.addCleanup(lambda: __import__("shutil").rmtree(self.config_dir, ignore_errors=True))

    def run_onboard(self, *argv: str) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        for key in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
                    "ALL_PROXY", "all_proxy"):
            env.pop(key, None)
        env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,localhost"
        env["SUPERVISOR_URL"] = f"http://127.0.0.1:{self.port}"
        env["SUPERVISOR_TOKEN"] = TOKEN
        return subprocess.run(
            [sys.executable, str(ONBOARD), *argv],
            env=env, capture_output=True, text=True, timeout=60,
        )

    def posts(self, path_prefix: str):
        return [r for r in self.state.requests if r[0] == "POST" and r[1].startswith(path_prefix)]


class TestWaitCore(OnboardTestBase):
    def test_wait_core_success(self):
        result = self.run_onboard("wait-core", "--timeout", "10")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Core is answering", result.stdout)
        # Bearer auth on the poll, so a 401 could never read as "core is up".
        self.assertIn(f"Bearer {TOKEN}", self.state.auth_headers)

    def test_wait_core_timeout(self):
        self.state.core_status = 503
        result = self.run_onboard("wait-core", "--timeout", "2")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("did not answer within 2s", result.stdout)
        # It kept polling rather than giving up on the first non-200.
        polls = [r for r in self.state.requests if r[1] == "/core/api/"]
        self.assertGreater(len(polls), 1)


class TestFinish(OnboardTestBase):
    def test_finish_happy_path(self):
        self.state.entries = [{"domain": "mqtt", "entry_id": "m1"}]
        result = self.run_onboard("finish", "--config-dir", str(self.config_dir))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        # The securacv flow was started with the exact handler payload…
        flow_starts = self.posts("/core/api/config/config_entries/flow")
        start = [r for r in flow_starts if r[1] == "/core/api/config/config_entries/flow"]
        self.assertEqual(len(start), 1)
        self.assertEqual(start[0][2], {"handler": "securacv", "show_advanced_options": False})
        # …and the user form was answered with automatic mode.
        answers = [r for r in flow_starts if r[1] == "/core/api/config/config_entries/flow/flow1"]
        self.assertEqual(len(answers), 1)
        self.assertEqual(answers[0][2], {"setup_mode": "auto"})

        # The digest automation was created from the blueprint, aimed at the
        # alphabetically-first mobile_app notify target.
        automations = self.posts(AUTOMATION_PATH)
        self.assertEqual(len(automations), 1)
        self.assertEqual(
            automations[0][2],
            {
                "alias": "SecuraCV daily digest",
                "description": "Created by the SecuraCV installer. Safe to edit or delete.",
                "use_blueprint": {
                    "path": "securacv/securacv_daily_digest.yaml",
                    "input": {"notify_service": "notify.mobile_app_alpha_tablet"},
                },
            },
        )
        self.assertIn("mobile_app_alpha_tablet", result.stdout)

    def test_finish_idempotent_second_run(self):
        self.state.entries = [{"domain": "mqtt", "entry_id": "m1"}]
        first = self.run_onboard("finish", "--config-dir", str(self.config_dir))
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)

        self.state.requests.clear()
        second = self.run_onboard("finish", "--config-dir", str(self.config_dir))
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        # Second run changes nothing: no flow started, no automation posted.
        self.assertEqual(self.posts("/core/api/config/config_entries/flow"), [])
        self.assertEqual(self.posts(AUTOMATION_PATH), [])
        self.assertIn("entry already exists", second.stdout)
        self.assertIn("automation already exists", second.stdout)

    def test_finish_no_notify_service_skips_digest(self):
        self.state.entries = [{"domain": "mqtt", "entry_id": "m1"}]
        self.state.services = SERVICES_NO_MOBILE
        result = self.run_onboard("finish", "--config-dir", str(self.config_dir))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("no mobile_app notification targets", result.stdout)
        self.assertEqual(self.posts(AUTOMATION_PATH), [])

    def test_finish_missing_mqtt_warns_and_continues(self):
        self.state.entries = []  # no mqtt entry at all
        result = self.run_onboard("finish", "--config-dir", str(self.config_dir))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Exact clicks are printed…
        self.assertIn(
            "Settings -> Devices & Services -> Add integration -> MQTT", result.stdout
        )
        # …and the rest of onboarding still ran: the securacv flow was driven.
        start = [
            r for r in self.state.requests
            if r == ("POST", "/core/api/config/config_entries/flow",
                     {"handler": "securacv", "show_advanced_options": False})
        ]
        self.assertEqual(len(start), 1)


if __name__ == "__main__":
    unittest.main()
