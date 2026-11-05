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

The HTTPS port is the sketch's HTTPS_PORT constant, which its one
ssl_config.port_secure write takes (sweep F400): httpd_ssl_start() binds
port_secure and writes it over ssl_config.httpd.server_port, so the old
reader's anchor, a write to httpd.server_port, bound nothing. A write there
is now refused, as is any other spelling of port_secure or of the constant.

The mDNS services' ports were typed in gen_wap.py (sweep F366, #1836's
filing): a start that advertised another port left the page green and
wrong. They are read now from the MDNS.addService() calls at each of the
sketch's three mDNS starts, each of which must register exactly the http and
the securacv service, and the starts must agree. A port it cannot read, a
call in a form it cannot read, two starts that disagree, a registration
under a condition it does not evaluate and an mDNS call in another file of
the sketch are each refused with the file and the line; a registration the
compiler never sees (in a comment, a string, under #if 0) is not counted,
and at the sketch's port 80 the page does not move.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# The HTTPS config's one port line and the constant it takes (sweep F400).
PS = "ssl_config.port_secure = HTTPS_PORT;"
CONST = "static const int   HTTPS_PORT          = 443;"
# The sketch's mDNS registrations (sweep F366). Its three starts write the
# same two lines, told apart here by their indentation: the re-announce after
# a rename (mdns_reannounce), the re-announce once the station has an
# address, and the boot announce.
HTTP = 'MDNS.addService("http", "tcp", 80);'
SECURACV = 'MDNS.addService("securacv", "tcp", 80);'
START_INDENT = {"rename": "  ", "sta": "          ", "boot": "    "}
INO_NAME = "canary_wap.ino"

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

    def test_the_https_port_is_the_constant_port_secure_binds(self):
        # The constant moves the page: it is what the one port_secure write
        # takes, and port_secure is what httpd_ssl_start() binds. (The
        # sketch's static_assert stops such a build until the https addresses
        # it spells name the port; the generator reads the text.)
        self.edit(INO, CONST, "static const int   HTTPS_PORT          = 8443;")
        self.assert_moves(https_port=8443)

    def test_an_httpd_server_port_write_is_refused(self):
        # httpd_ssl_start() writes port_secure over it, so a write there binds
        # nothing; the old reader took its number for the page. Refused with
        # the port_secure write still in place, and in its place.
        for new in (f"ssl_config.httpd.server_port = 443;\n    {PS}", "ssl_config.httpd.server_port = 443;",
                    f"{PS}\n    ssl_config.httpd.server_port = HTTPS_PORT;"):
            with self.subTest(new=new):
                self.edit(INO, PS, new)
                self.assert_refused("which httpd_ssl_start() overwrites with port_secure")
                self.tearDown()

    def test_a_port_secure_other_than_the_constant_is_refused(self):
        # The constant still says 443; each of these binds another port, or
        # 443 by a spelling the page's number is not read from.
        for rhs in ("8443", "443", "0x1BB", "kWapHttpsPort", "8443U", "(HTTPS_PORT)", "HTTPS_PORT + 0",
                    "(uint16_t)HTTPS_PORT"):
            with self.subTest(rhs=rhs):
                self.edit(INO, PS, f"ssl_config.port_secure = {rhs};")
                self.assert_refused("write it as ssl_config.port_secure = HTTPS_PORT;")
                self.tearDown()

    def test_port_secure_8443_under_a_443_constant_is_refused(self):
        self.edit(INO, PS, "ssl_config.port_secure = 8443;")
        self.assert_refused("sets the HTTPS server's port in a form this generator cannot read")

    def test_a_port_secure_on_another_config_is_refused(self):
        self.edit(INO, PS, f"{PS}\n    tls_config.port_secure = 8443;")
        self.assert_refused("in a form this generator cannot read")

    def test_a_port_insecure_write_is_refused(self):
        self.edit(INO, PS, f"{PS}\n    ssl_config.port_insecure = 8080;")
        self.assert_refused("in a form this generator cannot read")

    def test_a_transport_mode_other_than_secure_is_refused(self):
        # HTTPD_SSL_TRANSPORT_INSECURE binds port_insecure, not port_secure.
        self.edit(INO, PS, f"{PS}\n    ssl_config.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;")
        self.assert_refused("in a form this generator cannot read")

    def test_the_default_transport_spelled_out_moves_nothing(self):
        self.edit(INO, PS, f"{PS}\n    ssl_config.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;")
        self.assertEqual(self.generate(), self.base)

    def test_a_second_port_write_is_refused(self):
        self.edit(INO, PS, f"{PS}\n    {PS}")
        self.assert_refused("writes ssl_config.port_secure twice")

    def test_no_port_secure_write_is_refused(self):
        # Commented out, the write is gone: IDF's default would bind, which
        # the page's number is not read from.
        for new in (f"// {PS}", ""):
            with self.subTest(new=new):
                self.edit(INO, PS, new)
                self.assert_refused("no ssl_config.port_secure = HTTPS_PORT; in the sketch")
                self.tearDown()

    def test_an_https_port_constant_the_generator_cannot_read_is_refused(self):
        # 0x1BB and 0673 are 443 to the compiler (0673 is octal), 0443 is
        # 291; none is a decimal literal the page's number is read from.
        for new in ("static const int   HTTPS_PORT          = 0x1BB;",
                    "static const int   HTTPS_PORT          = 0673;",
                    "static const int   HTTPS_PORT          = 0443;",
                    "static const int   HTTPS_PORT          = 443U;",
                    "static const int   HTTPS_PORT          = (443);",
                    "static const uint16_t HTTPS_PORT       = 443;",
                    "constexpr int HTTPS_PORT = 443;",
                    "static const int   HTTPS_PORT{443};"):
            with self.subTest(new=new):
                self.edit(INO, CONST, new)
                self.assert_refused("the HTTPS_PORT constant is defined in a form this generator cannot read")
                self.tearDown()

    def test_a_second_or_missing_https_port_constant_is_refused(self):
        for new in (f"{CONST}\n#define HTTPS_PORT 8443", f"{CONST}\nstatic const int HTTPS_PORT = 8443;",
                    "", f"// {CONST}"):
            with self.subTest(new=new):
                self.edit(INO, CONST, new)
                self.assert_refused("the HTTPS_PORT constant: expected one definition in the sketch")
                self.tearDown()

    def test_a_port_out_of_range_is_refused(self):
        for value in ("65536", "99999"):
            with self.subTest(value=value):
                self.edit(INO, CONST, f"static const int   HTTPS_PORT          = {value};")
                self.assert_refused("which is not a TCP port")
                self.tearDown()

    def test_a_plain_port_with_a_leading_zero_is_refused(self):
        # C reads 0120 as octal 80: the page would say 120.
        self.edit(INO, "redirect_config.server_port = 80;", "redirect_config.server_port = 0120;")
        self.edit(INO, "    config.server_port = 80;", "    config.server_port = 0120;")
        self.assert_refused("in a form this generator cannot read")

    def test_a_named_server_port_is_refused(self):
        self.edit(INO, "    config.server_port = 80;", "    config.server_port = HTTP_PORT;")
        self.assert_refused("in a form this generator cannot read")

    def test_a_write_the_compiler_never_sees_is_not_counted(self):
        # The reader reads lines, so a write wrapped in a block comment whose
        # markers sit on lines of their own, or under #if 0, is not code: the
        # only one so wrapped leaves no write (IDF's default would bind), and
        # a second one so wrapped moves nothing.
        # Each case restores the sketch even when it fails, so one failure
        # does not leak into the next.
        for wrap in ("/*\n    {}\n    */", "#if 0\n    {}\n#endif", "#if 0\n#if FEATURE_X\n    {}\n#endif\n#endif"):
            with self.subTest(only=wrap):
                try:
                    self.edit(INO, PS, wrap.format(PS))
                    self.assert_refused("no ssl_config.port_secure = HTTPS_PORT; in the sketch")
                finally:
                    self.tearDown()
            with self.subTest(second=wrap):
                try:
                    self.edit(INO, PS, f"{PS}\n" + wrap.format(PS))
                    self.assertEqual(self.generate(), self.base)
                finally:
                    self.tearDown()
        for wrap in ("/*\n{}\n*/", "#if 0\n{}\n#endif"):
            with self.subTest(constant=wrap):
                try:
                    self.edit(INO, CONST, f"{CONST}\n" + wrap.format("static const int HTTPS_PORT = 8443;"))
                    self.assertEqual(self.generate(), self.base)
                finally:
                    self.tearDown()

    def test_a_write_in_a_live_branch_is_counted(self):
        # #else of an #if 0 is compiled; so is a write after a string that
        # holds "/*" (the redirect route's pattern opens no comment).
        with self.subTest(branch="#else"):
            try:
                self.edit(INO, PS, f"#if 0\n    ssl_config.port_secure = 8443;\n#else\n    {PS}\n#endif")
                self.assertEqual(self.generate(), self.base)
            finally:
                self.tearDown()
        with self.subTest(after='"/*"'):
            self.edit(INO, PS, f'static const char* kAllRoutes = "/*";\n    {PS}\n    {PS}')
            self.assert_refused("writes ssl_config.port_secure twice")

    def test_a_comment_line_naming_port_secure_moves_nothing(self):
        for line in ("// httpd_ssl_start() binds port_secure = 8443; here", "// ssl_config.port_secure = 443;",
                     "// ssl_config.httpd.server_port = 443;"):
            with self.subTest(line=line):
                self.edit(INO, PS, f"{PS}\n    {line}")
                self.assertEqual(self.generate(), self.base)
                self.tearDown()

    def test_the_https_config_must_start_from_idfs_default(self):
        # The 443 fallback for an unwritten port_secure is that default's.
        self.edit(INO, "httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();",
                  "httpd_ssl_config_t ssl_config = {};")
        self.assert_refused("the HTTPS config starts from IDF's default")

    def test_an_https_server_started_from_another_config_is_refused(self):
        self.edit(INO, "httpd_ssl_start(&g_https_server, &ssl_config)", "httpd_ssl_start(&g_https_server, &tls_config)")
        self.assert_refused("expected one httpd_ssl_start() call")

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

    # -- mDNS (sweep F366) ------------------------------------------------

    def at(self, start: str, line: str) -> str:
        """The text of `line` as the named start writes it, newline first."""
        return f"\n{START_INDENT[start]}{line}"

    def line_no(self, needle: str) -> int:
        """The 1-based line of the scratch sketch that holds `needle` (once), or is it."""
        lines = (self.root / INO).read_text(encoding="utf-8").split("\n")
        hits = [i + 1 for i, ln in enumerate(lines) if ln.strip() == needle]
        hits = hits or [i + 1 for i, ln in enumerate(lines) if needle in ln]
        self.assertEqual(len(hits), 1, f"{needle!r} is on lines {hits}")
        return hits[0]

    def services(self, **ports):
        want = json.loads(json.dumps(self.base["ap"]["mdns_services"]))
        for s in want:
            s["port"] = ports.get(s["service"].split(".")[0].lstrip("_"), s["port"])
        return want

    def assert_refused_at(self, why: str, needle: str) -> None:
        """Refused for `why`, naming the sketch and the line that holds `needle`."""
        line = self.line_no(needle)
        got = self.generate()
        self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
        self.assertIn(why, got)
        self.assertRegex(got, rf"\b{re.escape(INO_NAME)}:{line}\b")
        return got

    def test_the_committed_mdns_services_are_the_sketchs_at_port_80(self):
        self.assertEqual([(s["service"], s["port"]) for s in self.base["ap"]["mdns_services"]],
                         [("_http._tcp", 80), ("_securacv._tcp", 80)])

    def test_each_mdns_port_is_the_one_every_start_registers(self):
        for svc, line in (("http", HTTP), ("securacv", SECURACV)):
            with self.subTest(service=svc):
                try:
                    self.edit(INO, line, line.replace(" 80);", " 8080);"), count=3)
                    self.assert_moves(mdns_services=self.services(**{svc: 8080}))
                finally:
                    self.tearDown()

    def test_an_mdns_port_spelled_https_port_is_that_constants(self):
        self.edit(INO, HTTP, HTTP.replace(" 80);", " HTTPS_PORT);"), count=3)
        self.assert_moves(mdns_services=self.services(http=443))

    def test_an_mdns_port_the_generator_cannot_read_is_refused(self):
        # Each spelling at one start only, so a reader that skipped it would
        # see three agreeing starts and answer "equal". 0120 is octal 80 to
        # the compiler; HTTP_PORT is no constant the generator reads (the
        # page's http_port is read from the server configs, not a name).
        for rhs in ("0x50", "HTTP_PORT", "(80)", "80 /* web */", "80u", "080", "0120", "0", "-80", "80 + 0",
                    "(uint16_t)80", "kHttpPort", "80.0"):
            with self.subTest(port=rhs):
                try:
                    new = HTTP.replace(" 80);", f" {rhs});")
                    self.edit(INO, self.at("boot", HTTP), self.at("boot", new))
                    self.assert_refused_at("registers an mDNS service in a form this generator cannot read", new)
                finally:
                    self.tearDown()

    def test_an_mdns_call_the_generator_cannot_read_is_refused(self):
        boot_http = self.at("boot", HTTP)
        for new, needle in (
                ('MDNS.addService(String("http"), "tcp", 80);', 'String("http")'),
                ('MDNS.addService("http", "udp", 80);', '"udp"'),
                ('MDNS.addService("_http", "_tcp", 80);', '"_http"'),
                ('MDNS.addService("http", "tcp", 80); MDNS.addService("http", "tcp", 8080);', "8080"),
                ('MDNS.addService("http", "tcp",\n                    80);', 'MDNS.addService("http", "tcp",'),
                ('if (true) MDNS.addService("http", "tcp", 80);', "if (true)"),
                ('/* web */ MDNS.addService("http", "tcp", 80);', "/* web */"),
                ('MDNS.addService("http", "tcp", 80);  /* web */', "/* web */"),
                ('/* the http service:\n    */ MDNS.addService("http", "tcp", 80);', "*/ MDNS"),
                ('mdns.addService("http", "tcp", 80);', "mdns.addService")):
            with self.subTest(new=new):
                try:
                    self.edit(INO, boot_http, f"\n{START_INDENT['boot']}{new}")
                    self.assert_refused_at("registers an mDNS service in a form this generator cannot read", needle)
                finally:
                    self.tearDown()

    def test_a_service_call_the_generator_never_reads_is_named_as_such(self):
        # MDNS.enableArduino() and enableWorkstation(), and IDF's own
        # mdns_service_*, are never read: a line naming one is refused by
        # that name, as a call or as a definition by such a name (wave 23's
        # review: a definition is no registration "in a form this generator
        # cannot read", so the refusal says what it saw).
        boot_http = self.at("boot", HTTP)
        before_boot_http = f"\n{START_INDENT['boot']}{{}}{boot_http}"
        anchor = "static void mdns_reannounce() {"
        for old, new, name, needle in (
                (boot_http, before_boot_http.format("MDNS.enableArduino(3232);"),
                 "enableArduino", "MDNS.enableArduino(3232);"),
                (boot_http, before_boot_http.format("MDNS.enableWorkstation();"),
                 "enableWorkstation", "MDNS.enableWorkstation();"),
                (boot_http, before_boot_http.format('mdns_service_add(nullptr, "_http", "_tcp", 8080, nullptr, 0);'),
                 "mdns_service_add", "mdns_service_add("),
                (boot_http, before_boot_http.format('mdns_service_port_set("_http", "_tcp", 8080);'),
                 "mdns_service_port_set", "mdns_service_port_set("),
                (anchor, f"static void mdns_service_refresh() {{\n}}\n\n{anchor}",
                 "mdns_service_refresh", "static void mdns_service_refresh() {")):
            with self.subTest(name=name):
                try:
                    self.edit(INO, old, new)
                    got = self.assert_refused_at(f"the sketch names {name}(), an mDNS service call this generator "
                                                 "does not read", needle)
                    self.assertNotIn("registers an mDNS service in a form", got)
                finally:
                    self.tearDown()

    def test_a_preprocessor_line_naming_an_mdns_call_is_refused_as_a_macro(self):
        # A #define that names MDNS.begin() or a service call is no start and
        # no registration (wave 23's review): it is refused as a macro the
        # reader does not expand, not counted as a start that then "registers
        # no service", and not as a registration in a form it cannot read.
        # The macro's reason comes before the condition's (the fix is the
        # same); under #if 0 it is nothing, like any dead line.
        anchor = "static void mdns_reannounce() {"
        why = "a preprocessor line names an mDNS start or service call"
        for macro in ("#define MDNS_START() MDNS.begin(g_device.mdns_hostname)",
                      '#define REG_HTTP() MDNS.addService("http", "tcp", 8080)',
                      '#  define REG_HTTP() MDNS.addService("http", "tcp", 80)  // macro',
                      '#define MDNS_HTTP_PORT(p) mdns_service_port_set("_http", "_tcp", p)'):
            with self.subTest(macro=macro):
                try:
                    self.edit(INO, anchor, f"{macro}\n{anchor}")
                    got = self.assert_refused_at(why, macro)
                    self.assertIn("does not expand macros", got)
                    self.assertNotIn("the mDNS start here registers no", got)
                    self.assertNotIn("in a form this generator cannot read", got)
                    self.assertNotIn("under a condition this generator does not evaluate", got)
                finally:
                    self.tearDown()
        macro = '#define REG_HTTP() MDNS.addService("http", "tcp", 8080)'
        with self.subTest(macro="under #if FEATURE_X"):
            try:
                self.edit(INO, anchor, f"#if FEATURE_X\n{macro}\n#endif\n{anchor}")
                got = self.assert_refused_at(why, macro)
                self.assertNotIn("under a condition this generator does not evaluate", got)
            finally:
                self.tearDown()
        with self.subTest(macro="under #if 0"):
            try:
                self.edit(INO, anchor, f"#if 0\n{macro}\n#endif\n{anchor}")
                self.assertEqual(self.generate(), self.base)
            finally:
                self.tearDown()

    def test_an_mdns_port_out_of_range_is_refused(self):
        for value in ("65536", "99999"):
            with self.subTest(value=value):
                try:
                    new = HTTP.replace(" 80);", f" {value});")
                    self.edit(INO, self.at("boot", HTTP), self.at("boot", new))
                    self.assert_refused_at("which is not a TCP port", new)
                finally:
                    self.tearDown()

    def test_mdns_starts_that_disagree_are_refused(self):
        for start in START_INDENT:
            for svc, line in (("http", HTTP), ("securacv", SECURACV)):
                with self.subTest(start=start, service=svc):
                    try:
                        new = line.replace(" 80);", " 8080);")
                        self.edit(INO, self.at(start, line), self.at(start, new))
                        self.assert_refused_at(f"the mDNS starts disagree on the {svc} service's port", new)
                    finally:
                        self.tearDown()

    def test_a_start_that_does_not_register_a_service_is_refused(self):
        # Deleted at any start, or at the boot announce commented out, wrapped
        # in a block comment or under #if 0: that start no longer advertises
        # the service, whatever the others do, and the refusal names the
        # start's MDNS.begin() line (the starts are in START_INDENT's order).
        begins = [i + 1 for i, ln in enumerate(self.pristine[INO].split("\n"))
                  if "MDNS.begin(g_device.mdns_hostname)" in ln]
        self.assertEqual(len(begins), len(START_INDENT), begins)
        begin = dict(zip(START_INDENT, begins))
        cases = [(start, "") for start in START_INDENT]
        cases += [("boot", wrap) for wrap in ("// {}", "/*\n{}\n*/", "#if 0\n{}\n#endif")]
        for start, wrap in cases:
            for svc, line in (("http", HTTP), ("securacv", SECURACV)):
                with self.subTest(start=start, service=svc, wrap=wrap):
                    try:
                        self.edit(INO, self.at(start, line),
                                  f"\n{START_INDENT[start]}" + wrap.format(line) if wrap else "")
                        got = self.generate()
                        self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
                        self.assertIn(f"{INO_NAME}:{begin[start]}: the mDNS start here registers no {svc} service",
                                      got)
                    finally:
                        self.tearDown()

    def test_a_third_service_or_a_second_registration_is_refused(self):
        boot_http = self.at("boot", HTTP)
        for extra, why in (('MDNS.addService("ssh", "tcp", 22);', "registers a 'ssh' service"),
                           (HTTP.replace(" 80);", " 8080);"), "registers 'http' twice"),
                           (HTTP + " // the same again", "registers 'http' twice")):
            with self.subTest(extra=extra):
                try:
                    self.edit(INO, boot_http, boot_http + self.at("boot", extra))
                    self.assert_refused_at(why, extra)
                finally:
                    self.tearDown()

    def test_a_registration_the_compiler_never_sees_is_not_counted(self):
        # A second http line at 8080 in a comment, a log string, or a dead
        # branch moves nothing; in the #else of an #if 0, which is compiled,
        # it is counted.
        boot_http = self.at("boot", HTTP)
        other = HTTP.replace(" 80);", " 8080);")
        for wrap in ("// {}", "/*\n    {}\n    */", "#if 0\n    {}\n#endif",
                     "#if 0\n#if FEATURE_X\n    {}\n#endif\n#endif",
                     'log_health(SCV_LOG_INFO, SCV_CAT_NETWORK, "{}", nullptr);'):
            body = wrap.format(other.replace('"', '\\"') if wrap.startswith("log_health") else other)
            with self.subTest(wrap=wrap):
                try:
                    self.edit(INO, boot_http, boot_http + f"\n{START_INDENT['boot']}{body}")
                    self.assertEqual(self.generate(), self.base)
                finally:
                    self.tearDown()
        with self.subTest(branch="#else"):
            try:
                self.edit(INO, boot_http, f"\n#if 0\n    {HTTP}\n#else\n    {other}\n#endif")
                self.assert_refused_at("the mDNS starts disagree on the http service's port", other)
            finally:
                self.tearDown()

    def test_a_registration_under_a_condition_not_evaluated_is_refused(self):
        # The reader evaluates #if 0 and its #else only. Under any other
        # condition the compiler may drop the line, and the page would list a
        # service the device does not advertise: refused, naming the line and
        # the condition's line, for the start's only http line and for the
        # MDNS.begin() itself.
        boot_http = self.at("boot", HTTP)
        for wrap in ("#if FEATURE_MDNS_HTTP\n{}\n#endif", "#ifdef MDNS_HTTP\n{}\n#endif",
                     "#ifndef NO_MDNS_HTTP\n{}\n#endif", "#if 1\n{}\n#endif",
                     "#if 0\n#elif FEATURE_X\n{}\n#endif", "#if FEATURE_X\n#else\n{}\n#endif",
                     "#if FEATURE_X\n#if 0\n#else\n{}\n#endif\n#endif"):
            with self.subTest(wrap=wrap):
                try:
                    self.edit(INO, boot_http, "\n" + wrap.format(boot_http.lstrip("\n")))
                    lines = (self.root / INO).read_text(encoding="utf-8").split("\n")
                    line = lines.index(boot_http.lstrip("\n")) + 1
                    cond = next(n + 1 for n in range(line - 1, -1, -1)
                                if lines[n].startswith(("#if F", "#ifdef", "#ifndef", "#if 1", "#elif")))
                    got = self.generate()
                    self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
                    self.assertIn(f"{INO_NAME}:{line}: the sketch starts mDNS or registers a service under a "
                                  "condition this generator does not evaluate", got)
                    self.assertIn(f"under line {cond}: ", got)
                finally:
                    self.tearDown()
        with self.subTest(begin="#if FEATURE_MDNS"):
            begin = "\n  if (MDNS.begin(g_device.mdns_hostname)) {"
            self.edit(INO, begin, "\n#if FEATURE_MDNS" + begin + "\n#endif")
            cond = self.line_no("#if FEATURE_MDNS")
            got = self.generate()
            self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
            self.assertIn(f"{INO_NAME}:{cond + 1}: the sketch starts mDNS or registers a service under a "
                          f"condition this generator does not evaluate", got)
            self.assertIn(f"under line {cond}: #if FEATURE_MDNS", got)

    def test_a_fourth_start_is_held_to_the_same_rules(self):
        # An agreeing fourth start moves nothing (the page's services are
        # still what the device advertises); one that disagrees or registers
        # nothing is refused at its line.
        anchor = "static void mdns_reannounce() {"

        def fourth(body: str) -> str:
            return ("static void mdns_fourth() {\n  if (MDNS.begin(g_device.mdns_hostname)) {\n"
                    + body + "  }\n}\n\n" + anchor)

        cases = ((f"    {HTTP}\n    {SECURACV}\n", None, None),
                 (f"    {HTTP.replace(' 80);', ' 8080);')}\n    {SECURACV}\n",
                  "the mDNS starts disagree on the http service's port", "8080"),
                 ("", "registers no http and no securacv service", "static void mdns_fourth() {"))
        for body, why, needle in cases:
            with self.subTest(body=body):
                try:
                    self.edit(INO, anchor, fourth(body))
                    if why is None:
                        self.assertEqual(self.generate(), self.base)
                    else:
                        got = self.generate()
                        self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
                        self.assertIn(why, got)
                        line = self.line_no(needle) + (1 if needle.startswith("static void") else 0)
                        self.assertRegex(got, rf"\b{re.escape(INO_NAME)}:{line}\b")
                finally:
                    self.tearDown()

    def test_a_registration_before_any_start_is_refused(self):
        anchor = "static void mdns_sync_broker_txt() {"
        self.edit(INO, anchor, f"static void mdns_early() {{\n  {HTTP}\n}}\n\n{anchor}")
        got = self.generate()
        self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
        self.assertIn("registers an mDNS service before any MDNS.begin()", got)
        # The planted line is the first of the file's 2-space http lines.
        lines = (self.root / INO).read_text(encoding="utf-8").split("\n")
        self.assertIn(f"{INO_NAME}:{lines.index('  ' + HTTP) + 1}:", got)

    def test_an_mdns_call_in_another_sketch_file_is_refused(self):
        extra = self.root / SKETCH / "mdns_extra.cpp"
        for text, refused in (('void mdns_extra() {\n  MDNS.addService("http", "tcp", 8080);\n}\n', True),
                              ("void mdns_extra() {\n  if (MDNS.begin(\"canary\")) {}\n}\n", True),
                              ('void mdns_extra() {\n  mdns_service_port_set("_http", "_tcp", 8080);\n}\n', True),
                              ('// MDNS.addService("http", "tcp", 8080);\nvoid mdns_extra() {}\n', False)):
            with self.subTest(text=text):
                try:
                    extra.write_text(text, encoding="utf-8")
                    got = self.generate()
                    if refused:
                        self.assertIsInstance(got, str, "generated a page from a sketch it cannot read")
                        self.assertIn("mdns_extra.cpp:2: mDNS is started or a service registered outside", got)
                    else:
                        self.assertEqual(got, self.base)
                finally:
                    extra.unlink(missing_ok=True)

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
