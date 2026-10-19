#!/usr/bin/env python3
"""The WAP page's SoftAP channel, client cap and ports are the sketch's own.

gen_wap.py used to read ap.channel, ap.max_clients, ap.http_port,
ap.https_port and ap.ssid_prefix from wap_server.h, whose copies of them no
code used (sweep F277): canary_wap.ino could raise its AP on another channel
or bind another port and devices/wap.json kept the old number, green. They
are read now where the firmware uses them: the two constants every
WiFi.softAP() call passes, the port each server is started on, and
onboarding_fsm.h's SSID prefix. Pinned here on a scratch copy of the
generator's sources: a value changed in the sketch moves the page, a sketch
whose number the generator cannot take for the page is refused, and the
generator runs with no wap_server.h at all.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]
PROJECT = "firmware/projects/canary-wap"
SKETCH = f"{PROJECT}/arduino/canary_wap"
INO = f"{SKETCH}/canary_wap.ino"
FSM = f"{SKETCH}/onboarding_fsm.h"
OUT = "canary-local/devices/wap.json"
# Everything gen_wap.py reads, outside the sketch folder (copied whole).
FILES = (
    "canary-local/tools/gen_wap.py",
    "canary-local/tools/_tooling.py",
    "canary-local/devices/registry.json",
    "canary-local/devices/boards.json",
    "canary-local/emulator/web/bench.js",
    "docs/getting_started_canary.md",
    f"{PROJECT}/README.md",
    f"{PROJECT}/platformio.ini",
    f"{PROJECT}/Makefile",
    f"{PROJECT}/setup.sh",
)


class SketchApFacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._td = tempfile.TemporaryDirectory()
        cls.root = Path(cls._td.name)
        for rel in FILES:
            (cls.root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / rel, cls.root / rel)
        shutil.copytree(REPO / SKETCH, cls.root / SKETCH)
        cls.pristine = {rel: (cls.root / rel).read_text(encoding="utf-8") for rel in (INO, FSM)}
        cls.base = cls.generate(cls)  # the instance helper, on the class's scratch root
        if not isinstance(cls.base, dict):
            raise AssertionError(f"gen_wap.py fails on an unmodified copy of its sources: {cls.base}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls._td.cleanup()

    def tearDown(self) -> None:
        for rel, text in self.pristine.items():
            (self.root / rel).write_text(text, encoding="utf-8")

    def generate(self):
        """The generated wap.json as a dict, or gen_wap.py's error text when it refuses."""
        out = self.root / OUT
        out.unlink(missing_ok=True)
        env = {k: v for k, v in os.environ.items() if k != "GITHUB_ACTIONS"}
        run = subprocess.run([sys.executable, str(self.root / "canary-local/tools/gen_wap.py")],
                             cwd=self.root, env=env, capture_output=True, text=True, check=False)
        if run.returncode != 0:
            return run.stderr.strip() or f"exit {run.returncode}"
        return json.loads(out.read_text(encoding="utf-8"))

    def edit(self, rel: str, old: str, new: str, count: int = 1) -> None:
        path = self.root / rel
        text = path.read_text(encoding="utf-8")
        self.assertEqual(text.count(old), count, f"{rel} no longer says {old!r} {count} time(s)")
        path.write_text(text.replace(old, new), encoding="utf-8")

    def assert_moves(self, **ap) -> None:
        got = self.generate()
        self.assertIsInstance(got, dict, f"refused: {got}")
        want = json.loads(json.dumps(self.base))
        want["ap"].update(ap)
        self.assertEqual(got["ap"], want["ap"])
        self.assertEqual({k: v for k, v in got.items() if k != "ap"},
                         {k: v for k, v in self.base.items() if k != "ap"})

    def assert_refused(self, why: str) -> None:
        got = self.generate()
        self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
        self.assertIn(why, got)

    def test_an_unmodified_copy_gives_the_committed_ap_facts(self):
        committed = json.loads((REPO / OUT).read_text(encoding="utf-8"))
        self.assertEqual(self.base["ap"], committed["ap"])

    def test_the_channel_is_the_sketchs(self):
        self.edit(INO, "static const int   AP_CHANNEL          = 1;", "static const int   AP_CHANNEL          = 6;")
        self.assert_moves(channel=6)

    def test_the_client_cap_is_the_sketchs(self):
        self.edit(INO, "static const int   AP_MAX_CLIENTS      = 1;", "static const int   AP_MAX_CLIENTS      = 2;")
        self.assert_moves(max_clients=2)

    def test_the_http_port_is_the_one_both_plain_servers_bind(self):
        self.edit(INO, "redirect_config.server_port = 80;", "redirect_config.server_port = 8080;")
        self.edit(INO, "    config.server_port = 80;", "    config.server_port = 8080;")
        self.assert_moves(http_port=8080)

    def test_two_plain_ports_are_refused(self):
        self.edit(INO, "redirect_config.server_port = 80;", "redirect_config.server_port = 8080;")
        self.assert_refused("one port for the port-80 redirect server and the HTTP-only fallback")

    def test_the_https_port_is_the_one_httpd_ssl_start_binds(self):
        self.edit(INO, "ssl_config.httpd.server_port = 443;",
                  "ssl_config.httpd.server_port = 8443;\n    ssl_config.port_secure = 8443;")
        self.assert_moves(https_port=8443)

    def test_an_https_port_idf_overwrites_is_refused(self):
        # httpd_ssl_start() binds port_secure (443 by default) whatever
        # httpd.server_port says, so this sketch would still listen on 443.
        self.edit(INO, "ssl_config.httpd.server_port = 443;", "ssl_config.httpd.server_port = 8443;")
        self.assert_refused("httpd_ssl_start() binds port_secure")

    def test_a_softap_raised_on_another_channel_is_refused(self):
        self.edit(INO, "WiFi.softAP(g_device.ap_ssid, ap_pass, AP_CHANNEL, false, AP_MAX_CLIENTS);",
                  "WiFi.softAP(g_device.ap_ssid, ap_pass, 6, false, AP_MAX_CLIENTS);")
        self.assert_refused("does not pass AP_CHANNEL, false, AP_MAX_CLIENTS")

    def test_the_ssid_prefix_is_onboarding_fsms(self):
        self.edit(FSM, 'SSID_PREFIX = "SecuraCV-";', 'SSID_PREFIX = "Canary-";')
        got = self.generate()
        self.assertIsInstance(got, dict, f"refused: {got}")
        self.assertEqual(got["ap"]["ssid_prefix"], "Canary-")
        self.assertTrue(got["ap"]["ssid_example"].startswith("Canary-wap-"), got["ap"]["ssid_example"])

    def test_wap_server_h_is_not_read(self):
        header = self.root / SKETCH / "wap_server.h"
        text = header.read_text(encoding="utf-8")
        header.unlink()
        try:
            self.assertEqual(self.generate(), self.base)
        finally:
            header.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
