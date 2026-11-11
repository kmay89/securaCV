#!/usr/bin/env python3
"""Pins scripts/gen_firmware_sbom.py — the firmware SBOM is derived, not typed.

What is pinned and why:
  • the PlatformIO-ini resolver: `[env]` as the implicit base, `extends`
    chains, `extra_configs`, `${section.option}` interpolation with ':' in a
    section name, comment lines inside a multi-line value — the shapes the
    real inis use (gen_firmware_sbom.py --verify-with-pio proves it against
    PlatformIO itself where `pio` is installed; this suite has no `pio`);
  • lib_deps spec parsing: exact vs range, owner/name, a URL dependency;
  • the committed sbom/sbom-firmware.cdx.json matches the tree, is
    deterministic, has no timestamp, and its dependency graph is closed;
  • the audit claim that started this: the display and WAP products depend
    on the pioarduino core-3 line (Arduino 3.3.8 / ESP-IDF 5.5.4), which the
    hand-typed template never said;
  • --check goes red on a byte of drift;
  • --validate: the committed document and the timestamped artifact variant
    are strict CycloneDX 1.5 (cyclonedx-python-lib's JsonStrictValidator),
    and an unknown key, a URL with a space and a bad component type are
    refused — the validator is imported unconditionally, because lint.yml's
    tests step installs it and a skipTest on ImportError would read as
    covered while catching nothing; a file claiming another specVersion is
    refused (the schema types it as a free string); a missing or non-JSON
    FILE is one error line; bare --validate refuses a write flag beside it;
  • the validator's pip pin is one string across lint.yml, sbom.yml, the
    README and the generator;
  • sbom.yml's own two tools are pinned exactly (CI26): every `cargo install`
    and `npm install -g` names an x.y.z from the job env (cargo with
    --locked), the cargo-cyclonedx binary is cached under that version and
    nothing that rotates, the check refuses each way back to floating, and
    the README's tool rows say the same versions;
  • the two Arduino axes: sketch.yaml profiles are read from YAML (the
    files' comments quote core numbers too), workflow rows carry their
    matrix-resolved library pins and the sketch they compile, the agreement
    check is green on the tree and red on a workflow pin no profile carries,
    a sketch pin neither axis knows, a library off its core line (and the
    PlatformIO-core allowance is per product) — a finding fails generation;
    a sketch.yaml with no profiles is refused.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import re
import shlex
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_firmware_sbom.py"

spec = importlib.util.spec_from_file_location("gen_firmware_sbom", SCRIPT)
gs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gs)  # type: ignore[union-attr]


def _write(root: Path, rel: str, body: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body), encoding="utf-8")


class IniResolver(unittest.TestCase):
    def test_extends_interpolation_extra_configs_and_comments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "shared/pins.ini", """
                [platform_x]
                ; the one literal
                platform = espressif32@6.9.0
            """)
            _write(root, "shared/common.ini", """
                [common]
                framework = arduino
                lib_deps =
                    bblanchon/ArduinoJson@^7.0.0

                [common_s3]
                extends = common
                platform = ${platform_x.platform}
                build_flags =
                    -DBASE
            """)
            _write(root, "proj/platformio.ini", """
                [platformio]
                extra_configs =
                    ../shared/pins.ini
                    ../shared/common.ini

                [env]
                monitor_speed = 115200

                [env:a]
                extends = common_s3
                board = seeed_xiao_esp32s3
                lib_deps =
                    ${common.lib_deps}
                    ; a comment between entries is dropped, as PlatformIO does
                    knolleary/PubSubClient@^2.8 ; inline comment stripped
                lib_extra_dirs =
                    ../../common

                [env:b]
                extends = env:a
                build_flags =
                    ${env:a.build_flags}
                    -DB
            """)
            cfg = gs.PioConfig(root / "proj")
            a = cfg.env("a")
            self.assertEqual(a["platform"], "espressif32@6.9.0")
            self.assertEqual(a["framework"], "arduino")
            self.assertEqual(a["monitor_speed"], "115200")          # [env] base
            self.assertEqual(gs._lines(a["lib_deps"]),
                             ["bblanchon/ArduinoJson@^7.0.0", "knolleary/PubSubClient@^2.8"])
            b = cfg.env("b")
            self.assertEqual(b["board"], "seeed_xiao_esp32s3")      # via extends = env:a
            self.assertEqual(gs._lines(b["build_flags"]), ["-DBASE", "-DB"])
            self.assertEqual(gs._lines(b["lib_deps"]), gs._lines(a["lib_deps"]))
            self.assertEqual(sorted(cfg.envs()), ["a", "b"])
            facts = gs.env_facts({"name": "p"}, cfg, "a")
            self.assertTrue(facts["reaches_common"])
            self.assertEqual(facts["lib_extra_dirs"], ["../../common"])

    def test_unknown_placeholders_are_left_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "platformio.ini", """
                [env:x]
                build_flags = -I${PROJECT_DIR}/include -I${platformio.packages_dir}/fw ${sysenv.EXTRA}
            """)
            x = gs.PioConfig(root).env("x")
            self.assertEqual(x["build_flags"],
                             "-I${PROJECT_DIR}/include -I${platformio.packages_dir}/fw ${sysenv.EXTRA}")


class LibSpecs(unittest.TestCase):
    def test_registry_specs(self):
        cases = {
            "rweather/Crypto @ ^0.4.0": ("rweather", "Crypto", "^0.4.0", False),
            "bblanchon/ArduinoJson@^7.0.0": ("bblanchon", "ArduinoJson", "^7.0.0", False),
            "moononournation/GFX Library for Arduino@1.6.6":
                ("moononournation", "GFX Library for Arduino", "1.6.6", True),
            "h2zero/NimBLE-Arduino@~1.4.3": ("h2zero", "NimBLE-Arduino", "~1.4.3", False),
            "SomeLib": (None, "SomeLib", None, False),
        }
        for spec_text, (group, name, version, exact) in cases.items():
            lib = gs.parse_lib_spec(spec_text)
            self.assertEqual((lib["group"], lib["name"], lib["spec"], lib["exact"], lib["url"]),
                             (group, name, version, exact, None), spec_text)

    def test_url_spec(self):
        lib = gs.parse_lib_spec(
            "https://github.com/Seeed-Studio/Seeed_Arduino_SSCMA/archive/refs/tags/v1.0.3.zip")
        self.assertEqual((lib["group"], lib["name"], lib["spec"], lib["exact"]),
                         ("Seeed-Studio", "Seeed_Arduino_SSCMA", "1.0.3", True))
        self.assertTrue(lib["url"].startswith("https://github.com/Seeed-Studio/"))


class CommittedDocument(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = gs.render(gs.build_document())
        cls.doc = json.loads(cls.text)

    def test_matches_the_committed_file_and_is_deterministic(self):
        self.assertEqual(gs.OUT.read_text(encoding="utf-8"), self.text)
        self.assertEqual(gs.render(gs.build_document()), self.text)

    def test_shape(self):
        d = self.doc
        self.assertEqual((d["bomFormat"], d["specVersion"]), ("CycloneDX", "1.5"))
        self.assertRegex(d["serialNumber"],
                         r"^urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
        self.assertNotIn("timestamp", d["metadata"])
        refs = [c["bom-ref"] for c in d["components"]]
        self.assertEqual(refs, sorted(refs))
        self.assertEqual(len(refs), len(set(refs)))
        known = set(refs) | {d["metadata"]["component"]["bom-ref"]}
        for dep in d["dependencies"]:
            self.assertIn(dep["ref"], known)
            for target in dep["dependsOn"]:
                self.assertIn(target, known, (dep["ref"], target))
            self.assertEqual(dep["dependsOn"], sorted(dep["dependsOn"]))
        for c in d["components"]:
            for ref in c.get("externalReferences", []):
                self.assertNotIn(" ", ref["url"], ref["url"])

    def test_the_shipped_display_and_wap_ride_the_core3_line(self):
        deps = {dep["ref"]: set(dep["dependsOn"]) for dep in self.doc["dependencies"]}
        self.assertIn("platform:platform_core3", deps["product:canary-display"])
        self.assertIn("platform:platform_s3c3", deps["product:canary-display"])
        self.assertEqual(deps["product:canary-wap"] & {r for r in deps if r.startswith("platform:")},
                         {"platform:platform_core3"})
        self.assertEqual(deps["platform:platform_core3"], {"framework:arduino-esp32@3.3.8"})
        self.assertEqual(deps["framework:arduino-esp32@3.3.8"], {"framework:esp-idf@5.5.4"})
        self.assertEqual(deps["platform:platform_s3c3"], {"framework:arduino-esp32@2.0.17"})

    def test_platform_facts_cover_every_literal_platforms_ini_declares_in_use(self):
        in_use = {lit for lit, sec in gs.platform_sections().items()
                  if f"platform:{sec}" in {c["bom-ref"] for c in self.doc["components"]}}
        self.assertTrue(in_use)
        self.assertTrue(in_use <= set(gs.PLATFORM_FACTS), in_use - set(gs.PLATFORM_FACTS))

    def test_version_is_the_firmware_train(self):
        header = (REPO / "firmware/canary/include/canary_config.h").read_text(encoding="utf-8")
        train = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', header).group(1)
        self.assertEqual(self.doc["metadata"]["component"]["version"], train)
        for c in self.doc["components"]:
            if c["bom-ref"].startswith("product:"):
                self.assertEqual(c["version"], train, c["bom-ref"])

    def test_timestamp_only_when_asked(self):
        stamped = gs.build_document("2026-09-08T00:00:00Z")
        self.assertEqual(stamped["metadata"]["timestamp"], "2026-09-08T00:00:00Z")
        self.assertEqual(stamped["serialNumber"], self.doc["serialNumber"])
        self.assertEqual(stamped["components"], self.doc["components"])


class CheckMode(unittest.TestCase):
    def test_check_is_green_on_the_tree_and_red_on_drift(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "sbom.cdx.json"
            copy.write_text(gs.OUT.read_text(encoding="utf-8"), encoding="utf-8")
            with redirect_stdout(io.StringIO()):
                self.assertEqual(gs.main(["--check", "--out", str(copy)]), 0)
            copy.write_text(copy.read_text(encoding="utf-8").replace('"3.3.8"', '"3.3.9"'),
                            encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gs.main(["--check", "--out", str(copy)]), 1)
            self.assertIn("is stale", out.getvalue())


def _doc() -> dict:
    return json.loads(gs.OUT.read_text(encoding="utf-8"))


def _usage_error(argv: list[str]) -> int | None:
    """gs.main(argv)'s SystemExit code when argparse refuses the flags (2),
    else None — stdout and stderr swallowed."""
    with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
        try:
            gs.main(argv)
        except SystemExit as exc:
            return exc.code
    return None


class SchemaValidation(unittest.TestCase):
    """--validate: cyclonedx-python-lib's JsonStrictValidator against the
    CycloneDX 1.5 schema it bundles. Imported unconditionally — see the
    module docstring."""

    def test_the_committed_document_and_the_artifact_variant_are_strict_1_5(self):
        self.assertIsNone(gs.validate_document(gs.OUT.read_text(encoding="utf-8")))
        self.assertIsNone(gs.validate_document(
            gs.render(gs.build_document("2026-09-08T00:00:00Z"))))

    def test_an_unknown_key_is_refused(self):
        doc = _doc()
        doc["securacv"] = "not a CycloneDX field"
        problem = gs.validate_document(json.dumps(doc))
        self.assertIsNotNone(problem)
        self.assertIn("securacv", problem)
        self.assertTrue(problem.startswith("$:"), problem)   # located at the root

    def test_a_url_with_a_space_is_refused(self):
        # The README's original catch: an externalReferences url must be an
        # IRI, and the format checker that says so comes with the
        # json-validation extra — a bare jsonschema would pass this.
        doc = _doc()
        doc["components"][0]["externalReferences"] = [{"type": "website", "url": "https://x/a b"}]
        problem = gs.validate_document(json.dumps(doc))
        self.assertIsNotNone(problem)
        self.assertIn("a b", problem)
        self.assertIn("externalReferences", problem)

    def test_validate_flag_is_green_on_the_tree_and_red_on_a_doctored_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.cdx.json"
            doc = _doc()
            doc["components"][0]["type"] = "widget"
            bad.write_text(json.dumps(doc), encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gs.main(["--validate", str(bad)]), 1)
            self.assertIn("::error::", out.getvalue())
            self.assertIn("widget", out.getvalue())
            good = Path(tmp) / "sbom.cdx.json"
            good.write_text(gs.OUT.read_text(encoding="utf-8"), encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gs.main(["--check", "--validate", "--out", str(good)]), 0)
            self.assertIn("--validate: OK", out.getvalue())
            self.assertIn("--check: OK", out.getvalue())
            # bare --validate is read-only, and says so: a write flag beside
            # it (no --check) is refused up front, never dropped for a green
            # exit and no file. The FILE form ignores --out, so it refuses
            # that too.
            good.unlink()
            for argv in (["--validate", "--out", str(good)],
                         ["--validate", "--timestamp", "now"],
                         ["--validate", "--out", str(good), "--timestamp", "now"],
                         ["--validate", str(bad), "--out", str(good)]):
                self.assertEqual(_usage_error(argv), 2, argv)
            self.assertFalse(good.exists())

    def test_a_document_claiming_another_spec_version_is_refused(self):
        # The bundled schema types specVersion as a free string (examples,
        # no enum), so the schema alone cannot say which spec a file claims;
        # the OK line names 1.5, so the generator asserts it before the run.
        doc = _doc()
        doc["specVersion"] = "1.6"
        problem = gs.validate_document(json.dumps(doc))
        self.assertIsNotNone(problem)
        self.assertTrue(problem.startswith("$.specVersion:"), problem)
        self.assertIn("1.6", problem)
        self.assertIn(gs.SPEC_VERSION, problem)
        with tempfile.TemporaryDirectory() as tmp:
            claims16 = Path(tmp) / "claims16.cdx.json"
            claims16.write_text(json.dumps(doc), encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gs.main(["--validate", str(claims16)]), 1)
            self.assertIn("::error::", out.getvalue())
            self.assertIn("$.specVersion", out.getvalue())

    def test_a_missing_or_non_json_file_is_one_error_line_not_a_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "nope.cdx.json"
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gs.main(["--validate", str(missing)]), 1)
            self.assertIn("::error::", out.getvalue())
            self.assertIn(str(missing), out.getvalue())
            prose = Path(tmp) / "prose.cdx.json"
            prose.write_text("not json", encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gs.main(["--validate", str(prose)]), 1)
            self.assertIn("::error::", out.getvalue())
            self.assertIn("not JSON", out.getvalue())
            self.assertIsNotNone(gs.validate_document("not json"))


_PIN_RE = re.compile(r"cyclonedx-python-lib\[[^\]]*\]==[0-9A-Za-z.]+")


class ValidatorPin(unittest.TestCase):
    """One pin, five places: the generator's VALIDATOR_PIP is the string
    every workflow pip line, and the README, spell — a one-file bump would
    otherwise falsify the ImportError remedy and the lint.yml comment
    silently (the shape lint_fw_version_sync.sh exists to prevent)."""

    def test_every_workflow_pip_line_and_the_readme_carry_the_generator_pin(self):
        yaml = gs._yaml()
        quoted = f"'{gs.VALIDATOR_PIP}'"
        pip_lines: dict[str, list[str]] = {}
        for path in sorted(gs.WORKFLOWS.glob("*.yml")):
            wf = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            for job in (wf.get("jobs") or {}).values():
                for step in (job or {}).get("steps") or []:
                    for line in str((step or {}).get("run") or "").splitlines():
                        if "pip install" in line and "cyclonedx-python-lib" in line:
                            pip_lines.setdefault(path.name, []).append(line.strip())
        # the two gates install it — and nowhere else does
        self.assertEqual(sorted(pip_lines), ["lint.yml", "sbom.yml"], pip_lines)
        self.assertEqual(len(pip_lines["lint.yml"]), 2, pip_lines)   # tests step, byte gate
        self.assertEqual(len(pip_lines["sbom.yml"]), 1, pip_lines)
        for name, lines in pip_lines.items():
            for line in lines:
                self.assertIn(quoted, line, f"{name}: {line}")
        # and no text anywhere — a comment, the README, the source — quotes
        # a different pin of it
        readme = REPO / "sbom" / "README.md"
        self.assertIn(quoted, readme.read_text(encoding="utf-8"))
        for path in [*sorted(gs.WORKFLOWS.glob("*.yml")), readme, SCRIPT]:
            for found in _PIN_RE.findall(path.read_text(encoding="utf-8")):
                self.assertEqual(found, gs.VALIDATOR_PIP, path.name)


SBOM_YML = gs.WORKFLOWS / "sbom.yml"
# The tools sbom.yml installs to write the Rust and Node documents, each with
# the job env variable that pins it. A new `cargo install` or `npm install -g`
# in sbom.yml fails the test below until it is pinned the same way and named
# here.
SBOM_TOOLS = {"cargo-cyclonedx": "CARGO_CYCLONEDX_VERSION",
              "@cyclonedx/cdxgen": "CDXGEN_VERSION"}
# A bare x.y.z: `cargo install --version` installs exactly that release (an
# operator or a partial version would be a requirement it resolves), and to
# npm it is one version, not a range or a dist-tag.
_EXACT_VERSION = re.compile(r"\d+\.\d+\.\d+")
_ENV_REF = re.compile(r"\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*))")
_EXPR = re.compile(r"\$\{\{\s*(.*?)\s*\}\}")
# cargo install options that take a value (the value is not the crate name)
_CARGO_VALUED = {"--version", "--vers", "--root", "--target", "--target-dir",
                 "--features", "-F", "--git", "--branch", "--tag", "--rev",
                 "--path", "--registry", "--index", "--profile", "-j", "--jobs",
                 "--config", "-Z", "--bin", "--example", "--color"}


def _installs(line: str) -> list[tuple[str, str, str | None, set[str]]]:
    """(manager, package, version token or None, flags) for each package a
    `cargo install` or global `npm install` on this shell line installs."""
    try:
        words = shlex.split(line, comments=True)
    except ValueError:  # a quote this line opens and a later one closes
        words = [w.strip("'\"") for w in line.split()]
    found = []
    for i in range(len(words) - 1):
        if words[i] == "cargo" and words[i + 1] == "install":
            args, version, crates, j = words[i + 2:], None, [], 0
            while j < len(args) and args[j] not in {"&&", "||", ";", "|"}:
                arg = args[j]
                if arg in _CARGO_VALUED:
                    if arg in {"--version", "--vers"} and j + 1 < len(args):
                        version = args[j + 1]
                    j += 2
                    continue
                if arg.startswith(("--version=", "--vers=")):
                    version = arg.split("=", 1)[1]
                elif not arg.startswith("-"):
                    crates.append(arg)
                j += 1
            flags = {a for a in args[:j] if a.startswith("-")}
            for crate in crates:
                name, at, ver = crate.partition("@")
                found.append(("cargo", name, ver if at else version, flags))
        if words[i] == "npm" and words[i + 1] in {"install", "i", "add"}:
            args = []
            for arg in words[i + 2:]:
                if arg in {"&&", "||", ";", "|"}:
                    break
                args.append(arg)
            if not {"-g", "--global"} & set(args):
                continue
            for spec in (a for a in args if not a.startswith("-")):
                scoped = spec.startswith("@")
                name, at, ver = spec[1:].partition("@") if scoped else spec.partition("@")
                found.append(("npm", ("@" if scoped else "") + name, ver if at else None, set()))
    return found


def _sbom_tool_pin_findings(text: str) -> list[str]:
    """What is wrong with how this sbom.yml pins the tools it installs; an
    empty list is a workflow whose every tool install, and the cargo binary's
    cache key, names an exact version from the job env."""
    wf = gs._yaml().safe_load(text) or {}
    findings: list[str] = []
    installed = {tool: 0 for tool in SBOM_TOOLS}
    cached: set[str] = set()
    for job_name, job in (wf.get("jobs") or {}).items():
        # what the install lines and the cache key read: the workflow's env
        # under the job's; a step that sets a pin of its own is refused below
        job_env = {k: str(v) for k, v in {**(wf.get("env") or {}),
                                           **((job or {}).get("env") or {})}.items()}
        for step in (job or {}).get("steps") or []:
            step = step or {}
            where = f"{job_name}: {step.get('name') or step.get('uses')}"
            for var in sorted(set(SBOM_TOOLS.values()) & set(step.get("env") or {})):
                findings.append(f"{where}: sets {var} for one step; the pin is the "
                                "job's, which the cache key reads too")
            run = str(step.get("run") or "").replace("\\\n", " ")  # join continued lines
            for line in run.splitlines():
                for manager, tool, token, flags in _installs(line):
                    var = SBOM_TOOLS.get(tool)
                    if var is None:
                        findings.append(f"{where}: {manager} installs {tool}, which "
                                        "SBOM_TOOLS does not pin")
                        continue
                    installed[tool] += 1
                    ref = _ENV_REF.fullmatch(token or "")
                    if not ref or (ref.group(1) or ref.group(2)) != var:
                        findings.append(f"{where}: {tool} is installed at {token!r}, "
                                        f"not at the job env's ${var}: {line.strip()}")
                    elif not _EXACT_VERSION.fullmatch(job_env.get(var, "")):
                        findings.append(f"{where}: the job env's {var} is "
                                        f"{job_env.get(var)!r}, not an exact x.y.z")
                    if manager == "cargo" and "--locked" not in flags:
                        findings.append(f"{where}: cargo install {tool} without "
                                        f"--locked: {line.strip()}")
            uses = str(step.get("uses") or "")
            if not uses.startswith("actions/cache@"):
                continue
            with_ = step.get("with") or {}
            paths = str(with_.get("path") or "").split()
            for tool, var in SBOM_TOOLS.items():
                if f"~/.cargo/bin/{tool}" not in paths:
                    continue
                cached.add(tool)
                key = _EXPR.sub(lambda m: "${{ " + m.group(1) + " }}",
                                str(with_.get("key") or ""))
                want = f"{tool}-${{{{ runner.os }}}}-${{{{ env.{var} }}}}"
                if key != want:
                    findings.append(f"{where}: the {tool} binary is cached under "
                                    f"{key!r}, not {want!r} (a key on anything "
                                    "but the version rebuilds the same release "
                                    "or restores another one)")
                if job_env.get(var) is None:
                    findings.append(f"{where}: this job's env does not set {var}, "
                                    f"the {tool} pin its cache key must read")
                if with_.get("restore-keys"):
                    findings.append(f"{where}: restore-keys on the {tool} binary "
                                    "would restore another version's build")
    for tool, count in installed.items():
        if not count:
            findings.append(f"no step installs {tool}")
    if "cargo-cyclonedx" not in cached:
        findings.append("no actions/cache step holds ~/.cargo/bin/cargo-cyclonedx")
    return findings


class SbomToolPins(unittest.TestCase):
    """sbom.yml's two tools are pinned exactly, from one env per tool. Until
    CI26 it ran `cargo install cargo-cyclonedx --locked` under a weekly cache
    key (crates.io latest, a new build each ISO week) and an unpinned
    `npm install -g @cyclonedx/cdxgen` (npm latest on every run), so which
    tool wrote a release's bill of materials depended on the day it ran."""

    def test_every_tool_install_and_the_binary_cache_carry_the_env_pin(self):
        self.assertEqual(_sbom_tool_pin_findings(SBOM_YML.read_text(encoding="utf-8")), [])

    def test_the_check_refuses_each_way_back_to_floating(self):
        text = SBOM_YML.read_text(encoding="utf-8")
        # built from today's pins, so a bump needs no edit in this test
        env = gs._yaml().safe_load(text)["jobs"]["generate-sbom"]["env"]
        cargo_pin = f'CARGO_CYCLONEDX_VERSION: "{env["CARGO_CYCLONEDX_VERSION"]}"'
        cdxgen_pin = f'CDXGEN_VERSION: "{env["CDXGEN_VERSION"]}"'
        week = "-${{ steps.week.outputs.stamp }}"
        key = "key: cargo-cyclonedx-${{ runner.os }}-${{ env.CARGO_CYCLONEDX_VERSION }}"
        for label, old, new in (
            ("no --version", ' --version "$CARGO_CYCLONEDX_VERSION"', ""),
            ("a typed version, not the env's", '--version "$CARGO_CYCLONEDX_VERSION"',
             f'--version {env["CARGO_CYCLONEDX_VERSION"]}'),
            ("no --locked", '"$CARGO_CYCLONEDX_VERSION" --locked', '"$CARGO_CYCLONEDX_VERSION"'),
            ("no @version on cdxgen", '"@cyclonedx/cdxgen@$CDXGEN_VERSION"', "@cyclonedx/cdxgen"),
            ("a dist-tag", cdxgen_pin, 'CDXGEN_VERSION: "latest"'),
            ("a caret range", cargo_pin, 'CARGO_CYCLONEDX_VERSION: "^0.5"'),
            ("a partial version", cdxgen_pin, 'CDXGEN_VERSION: "12"'),
            ("the week stamp back in the key", key, key + week),
            ("a key without the version", key, "key: cargo-cyclonedx-${{ runner.os }}" + week),
            ("a restore-keys prefix", key, key + "\n        restore-keys: cargo-cyclonedx-"),
            ("a step's own pin", "    - name: Install cdxgen\n",
             "    - name: Install cdxgen\n      env:\n        CDXGEN_VERSION: latest\n"),
        ):
            with self.subTest(label):
                self.assertEqual(text.count(old), 1, f"mutant {label!r} no longer applies")
                self.assertNotEqual(_sbom_tool_pin_findings(text.replace(old, new)), [])
        # and a third tool installed unpinned is not waved through
        extra = text.replace('npm install -g "@cyclonedx/cdxgen@$CDXGEN_VERSION"',
                             'npm install -g "@cyclonedx/cdxgen@$CDXGEN_VERSION" @cyclonedx/cyclonedx-cli')
        self.assertNotEqual(extra, text)
        self.assertNotEqual(_sbom_tool_pin_findings(extra), [])

    def test_the_readme_names_the_pinned_versions(self):
        job_env = gs._yaml().safe_load(SBOM_YML.read_text(encoding="utf-8"))[
            "jobs"]["generate-sbom"]["env"]
        readme = (REPO / "sbom" / "README.md").read_text(encoding="utf-8")
        for tool, var in SBOM_TOOLS.items():
            with self.subTest(tool):
                said = re.findall(rf"\| `{re.escape(tool)}` \| ([^ |]+) \|", readme)
                self.assertEqual(said, [str(job_env[var])], f"sbom/README.md's row for {tool}")
                self.assertIn(f"`{var}`", readme)


def _product_cores(doc: dict) -> dict[str, set[str]]:
    """product → Arduino cores of its PlatformIO platforms, read off the
    committed graph (product → platform → framework:arduino-esp32@X)."""
    deps = {d["ref"]: d["dependsOn"] for d in doc["dependencies"]}
    cores: dict[str, set[str]] = {}
    for ref, targets in deps.items():
        if not ref.startswith("product:"):
            continue
        for plat in (t for t in targets if t.startswith("platform:")):
            for core in deps[plat]:
                cores.setdefault(ref[len("product:"):], set()).add(core.rsplit("@", 1)[1])
    return cores


class ArduinoCorePins(unittest.TestCase):
    """The two Arduino axes and the agreement asserted between them."""

    @classmethod
    def setUpClass(cls):
        cls.workflow = gs.arduino_core_pins()
        cls.sketch = gs.sketch_core_pins(gs.load_json(gs.FLAVORS))
        cls.cores = _product_cores(_doc())

    def test_sketch_profiles_are_read_from_yaml_not_grepped(self):
        by = {(p["sketch"], p["profile"]): p for p in self.sketch}
        self.assertEqual(by[("canary_display", "watch-core3")]["version"], "3.3.10")
        self.assertEqual(by[("canary_display", "watch")]["libraries"]["GFX Library for Arduino"],
                         "1.4.9")
        self.assertEqual(by[("canary_wap", "xiao_sense")]["product"], "canary-wap")
        self.assertIsNone(by[("canary_wap", "xiao_sense")]["libraries"]["NimBLE-Arduino"])
        # the display file's comments name 2.0.17 too; only two profiles pin it
        self.assertEqual(sum(p["version"] == "2.0.17" for p in self.sketch
                             if p["sketch"] == "canary_display"), 2)

    def test_workflow_rows_carry_their_library_pins_and_the_sketch_they_compile(self):
        rows = {(w["where"], w["version"]): w for w in self.workflow}
        display = rows[("firmware.yml:build-arduino-cli-display", "3.3.10")]
        self.assertEqual(display["libraries"]["lvgl"], "9.5.0")    # ${{ matrix.core.lvgl }}
        self.assertEqual(display["products"], ["canary-display"])
        wap = rows[("firmware.yml:build-arduino-cli", None)]
        self.assertEqual(wap["products"], ["canary-wap"])
        self.assertIsNone(wap["libraries"]["NimBLE-Arduino"])
        # one release job installs the WAP's pinned core and both display cores
        release = [w for w in self.workflow if w["where"] == "firmware-release.yml:build-sign-release"]
        self.assertEqual(sorted((w["version"] or "latest", tuple(w["products"])) for w in release),
                         [("2.0.17", ("canary-display",)), ("3.3.10", ("canary-display",)),
                          ("3.3.8", ("canary-wap",))])

    def test_the_agreement_holds_on_the_tree(self):
        self.assertEqual(gs.check_core_pin_agreement(self.workflow, self.sketch, self.cores), [])

    def test_a_workflow_pin_no_profile_carries_is_a_finding(self):
        rows = self.workflow + [{"version": "3.3.11", "where": "x.yml:job",
                                 "libraries": {}, "products": ["canary-wap"]}]
        findings = gs.check_core_pin_agreement(rows, self.sketch, self.cores)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("x.yml:job pins esp32:esp32 3.3.11", findings[0])

    def _wap_pinned_rows(self):
        return [w for w in self.workflow if w["version"] and "canary-wap" in w["products"]]

    def test_a_sketch_pin_neither_axis_knows_is_a_finding(self):
        doctored = [dict(p, version="3.3.9") if p["profile"] == "xiao_sense" else p
                    for p in self.sketch]
        findings = gs.check_core_pin_agreement(self.workflow, doctored, self.cores)
        # the profile itself, and each WAP row whose 3.3.8 it no longer answers
        rows = self._wap_pinned_rows()
        self.assertEqual(len(findings), 1 + len(rows), findings)
        profile = [f for f in findings if "xiao_sense" in f]
        self.assertEqual(len(profile), 1, findings)
        self.assertIn("3.3.9", profile[0])
        for w in rows:
            self.assertTrue(any(f.startswith(f"{w['where']} pins esp32:esp32 3.3.8 for canary-wap")
                                for f in findings), (w["where"], findings))

    def test_the_wap_release_rows_pin_the_sketch_core_and_the_pr_row_floats(self):
        # The release decision recorded in firmware/PLATFORMS.md (sweep F21,
        # #1802): the rows that make or measure a release pin the WAP
        # sketch's 3.3.8, and firmware.yml's PR row stays on the action's
        # "latest" as the early warning for the next core. A row moved either
        # way is a change to that decision, so it fails here.
        wap = {w["where"]: w["version"] for w in self.workflow if "canary-wap" in w["products"]}
        self.assertEqual(wap, {
            "csi_module_disable_matrix.yml:build-with-module-disabled": "3.3.8",
            "firmware-release.yml:build-sign-release": "3.3.8",
            "firmware.yml:build-arduino-cli": None,
            "flasher-release.yml:factory-images": "3.3.8",
            "ram_audit.yml:ram-audit": "3.3.8",
        })
        # the pinned rows answer the sketch on their own, PlatformIO aside
        self.assertEqual(gs.check_core_pin_agreement(self.workflow, self.sketch,
                                                     {k: set() for k in self.cores}), [])

    def test_the_wap_sketch_may_track_the_platformio_core_when_its_rows_float(self):
        # Rule 2's allowance, held on the rows as they stood before #1802 (every
        # WAP row on the action's latest): 3.3.8 is then no workflow pin, only
        # platform_core3's core, and the sketch is legal through it alone.
        floating = [dict(w, version=None) if "canary-wap" in w["products"] else w
                    for w in self.workflow]
        self.assertNotIn("3.3.8", {w["version"] for w in floating})
        self.assertIn("3.3.8", self.cores["canary-wap"])
        self.assertEqual(gs.check_core_pin_agreement(floating, self.sketch, self.cores), [])
        findings = gs.check_core_pin_agreement(floating, self.sketch,
                                               {k: set() for k in self.cores})
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("xiao_sense", findings[0])
        # and the allowance is per product: the display's platform carrying
        # 3.3.8 does not excuse the WAP's sketch (today both carry it, so a
        # union of the map would pass unnoticed without this)
        findings = gs.check_core_pin_agreement(
            floating, self.sketch, {"canary-wap": set(), "canary-display": {"3.3.8"}})
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("xiao_sense", findings[0])
        self.assertEqual(gs.check_core_pin_agreement(
            floating, self.sketch, {"canary-wap": {"3.3.8"}, "canary-display": set()}), [])

    def test_a_row_pin_is_answered_only_by_the_profiles_of_the_product_it_builds(self):
        # review round on #1686: a WAP row moved to 3.3.10 must not stay green
        # on the display's 3.3.10 profile
        rows = self.workflow + [{"version": "3.3.10", "where": "x.yml:wap",
                                 "libraries": {}, "products": ["canary-wap"]}]
        findings = gs.check_core_pin_agreement(rows, self.sketch, self.cores)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("x.yml:wap pins esp32:esp32 3.3.10 for canary-wap", findings[0])
        self.assertIn("its profiles pin: 3.3.8", findings[0])
        # the same pin on a row that builds the display is answered by its profiles
        rows = self.workflow + [{"version": "3.3.10", "where": "x.yml:display",
                                 "libraries": {}, "products": ["canary-display"]}]
        self.assertEqual(gs.check_core_pin_agreement(rows, self.sketch, self.cores), [])

    def test_a_profile_pin_is_answered_only_by_rows_that_build_its_product(self):
        # a WAP profile on 2.0.17 — a display row's pin, never a WAP row's, and
        # not the WAP's PlatformIO core — is a finding, not a coincidence
        doctored = [dict(p, version="2.0.17") if p["profile"] == "xiao_sense" else p
                    for p in self.sketch]
        findings = gs.check_core_pin_agreement(self.workflow, doctored, self.cores)
        # the profile, and each WAP row whose 3.3.8 it no longer answers
        self.assertEqual(len(findings), 1 + len(self._wap_pinned_rows()), findings)
        profile = [f for f in findings if "xiao_sense" in f]
        self.assertEqual(len(profile), 1, findings)
        self.assertIn("builds canary-wap (3.3.8)", profile[0])

    def test_a_library_off_its_core_line_is_a_finding(self):
        # lvgl 8.4 on a core-3 profile: one finding per workflow row on 3.3.10
        # that builds the display (firmware, firmware-release, flasher-release)
        doctored = [dict(p, libraries=dict(p["libraries"], lvgl="8.4.0"))
                    if p["profile"] == "dash-core3" else p for p in self.sketch]
        findings = gs.check_core_pin_agreement(self.workflow, doctored, self.cores)
        self.assertEqual(len(findings), 3, findings)
        self.assertTrue(all("dash-core3" in f and "lvgl 8.4.0" in f and "lvgl@9.5.0" in f
                            for f in findings), findings)
        # and a pin the sketch dropped is "unpinned", not silently equal
        doctored = [dict(p, libraries={k: v for k, v in p["libraries"].items()
                                       if k != "NimBLE-Arduino"})
                    if p["profile"] == "watch" else p for p in self.sketch]
        findings = gs.check_core_pin_agreement(self.workflow, doctored, self.cores)
        self.assertEqual(len(findings), 3, findings)
        self.assertIn("NimBLE-Arduino unpinned", findings[0])

    def test_a_disagreement_fails_generation(self):
        doctored = [dict(p, version="3.3.9") if p["profile"] == "xiao_sense" else p
                    for p in self.sketch]
        with mock.patch.object(gs, "sketch_core_pins", return_value=doctored):
            with self.assertRaises(SystemExit) as cm:
                gs.build_document()
        self.assertIn("disagree", str(cm.exception))
        self.assertIn("xiao_sense", str(cm.exception))

    def test_a_sketch_yaml_with_no_profiles_is_refused(self):
        # A profile-less file would shrink the axis rule (a) is checked
        # against to nothing, silently; the reader refuses every other shape
        # it cannot spell, and this one too.
        flavors = [{"name": "x", "dir": "firmware/projects/x"}]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sketch = "firmware/projects/x/arduino/x/sketch.yaml"
            with mock.patch.object(gs, "REPO", root), \
                    mock.patch.object(gs, "PROJECTS", root / "firmware" / "projects"):
                for body in ("name: x\n", "name: x\nprofiles: {}\n", "name: x\nprofiles:\n"):
                    _write(root, sketch, body)
                    with self.assertRaises(SystemExit) as cm:
                        gs.sketch_core_pins(flavors)
                    self.assertIn("no profiles", str(cm.exception))
                    self.assertIn(sketch, str(cm.exception))
                _write(root, sketch, """
                    profiles:
                      a:
                        fqbn: esp32:esp32:esp32s3
                        platforms:
                          - platform: esp32:esp32 (3.3.10)
                        libraries:
                          - lvgl (9.5.0)
                          - ArduinoJson
                """)
                pins = gs.sketch_core_pins(flavors)
        self.assertEqual([(p["product"], p["profile"], p["version"], p["libraries"])
                          for p in pins],
                         [("x", "a", "3.3.10", {"lvgl": "9.5.0", "ArduinoJson": None})])

    def test_sketch_profiles_are_build_paths_of_their_core(self):
        paths = {c["bom-ref"]: next(p["value"] for p in c["properties"]
                                    if p["name"] == "securacv:build_paths")
                 for c in _doc()["components"]
                 if c["bom-ref"].startswith("framework:arduino-esp32@")}
        self.assertIn("sketch.yaml (canary_wap/xiao_sense)", paths["framework:arduino-esp32@3.3.8"])
        self.assertIn("sketch.yaml (canary_display/watch-core3)",
                      paths["framework:arduino-esp32@3.3.10"])
        self.assertIn("sketch.yaml (canary_display/watch)", paths["framework:arduino-esp32@2.0.17"])
        self.assertNotIn("sketch.yaml", paths["framework:arduino-esp32@latest"])


if __name__ == "__main__":
    sys.exit(unittest.main())
