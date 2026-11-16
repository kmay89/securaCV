#!/usr/bin/env python3
"""The patched core-2 WebServer: its bytes, and every build that must link it.

arduino-esp32 2.0.17's WebServer sizes a stack array from the client's
multipart boundary before any route handler runs (GHSA-8cmm-3887-r32j), and no
2.x release fixes it. firmware/third_party/WebServer-2.0.17-securacv is that
library with the fix backported, and the core-2 builds that serve HTTP link it
instead of the core's copy (its README.md). Three things have to stay true, and
each fails quietly when it stops being true: a stray edit to the vendored copy
compiles; an env that stops listing the storage links the core's copy and
builds green; a core-3 env that inherits it links a 2.0.17 library into a 3.x
core. So:

(a) Bytes. Every file in the directory is in the README's provenance table, and
    every file the table lists is there. A `verbatim` file hashes to the
    upstream sha256 the table records. A `patched` file, with exactly the hunks
    in PATCHES taken back out (each one present once), hashes to its upstream
    sha256: its diff from upstream is those hunks and nothing else. The storage
    (firmware/third_party, which those envs list in lib_extra_dirs) holds no
    library but this one, since every library in it is visible to them.
(b) PlatformIO. Every env of every project, resolved through extends,
    extra_configs and ${section.option} by the same reader the firmware SBOM
    uses (scripts/gen_firmware_sbom.py's PioConfig; its --verify-with-pio
    proves that reader against `pio project config`). An env on
    [platform_s3c3] (core 2.0.17) that compiles a WebServer user (a source with
    `#include <WebServer.h>`, through its build_src_filter, or a firmware/common
    storage in its lib_extra_dirs that the LDF could build one from) lists the
    storage. No env on any other platform does.
(c) Arduino CLI. Every workflow step that compiles the display sketch, on
    every matrix leg it runs on, with its core taken from the setup action
    before it (or from sketch.yaml, for a --profile build): on core 2.0.17 it
    passes --library with the vendored directory and hands the compile's JSON
    result to firmware/scripts/check_webserver_link.py with --expect; on any
    other core it does not name it.
(d) The link check itself: a PlatformIO log whose dependency graph names the
    core's WebServer (2.0.0) fails, and an arduino-cli result passes only
    when its one WebServer is the expected directory. The release workflows
    run it on the Vision and display builds too, and withhold an image that
    fails it.

The behavior of the patch itself is firmware/tests_host/test_webserver_boundary.cpp.
Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import contextlib
import glob
import hashlib
import importlib.util
import io
import itertools
import json
import os
import re
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
FIRMWARE = REPO / "firmware"
STORAGE = FIRMWARE / "third_party"
LIB = STORAGE / "WebServer-2.0.17-securacv"
LIB_REL = "firmware/third_party/WebServer-2.0.17-securacv"
README = LIB / "README.md"
WORKFLOWS = REPO / ".github" / "workflows"
SKETCH_YAML = FIRMWARE / "projects" / "canary-display" / "arduino" / "canary_display" / "sketch.yaml"
CORE2 = "2.0.17"

_spec = importlib.util.spec_from_file_location("gen_firmware_sbom", REPO / "scripts" / "gen_firmware_sbom.py")
gs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gs)  # type: ignore[union-attr]

# (upstream text, our text) for each hunk of each patched file, in file order.
PATCHES: dict[str, list[tuple[str, str]]] = {
    "library.properties": [
        ("version=2.0.0\n", "version=2.0.17+securacv.1\n"),
    ],
    "src/Parsing.cpp": [
        (
            "  Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)\n*/\n",
            "  Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)\n"
            "  Modified 16 Nov 2026 for SecuraCV (GHSA-8cmm-3887-r32j: multipart boundary capped at 70,"
            " fixed-size boundary buffer)\n*/\n",
        ),
        (
            '          boundaryStr.replace("\\"","");\n'
            "          isForm = true;\n",
            '          boundaryStr.replace("\\"","");\n'
            "          if (boundaryStr.length() > 70){  // RFC 2046: max boundary length is 70"
            " (SecuraCV backport of upstream bb86048, GHSA-8cmm-3887-r32j)\n"
            '            log_e("Invalid boundary length: %u", boundaryStr.length());\n'
            "            return false;\n"
            "          }\n"
            "          isForm = true;\n",
        ),
        (
            "bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){\n"
            "  (void) len;\n",
            "bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){\n"
            "  (void) len;\n"
            "  if (boundary.length() > 70) return false;  // SecuraCV: the cap _parseRequest applies,"
            " held here too, so fastBoundary below can never overflow\n",
        ),
        (
            "            char fastBoundary[ fastBoundaryLen ];\n",
            "            char fastBoundary[ 4 /* \\r\\n-- */ + 70 /* RFC 2046 max */ + 1 /* \\0 */ ];"
            "  // SecuraCV: fixed size, never sized by the client (GHSA-8cmm-3887-r32j)\n",
        ),
    ],
}

_ROW_RE = re.compile(r"^\| `([^`]+)` \| `([^`]+)` \| `([0-9a-f]{64})` \| (verbatim|patched) \|$")


def provenance() -> dict[str, tuple[str, str]]:
    """{path here: (upstream sha256, state)} from the README's table."""
    rows = {}
    for line in README.read_text(encoding="utf-8").splitlines():
        m = _ROW_RE.match(line)
        if m:
            rows[m.group(1)] = (m.group(3), m.group(4))
    return rows


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class VendoredBytes(unittest.TestCase):
    def test_every_file_is_in_the_table_and_the_table_is_complete(self):
        on_disk = {p.relative_to(LIB).as_posix() for p in LIB.rglob("*") if p.is_file()}
        on_disk.discard("README.md")
        self.assertGreaterEqual(len(provenance()), 15)
        self.assertEqual(on_disk, set(provenance()))

    def test_verbatim_files_hash_to_upstream(self):
        for rel, (want, state) in provenance().items():
            if state == "verbatim":
                with self.subTest(rel):
                    self.assertEqual(sha256((LIB / rel).read_bytes()), want,
                                     f"{rel} differs from arduino-esp32 2.0.17's")

    def test_patched_files_differ_from_upstream_by_exactly_the_hunks(self):
        patched = {rel for rel, (_, state) in provenance().items() if state == "patched"}
        self.assertEqual(patched, set(PATCHES))
        for rel, hunks in PATCHES.items():
            with self.subTest(rel):
                text = (LIB / rel).read_text(encoding="utf-8")
                for upstream, ours in hunks:
                    self.assertEqual(text.count(ours), 1, f"{rel}: hunk not present exactly once:\n{ours}")
                    text = text.replace(ours, upstream)
                self.assertEqual(sha256(text.encode("utf-8")), provenance()[rel][0],
                                 f"{rel}: with the hunks taken out it is not upstream's file, "
                                 f"so it carries a change PATCHES does not list")

    def test_no_stack_buffer_is_sized_from_the_boundary(self):
        text = (LIB / "src" / "Parsing.cpp").read_text(encoding="utf-8")
        self.assertNotIn("char fastBoundary[ fastBoundaryLen ]", text)

    def test_the_storage_holds_only_this_library(self):
        entries = {p.name for p in STORAGE.iterdir()}
        self.assertEqual(entries - {"README.md"}, {LIB.name},
                         "every library in firmware/third_party is visible to each core-2 env "
                         "that lists it; add one deliberately, and here")


def webserver_users() -> set[Path]:
    """Sources the PlatformIO builds compile that include Arduino WebServer."""
    out = set()
    for path in FIRMWARE.rglob("*"):
        if path.suffix not in (".cpp", ".h", ".c", ".ino") or not path.is_file():
            continue
        parts = set(path.relative_to(FIRMWARE).parts)
        if parts & {"third_party", "tests_host", "arduino", "_archive", ".pio"}:
            continue
        if re.search(r"^\s*#\s*include\s*[<\"]WebServer\.h[>\"]",
                     path.read_text(encoding="utf-8", errors="ignore"), re.M):
            out.add(path.resolve())
    return out


def compiled_sources(src_dir: Path, src_filter: str) -> set[Path]:
    """PlatformIO's build_src_filter over src_dir: `+<glob>` adds, `-<glob>`
    removes, a matched directory stands for every file under it."""
    files: set[Path] = set()
    for sign, pattern in re.findall(r"([+-])<([^>]*)>", src_filter or "+<*>"):
        matched: set[Path] = set()
        for hit in glob.glob(os.path.join(src_dir, pattern), recursive=True):
            hit_path = Path(hit)
            if hit_path.is_dir():
                matched |= {p.resolve() for p in hit_path.rglob("*") if p.is_file()}
            elif hit_path.is_file():
                matched.add(hit_path.resolve())
        files = files | matched if sign == "+" else files - matched
    return files


def projects() -> list[Path]:
    return sorted(p.parent for p in [FIRMWARE / "canary" / "platformio.ini",
                                     *FIRMWARE.glob("projects/*/platformio.ini")])


class PlatformIOEnvs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.users = webserver_users()
        cls.sections = gs.platform_sections()   # literal -> [platform_*] section
        cls.envs = []                           # (project, env, section, compiles a user, reaches LIB)
        for pdir in projects():
            cfg = gs.PioConfig(pdir)
            src_dir = pdir / cfg.sections.get("platformio", {}).get("src_dir", "src")
            for env in cfg.envs():
                opts = cfg.env(env)
                extra = [(pdir / e).resolve() for e in gs._lines(opts.get("lib_extra_dirs", ""))]
                reaches = any(e == STORAGE or e == LIB or STORAGE in e.parents for e in extra)
                via_common = any(e == (FIRMWARE / "common").resolve() for e in extra) and any(
                    (FIRMWARE / "common").resolve() in u.parents for u in cls.users)
                compiles = via_common or bool(
                    compiled_sources(src_dir, opts.get("build_src_filter", "")) & cls.users)
                section = cls.sections.get(opts.get("platform", ""), opts.get("platform", ""))
                cls.envs.append((pdir.name, env, section, compiles, reaches))

    def test_the_webserver_users_are_found(self):
        names = {p.name for p in self.users}
        self.assertTrue({"node_http.cpp", "setup_portal.cpp", "glass_web.cpp", "provision.cpp"} <= names, names)

    def test_every_core2_env_that_compiles_webserver_links_the_patched_copy(self):
        need = [(p, e) for p, e, s, compiles, _ in self.envs if s == "platform_s3c3" and compiles]
        # Not vacuous: the shipped core-2 HTTP products are all in the set.
        for env in ("canary-vision-default", "canary-vision-xiao-c3", "canary-vision-xiao-s3",
                    "canary-vision-c3-super-mini", "canary-display-watch", "canary-display-nightstand-s3",
                    "canary-display-touch169", "canary-display-amoled241",
                    "canary-display-nightlight-c3", "canary-sentinel-lite"):
            self.assertIn(env, {e for _, e in need})
        missing = [f"{p}:{e}" for p, e, s, compiles, reaches in self.envs
                   if s == "platform_s3c3" and compiles and not reaches]
        self.assertEqual(missing, [], "these core-2 envs compile Arduino WebServer but do not list "
                         "../../third_party in lib_extra_dirs, so they link core 2.0.17's own copy "
                         "(GHSA-8cmm-3887-r32j)")

    def test_no_other_env_reaches_it(self):
        self.assertIn("platform_core3", {s for _, _, s, _, _ in self.envs})
        wrong = [f"{p}:{e} ({s})" for p, e, s, _, reaches in self.envs if reaches and s != "platform_s3c3"]
        self.assertEqual(wrong, [], "a 2.0.17 library reaches an env on another core; core 3.3.8 and "
                         "later carry the upstream fix. State the env's own lib_extra_dirs without "
                         "../../third_party (as [env:canary-display-dash] does)")


_MATRIX_REF = re.compile(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)\s*\}\}")
_IF_TERM = re.compile(r"^matrix\.([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)\s*(==|!=)\s*'([^']*)'$")


def legs(job: dict) -> list[dict]:
    """Every matrix leg of a job, as {dimension: entry}; [{}] without a matrix."""
    matrix = (job.get("strategy") or {}).get("matrix") or {}
    if not isinstance(matrix, dict):
        return [{}]
    if "include" in matrix or "exclude" in matrix:
        raise AssertionError("include/exclude matrices are not modeled here; extend legs()")
    dims = {k: v for k, v in matrix.items() if isinstance(v, list)}
    return [dict(zip(dims, combo)) for combo in itertools.product(*dims.values())] or [{}]


def render(text: str, leg: dict) -> str:
    def sub(m: re.Match) -> str:
        entry = leg.get(m.group(1))
        return str(entry.get(m.group(2), "")) if isinstance(entry, dict) else m.group(0)
    return _MATRIX_REF.sub(sub, text)


def runs_on(condition, leg: dict) -> bool:
    """A step's `if:` on one leg: `matrix.a.b == 'x'` terms joined by &&;
    anything else (hashFiles, inputs) counts as running."""
    if not condition:
        return True
    for term in str(condition).split("&&"):
        m = _IF_TERM.match(term.strip())
        if not m:
            continue
        entry = leg.get(m.group(1))
        value = str(entry.get(m.group(2), "")) if isinstance(entry, dict) else ""
        if (value == m.group(4)) != (m.group(3) == "=="):
            return False
    return True


def profile_cores() -> dict[str, str]:
    profiles = (yaml.safe_load(SKETCH_YAML.read_text(encoding="utf-8")) or {}).get("profiles") or {}
    out = {}
    for name, body in profiles.items():
        for plat in (body or {}).get("platforms") or []:
            m = re.match(r"esp32:esp32\s*\(([^)]+)\)", str(plat.get("platform", "")))
            if m:
                out[str(name)] = m.group(1).strip()
    return out


def display_compiles() -> list[tuple[str, str, str, str]]:
    """(workflow, step, core, rendered run+env) for every leg on which a step
    compiles the display sketch."""
    profiles = profile_cores()
    out = []
    for wf in sorted(WORKFLOWS.glob("*.yml")):
        doc = yaml.safe_load(wf.read_text(encoding="utf-8")) or {}
        for job in (doc.get("jobs") or {}).values():
            if not any("arduino-cli compile" in str(st.get("run", "")) and
                       "canary_display.ino" in str(st.get("run", "")) for st in job.get("steps") or []):
                continue
            for leg in legs(job):
                core = None
                for step in job.get("steps") or []:
                    if "setup-arduino-esp32" in str(step.get("uses", "")):
                        core = render(str((step.get("with") or {}).get("core-version", "")), leg) or "latest"
                    run = str(step.get("run", ""))
                    if "arduino-cli compile" not in run or "canary_display.ino" not in run:
                        continue
                    if not runs_on(step.get("if"), leg):
                        continue
                    text = render(run + "\n" + "\n".join(
                        f"{k}={v}" for k, v in (step.get("env") or {}).items()), leg)
                    m = re.search(r"--profile\s+(\S+)", text)
                    step_core = profiles.get(m.group(1), "?") if m else core
                    out.append((wf.name, render(str(step.get("name", "")), leg), step_core, text))
    return out


class ArduinoCliRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = display_compiles()

    def test_the_core2_rows_are_found(self):
        core2 = {(wf, name) for wf, name, core, _ in self.rows if core == CORE2}
        for wf in ("firmware-release.yml", "flasher-release.yml", "firmware.yml"):
            self.assertTrue(any(w == wf for w, _ in core2), f"no core {CORE2} display compile found in {wf}")

    def test_every_core2_row_passes_the_patched_library(self):
        for wf, name, core, text in self.rows:
            if core != CORE2:
                continue
            with self.subTest(f"{wf}: {name}"):
                self.assertIn("--library", text)
                self.assertIn(LIB_REL, text, "a core 2.0.17 display build without the patched "
                              "WebServer links the core's own (GHSA-8cmm-3887-r32j)")

    def test_no_other_core_names_it(self):
        self.assertTrue(any(core != CORE2 for _, _, core, _ in self.rows))
        for wf, name, core, text in self.rows:
            if core != CORE2:
                with self.subTest(f"{wf}: {name} ({core})"):
                    self.assertNotIn(LIB_REL, text)

    def test_every_core2_row_checks_what_it_linked(self):
        # Passing --library is not proof the build used it: every 2.0.17 row
        # compiles with --format json and hands the result to
        # check_webserver_link.py with --expect, which fails (or withholds the
        # image of) a build that linked any other WebServer.
        for wf, name, core, text in self.rows:
            if core != CORE2:
                continue
            with self.subTest(f"{wf}: {name}"):
                self.assertRegex(text, r"arduino-cli compile --format json")
                self.assertRegex(text, r"check_webserver_link\.py\" cli |check_webserver_link\.py cli ")
                self.assertIn("--expect", text)


def _checker():
    spec = importlib.util.spec_from_file_location(
        "check_webserver_link", FIRMWARE / "scripts" / "check_webserver_link.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


class LinkCheck(unittest.TestCase):
    """(d) firmware/scripts/check_webserver_link.py, which the release and CI
    rows run on what a build says it linked."""

    @classmethod
    def setUpClass(cls):
        cls.ck = _checker()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, name: str, text: str) -> str:
        p = self.dir / name
        p.write_text(text, encoding="utf-8")
        return str(p)

    def _quiet(self, fn, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return fn(*args)

    def test_pio_fails_the_core_copy_and_passes_the_patched_one(self):
        graph = ("Dependency Graph\n|-- WiFi @ 2.0.0\n|-- {}\n|   |-- FS @ 2.0.0\n"
                 "Building in release mode\n")
        core = self._write("core.log", graph.format("WebServer @ 2.0.0"))
        ours = self._write("ours.log", graph.format("WebServer @ 2.0.17+securacv.1"))
        none = self._write("none.log", graph.format("ESPmDNS @ 2.0.0"))
        core3 = self._write("core3.log", graph.format("WebServer @ 3.3.12"))
        self.assertEqual(self._quiet(self.ck.pio, core), 1)
        self.assertEqual(self._quiet(self.ck.pio, ours), 0)
        self.assertEqual(self._quiet(self.ck.pio, none), 0)
        self.assertEqual(self._quiet(self.ck.pio, core3), 0)

    def _result(self, libs, success=True) -> str:
        return self._write("result.json", json.dumps({
            "compiler_out": "Sketch uses 1 bytes\n", "compiler_err": "", "success": success,
            "builder_result": {"used_libraries": libs}}))

    def test_cli_holds_the_one_webserver_to_the_expected_directory(self):
        lib = self.dir / "WebServer-2.0.17-securacv"
        lib.mkdir()
        ours = {"name": "WebServer", "version": "2.0.17+securacv.1", "install_dir": str(lib) + "/"}
        core = {"name": "WebServer", "version": "2.0.0",
                "install_dir": "/root/.arduino15/packages/esp32/hardware/esp32/2.0.17/libraries/WebServer"}
        wifi = {"name": "WiFi", "version": "2.0.0", "install_dir": "/x/WiFi"}
        self.assertEqual(self._quiet(self.ck.cli, self._result([wifi, ours]), str(lib)), 0)
        self.assertEqual(self._quiet(self.ck.cli, self._result([wifi, core]), str(lib)), 1)
        self.assertEqual(self._quiet(self.ck.cli, self._result([wifi]), str(lib)), 1)
        self.assertEqual(self._quiet(self.ck.cli, self._result([ours, core]), str(lib)), 1)
        self.assertEqual(self._quiet(self.ck.cli, self._result([]), str(lib)), 1)

    def test_cli_reports_a_failed_compile_and_text_that_is_not_json(self):
        self.assertEqual(self._quiet(self.ck.cli, self._result([], success=False), "/x"), 2)
        self.assertEqual(self._quiet(self.ck.cli, self._write("t.txt", "Sketch uses 1 bytes\n"), "/x"), 2)

    def test_cli_without_expect_only_reports_the_compile(self):
        core = {"name": "WebServer", "version": "2.0.0", "install_dir": "/core/WebServer"}
        self.assertEqual(self._quiet(self.ck.cli, self._result([core]), None), 0)
        self.assertEqual(self._quiet(self.ck.cli, self._result([core], success=False), None), 2)


if __name__ == "__main__":
    unittest.main()
