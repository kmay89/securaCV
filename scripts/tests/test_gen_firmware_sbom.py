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
    on the pioarduino core-3 line (Arduino 3.3.11 / ESP-IDF 5.5.5), which the
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
  • sbom.yml's own two tools are pinned exactly (CI26): every command that
    installs or fetches a package by name (cargo install, npm install, npx,
    npm exec, pnpm/yarn/bun add and dlx) names an x.y.z from the job env
    (cargo with --locked; cargo binstall is refused), every cache of the
    cargo-cyclonedx binary keys on that version first and nothing that
    rotates, the check refuses each listed way back to floating and its
    guards an install or a cache it cannot read, the step that reads each
    pin back off the documents is run on both tools' shapes, and the
    README's tool rows say the same versions;
  • the two Arduino axes: sketch.yaml profiles are read from YAML (the
    files' comments quote core numbers too), workflow rows carry their
    matrix-resolved library pins and the sketch they compile, the agreement
    check is green on the tree and red on a workflow pin no profile carries,
    a sketch pin neither axis knows, a library off its core line (and the
    PlatformIO-core allowance is per product) — a finding fails generation;
    a sketch.yaml with no profiles is refused;
  • release rows name exact library versions (A2-19): every library a
    firmware-release.yml / flasher-release.yml row installs carries one
    exact version (a bare name was Library Manager's newest on the day the
    image was signed), an unversioned one fails generation, a PR row may
    float, the WAP rows equal the WAP profile, the release rows' libraries
    are `arduino-lib:` components of the products they build, and the
    PlatformIO lib_deps under firmware/envs/platformio are exact pins;
  • the firmware/third_party copies (GHSA-8cmm-3887-r32j's patched core-2
    WebServer): one `vendored:` component each, with its manifest's
    version, a pedigree naming the upstream library and the advisory the
    backport resolves; exactly the products whose build envs list the
    storage, or whose Arduino-CLI rows pass it with --library, depend on it,
    and each still reaches the core it came from (where the advisory check
    compares upstream's advisories); a directory with no facts, facts with
    no directory, a version not naming the core, a link on another core, a
    row naming an unknown directory and a copy nothing links each fail
    generation; `--libraries`, comment lines and a matrix leg without the
    key name nothing.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import fnmatch
import importlib.util
import io
import json
import os
import re
import shlex
import shutil
import subprocess
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
        self.assertEqual(deps["platform:platform_core3"], {"framework:arduino-esp32@3.3.11"})
        self.assertEqual(deps["framework:arduino-esp32@3.3.11"], {"framework:esp-idf@5.5.5"})
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
            copy.write_text(copy.read_text(encoding="utf-8").replace('"3.3.11"', '"3.3.13"'),
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
# the job env variable that pins it. Any other package a run line installs or
# runs by name (the forms _installs reads: cargo install, npm install/i/add,
# npx, npm exec, pnpm/yarn/bun add, their dlx/x) fails the test below until it
# is pinned the same way and named here, and cargo binstall is refused outright.
SBOM_TOOLS = {"cargo-cyclonedx": "CARGO_CYCLONEDX_VERSION",
              "@cyclonedx/cdxgen": "CDXGEN_VERSION"}
# The built binaries the job caches, and where cargo puts them.
SBOM_CACHED = {"cargo-cyclonedx": "~/.cargo/bin/cargo-cyclonedx"}
# The step after the gate that reads each pin back off the documents.
SBOM_TOOL_STEP = "Check each document names the pinned tool that wrote it"
# A bare x.y.z. To `cargo install --version` that is exactly one release (an
# operator makes it a requirement cargo resolves, and cargo refuses a partial
# version outright); to npm it is one version, where a partial one is a range
# and a word is a dist-tag.
_EXACT_VERSION = re.compile(r"\d+\.\d+\.\d+")
_ENV_REF = re.compile(r"\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*))")
_EXPR = re.compile(r"\$\{\{\s*(.*?)\s*\}\}")
# in a run line, `${{ env.NAME }}` is the job env's value, as `$NAME` is
_ENV_EXPR = re.compile(r"\$\{\{\s*env\.([A-Za-z_][A-Za-z0-9_]*)\s*\}\}")
_SHELL_OPS = set("();<>|&")
# cargo options that take a value (the value is not the crate name), the
# global ones before the subcommand included
_CARGO_VALUED = {"--version", "--vers", "--root", "--target", "--target-dir",
                 "--features", "-F", "--git", "--branch", "--tag", "--rev",
                 "--path", "--registry", "--index", "--profile", "-j", "--jobs",
                 "--config", "-Z", "-C", "--bin", "--example", "--color"}
# npm, pnpm, yarn and bun options that take a value (the value is not a
# package; `-p`/`--package` names one, read on its own)
_NPM_VALUED = {"--prefix", "--registry", "--location", "--cache", "--userconfig",
               "--globalconfig", "--tag", "--before", "--omit", "--include",
               "--install-strategy", "--loglevel", "-w", "--workspace", "-C",
               "--dir", "--cwd", "--filter", "-c", "--call", "--shell"}
_NPM_PACKAGE = {"-p", "--package"}
# The commands that fetch an npm package by name. "install": every positional
# is a package. "run": the first positional (or each --package) is fetched and
# run, and the rest of the line is the tool's own arguments.
_NPM_LIKE = {("npm", "install"): "install", ("npm", "i"): "install",
             ("npm", "in"): "install", ("npm", "add"): "install",
             ("npm", "install-test"): "install", ("npm", "it"): "install",
             ("pnpm", "add"): "install", ("yarn", "add"): "install",
             ("yarn", "global", "add"): "install", ("bun", "add"): "install",
             ("npx",): "run", ("npm", "exec"): "run", ("npm", "x"): "run",
             ("pnpm", "dlx"): "run", ("yarn", "dlx"): "run", ("bunx",): "run",
             ("bun", "x"): "run"}


def _commands(line: str) -> list[list[str]]:
    """The simple commands on one shell line, each as its words: split at the
    control operators, with a redirection and its target dropped, a command
    substitution read as a command of its own, and `${{ env.NAME }}` read as
    the `${NAME}` the job env exports (any other expression stays one word)."""
    line = _ENV_EXPR.sub(r"${\1}", line)
    line = _EXPR.sub(lambda m: "${{" + "".join(m.group(1).split()) + "}}", line)
    lex = shlex.shlex(line, posix=True, punctuation_chars=True)
    lex.whitespace_split = True
    try:
        tokens = list(lex)
    except ValueError:  # a quote this line opens and a later one closes
        tokens = [w.strip("'\"") for w in re.findall(r"[();<>|&]+|[^\s();<>|&]+", line)]
    commands: list[list[str]] = []
    words: list[str] = []
    drop_target = False
    for tok in tokens:
        if drop_target:
            drop_target = False
            continue
        if tok and set(tok) <= _SHELL_OPS:
            if ("<" in tok or ">" in tok) and not ("(" in tok or ")" in tok):
                if words and words[-1].isdigit():  # the fd in 2>&1
                    words.pop()
                drop_target = True
                continue
            if words:
                commands.append(words)
            words = []
            continue
        for sub in re.finditer(r"\$\(|`", tok):  # "$(npm i x)" is one quoted word
            commands += _commands(tok[sub.end():])
        words.append(tok.strip("`"))
    if words:
        commands.append(words)
    return commands


def _npm_spec(spec: str) -> tuple[str, str | None]:
    """(package, version or None) of `name`, `name@version`, `@scope/name@version`."""
    scoped = spec.startswith("@")
    name, at, ver = (spec[1:] if scoped else spec).partition("@")
    return ("@" if scoped else "") + name, ver if at else None


def _npm_specs(args: list[str], mode: str) -> list[str]:
    """The package specs an npm-like command (see _NPM_LIKE) fetches."""
    named: list[str] = []
    specs: list[str] = []
    j = 0
    while j < len(args):
        arg = args[j]
        if arg in _NPM_PACKAGE:
            named += args[j + 1:j + 2]
            j += 2
        elif arg.startswith("--package="):
            named.append(arg.split("=", 1)[1])
            j += 1
        elif arg in _NPM_VALUED:
            j += 2
        elif arg.startswith("-"):  # a flag, `--location=global` and `--` among them
            j += 1
        elif mode == "run":
            return named or [arg]
        else:
            specs.append(arg)
            j += 1
    return named + specs


def _cargo_installs(args: list[str], flags: set[str]) -> list[tuple[str, str, str | None, set[str]]]:
    """The crates a `cargo install` with these arguments installs."""
    version, crates, j = None, [], 0
    while j < len(args):
        arg = args[j]
        if arg in _CARGO_VALUED:
            if arg in {"--version", "--vers"} and j + 1 < len(args):
                version = args[j + 1]
            j += 2
            continue
        if arg.startswith(("--version=", "--vers=")):
            version = arg.split("=", 1)[1]
        elif arg.startswith("-"):
            flags = flags | {arg}
        else:
            crates.append(arg)
        j += 1
    out = []
    for crate in crates:
        name, at, ver = crate.partition("@")
        out.append(("cargo install", name, ver if at else version, flags))
    return out


def _installs(line: str) -> list[tuple[str, str, str | None, set[str]]]:
    """(command, package, version token or None, flags) for each package a
    command on this shell line installs or fetches by name. A cargo binstall
    comes back as ("cargo binstall", "", None, set()), refused whatever it names."""
    found: list[tuple[str, str, str | None, set[str]]] = []
    for words in _commands(line):
        for i, word in enumerate(words):
            if word == "cargo":
                j, flags = i + 1, set()
                while j < len(words) and words[j].startswith(("+", "-")):  # +nightly, --locked
                    flags.add(words[j])
                    j += 2 if words[j] in _CARGO_VALUED else 1
                sub = words[j] if j < len(words) else ""
                if sub == "install":
                    found += _cargo_installs(words[j + 1:], flags)
                elif sub == "binstall":
                    found.append(("cargo binstall", "", None, set()))
                continue
            if word == "cargo-binstall":
                found.append(("cargo binstall", "", None, set()))
                continue
            for command, mode in _NPM_LIKE.items():
                if tuple(words[i:i + len(command)]) == command:
                    for spec in _npm_specs(words[i + len(command):], mode):
                        found.append((" ".join(command), *_npm_spec(spec), set()))
    return found


def _cache_holds(path: str, target: str) -> bool:
    """Whether an actions/cache `path` value holds target: the path itself, a
    directory above it, or a glob that matches either, unless a `!` pattern
    excludes it the same way. $HOME and the runner's home read as ~."""
    def norm(p: str) -> str:
        p = re.sub(r"^(?:\$HOME|\$\{HOME\}|\$\{\{\s*env\.HOME\s*\}\}|/home/runner)(?=/|$)", "~", p)
        return p.rstrip("/") or "/"
    parts = target.split("/")
    above = ["/".join(parts[:k]) for k in range(1, len(parts) + 1)]
    held = excluded = False
    for raw in (p.strip() for p in path.splitlines()):
        if not raw:
            continue
        pattern = norm(raw.lstrip("!"))
        if any(fnmatch.fnmatchcase(p, pattern) for p in above):
            if raw.startswith("!"):
                excluded = True
            else:
                held = True
    return held and not excluded


def _sbom_tool_pin_findings(text: str) -> list[str]:
    """What is wrong with how this sbom.yml pins the tools it installs; an
    empty list is a workflow whose every tool install, and every cache of the
    cargo binary, names an exact version from the job env."""
    wf = gs._yaml().safe_load(text) or {}
    findings: list[str] = []
    installed = {tool: 0 for tool in SBOM_TOOLS}
    cached: set[str] = set()
    for job_name, job in (wf.get("jobs") or {}).items():
        # what the install lines and the cache key read: the workflow's env
        # under the job's; a step or a run line that sets a pin is refused below
        job_env = {k: str(v) for k, v in {**(wf.get("env") or {}),
                                           **((job or {}).get("env") or {})}.items()}
        for step in (job or {}).get("steps") or []:
            step = step or {}
            where = f"{job_name}: {step.get('name') or step.get('uses')}"
            for var in sorted(set(SBOM_TOOLS.values()) & set(step.get("env") or {})):
                findings.append(f"{where}: sets {var} for one step; the pin is the "
                                "job's, which the cache key reads too")
            run = str(step.get("run") or "").replace("\\\n", " ")  # join continued lines
            for var in sorted(SBOM_TOOLS.values()):
                # VAR=, export VAR=, "VAR=..." >> "$GITHUB_ENV", unset VAR
                if re.search(rf"(?<![\w${{]){var}=|\bunset\b[^\n;&|]*\b{var}\b", run):
                    findings.append(f"{where}: changes {var} in a run line, past the "
                                    "job env the cache key reads")
            for line in run.splitlines():
                for command, tool, token, flags in _installs(line):
                    if command == "cargo binstall":
                        findings.append(f"{where}: cargo binstall fetches a prebuilt "
                                        "binary; sbom.yml builds its tools at the pin "
                                        f"with cargo install --locked: {line.strip()}")
                        continue
                    var = SBOM_TOOLS.get(tool)
                    if var is None:
                        findings.append(f"{where}: {command} fetches {tool}, which "
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
                    if command == "cargo install" and "--locked" not in flags:
                        findings.append(f"{where}: cargo install {tool} without "
                                        f"--locked: {line.strip()}")
            if not re.match(r"actions/cache(?:/restore|/save)?@", str(step.get("uses") or "")):
                continue
            with_ = step.get("with") or {}
            for tool, binary in SBOM_CACHED.items():
                if not _cache_holds(str(with_.get("path") or ""), binary):
                    continue
                var = SBOM_TOOLS[tool]
                cached.add(tool)
                key = _EXPR.sub(lambda m: "${{ " + m.group(1) + " }}",
                                str(with_.get("key") or ""))
                want = f"{tool}-${{{{ env.{var} }}}}-${{{{ runner.os }}}}"
                if key != want:
                    findings.append(f"{where}: {binary} is cached under {key!r}, not "
                                    f"{want!r} (a key on anything but the version "
                                    "rebuilds the same release or restores another "
                                    "one, and a key that ends in the version is a "
                                    "prefix of a longer version's)")
                if job_env.get(var) is None:
                    findings.append(f"{where}: this job's env does not set {var}, "
                                    f"the {tool} pin its cache key must read")
                if with_.get("restore-keys"):
                    findings.append(f"{where}: restore-keys on {binary} would "
                                    "restore another version's build")
    for tool, count in installed.items():
        if not count:
            findings.append(f"no step installs {tool}")
    for tool, binary in SBOM_CACHED.items():
        if tool not in cached:
            findings.append(f"no actions/cache step holds {binary}")
    return findings


def _mutate(text: str, edits) -> str:
    """text with each (old, new) applied; each old must occur exactly once."""
    for old, new in edits:
        if text.count(old) != 1:
            raise AssertionError(f"{old!r} occurs {text.count(old)} times, not once")
        text = text.replace(old, new)
    return text


class SbomToolPins(unittest.TestCase):
    """sbom.yml's two tools are pinned exactly, from one env per tool. Until
    CI26 it ran `cargo install cargo-cyclonedx --locked` under a weekly cache
    key (crates.io latest, a new build each ISO week) and an unpinned
    `npm install -g @cyclonedx/cdxgen` (npm latest on every run), so which
    tool wrote a release's bill of materials depended on the day it ran."""

    def setUp(self):
        self.text = SBOM_YML.read_text(encoding="utf-8")
        self.job = gs._yaml().safe_load(self.text)["jobs"]["generate-sbom"]

    def test_every_tool_install_and_the_binary_cache_carry_the_env_pin(self):
        self.assertEqual(_sbom_tool_pin_findings(self.text), [])

    def test_the_check_refuses_each_listed_way_back_to_floating(self):
        # built from today's pins, so a bump needs no edit in this test
        env = self.job["env"]
        cargo_pin = f'CARGO_CYCLONEDX_VERSION: "{env["CARGO_CYCLONEDX_VERSION"]}"'
        cdxgen_pin = f'CDXGEN_VERSION: "{env["CDXGEN_VERSION"]}"'
        cargo_install = 'run: cargo install cargo-cyclonedx --version "$CARGO_CYCLONEDX_VERSION" --locked'
        cdxgen_install = 'run: npm install -g --ignore-scripts "@cyclonedx/cdxgen@$CDXGEN_VERSION"'
        node_doc = "run: cdxgen -t node -o sbom-node.cdx.json canary-vision/"
        node_args = " -t node -o sbom-node.cdx.json canary-vision/"
        path = "path: ~/.cargo/bin/cargo-cyclonedx"
        key = "key: cargo-cyclonedx-${{ env.CARGO_CYCLONEDX_VERSION }}-${{ runner.os }}"
        week = "key: cargo-cyclonedx-${{ runner.os }}-${{ steps.week.outputs.stamp }}"
        second_cache = ("    - name: Install cargo-cyclonedx\n",
                        "    - uses: actions/cache@v6\n      with:\n"
                        "        path: @PATH@\n        " + week + "\n"
                        "        restore-keys: cargo-cyclonedx-\n"
                        "    - name: Install cargo-cyclonedx\n")
        latest = "@cyclonedx/cdxgen is installed at 'latest'"
        for label, edits, expected in (
            # the install lines
            ("no --version", [(' --version "$CARGO_CYCLONEDX_VERSION"', "")],
             "cargo-cyclonedx is installed at None"),
            ("a typed version, not the env's",
             [('--version "$CARGO_CYCLONEDX_VERSION"', f'--version {env["CARGO_CYCLONEDX_VERSION"]}')],
             f"cargo-cyclonedx is installed at '{env['CARGO_CYCLONEDX_VERSION']}'"),
            ("no --locked", [('"$CARGO_CYCLONEDX_VERSION" --locked', '"$CARGO_CYCLONEDX_VERSION"')],
             "without --locked"),
            ("no @version on cdxgen", [('"@cyclonedx/cdxgen@$CDXGEN_VERSION"', "@cyclonedx/cdxgen")],
             "@cyclonedx/cdxgen is installed at None"),
            ("a second install, global by --location=",
             [(cdxgen_install, cdxgen_install + " && npm i --location=global @cyclonedx/cdxgen@latest")],
             latest),
            ("a second install, global by --location",
             [(cdxgen_install, cdxgen_install + " && npm i --location global @cyclonedx/cdxgen@latest")],
             latest),
            ("npx at a dist-tag", [(node_doc, "run: npx --yes @cyclonedx/cdxgen@latest" + node_args)],
             latest),
            ("npm exec at a dist-tag",
             [(node_doc, "run: npm exec --yes -- @cyclonedx/cdxgen@latest" + node_args)], latest),
            ("pnpm dlx, unpinned", [(node_doc, "run: pnpm dlx @cyclonedx/cdxgen" + node_args)],
             "@cyclonedx/cdxgen is installed at None"),
            ("inside a command substitution",
             [(cdxgen_install, 'run: echo "$(npm install -g @cyclonedx/cdxgen@latest)"')], latest),
            ("cargo binstall after the pinned build",
             [(cargo_install, cargo_install + " && cargo binstall -y --force cargo-cyclonedx")],
             "cargo binstall fetches a prebuilt binary"),
            ("a third tool, unpinned",
             [(cdxgen_install, cdxgen_install + " @cyclonedx/cyclonedx-cli")],
             "fetches @cyclonedx/cyclonedx-cli, which SBOM_TOOLS does not pin"),
            # the pins themselves
            ("a dist-tag", [(cdxgen_pin, 'CDXGEN_VERSION: "latest"')],
             "CDXGEN_VERSION is 'latest', not an exact x.y.z"),
            ("a caret range", [(cargo_pin, 'CARGO_CYCLONEDX_VERSION: "^0.5"')],
             "CARGO_CYCLONEDX_VERSION is '^0.5', not an exact x.y.z"),
            ("a partial version", [(cdxgen_pin, 'CDXGEN_VERSION: "12"')],
             "CDXGEN_VERSION is '12', not an exact x.y.z"),
            ("the pin gone from the job env", [(cargo_pin, "")],
             "this job's env does not set CARGO_CYCLONEDX_VERSION"),
            ("a step's own pin", [("    - name: Install cdxgen\n",
                                   "    - name: Install cdxgen\n      env:\n        CDXGEN_VERSION: latest\n")],
             "sets CDXGEN_VERSION for one step"),
            ("a pin rewritten through $GITHUB_ENV",
             [("run: cd canary-vision && npm ci\n",
               'run: cd canary-vision && npm ci && echo "CDXGEN_VERSION=latest" >> "$GITHUB_ENV"\n')],
             "changes CDXGEN_VERSION in a run line"),
            ("a pin unset", [(cargo_install, "run: unset CARGO_CYCLONEDX_VERSION; " + cargo_install[5:])],
             "changes CARGO_CYCLONEDX_VERSION in a run line"),
            # the guards behind the parser: an install or a cache it cannot read
            ("cdxgen from a command the parser does not read",
             [(cdxgen_install, "run: volta install @cyclonedx/cdxgen")],
             "no step installs @cyclonedx/cdxgen"),
            ("cargo-cyclonedx from a command the parser does not read",
             [(cargo_install, "run: cargo quickinstall cargo-cyclonedx")],
             "no step installs cargo-cyclonedx"),
            ("a cache of something else", [(path, "path: ~/.cargo/registry")],
             "no actions/cache step holds ~/.cargo/bin/cargo-cyclonedx"),
            ("a cache that excludes the binary",
             [(path, "path: |\n          ~/.cargo/bin\n          !~/.cargo/bin/cargo-cyclonedx")],
             "no actions/cache step holds ~/.cargo/bin/cargo-cyclonedx"),
            # the cache key
            ("the week stamp back in the key", [(key, key + "-${{ steps.week.outputs.stamp }}")],
             "is cached under"),
            ("a key without the version", [(key, week)], "is cached under"),
            ("the version after the OS",
             [(key, "key: cargo-cyclonedx-${{ runner.os }}-${{ env.CARGO_CYCLONEDX_VERSION }}")],
             "is cached under"),
            ("a restore-keys prefix", [(key, key + "\n        restore-keys: cargo-cyclonedx-")],
             "restore-keys on ~/.cargo/bin/cargo-cyclonedx"),
            ("the directory above the binary, on a weekly key", [(path, "path: ~/.cargo/bin"), (key, week)],
             "is cached under"),
            ("a second cache over ~/.cargo/bin", [(second_cache[0], second_cache[1].replace("@PATH@", "~/.cargo/bin"))],
             "restore-keys on ~/.cargo/bin/cargo-cyclonedx"),
            ("a second cache over a glob of $HOME/.cargo",
             [(second_cache[0], second_cache[1].replace("@PATH@", "$HOME/.cargo/*"))], "is cached under"),
        ):
            with self.subTest(label):
                findings = _sbom_tool_pin_findings(_mutate(self.text, edits))
                self.assertTrue(any(expected in f for f in findings),
                                f"no finding says {expected!r}: {findings}")

    def test_the_check_reads_a_pin_spelled_another_way_as_the_same_pin(self):
        cargo_install = 'cargo install cargo-cyclonedx --version "$CARGO_CYCLONEDX_VERSION" --locked'
        cdxgen_install = 'npm install -g --ignore-scripts "@cyclonedx/cdxgen@$CDXGEN_VERSION"'
        for label, edits in (
            ("${{ env.VAR }}", [('"$CARGO_CYCLONEDX_VERSION" --locked',
                                 '"${{ env.CARGO_CYCLONEDX_VERSION }}" --locked')]),
            ("crate@version", [(cargo_install,
                                'cargo install cargo-cyclonedx@"$CARGO_CYCLONEDX_VERSION" --locked')]),
            ("a valued npm flag", [(cdxgen_install, cdxgen_install.replace(
                "-g ", '-g --prefix "$HOME/.npm-global" '))]),
            ("a redirection", [(cdxgen_install, cdxgen_install + " 2>&1 | tee cdxgen-install.log")]),
            ("npx at the pin", [("run: cdxgen -t node", 'run: npx --yes "@cyclonedx/cdxgen@$CDXGEN_VERSION" -t node')]),
        ):
            with self.subTest(label):
                self.assertEqual(_sbom_tool_pin_findings(_mutate(self.text, edits)), [])

    def test_the_tool_step_reads_each_pin_back_off_the_documents(self):
        """The step after the gate, run here with bash and jq on documents in
        each tool's own shape (cargo-cyclonedx's 1.3 tools array, cdxgen's 1.7
        tools.components): the pinned releases pass, and a document naming
        another release, another tool or none fails it."""
        self.assertIsNotNone(shutil.which("jq"), "jq (on the ubuntu runners) runs the step")
        steps = [s for s in self.job["steps"] if s.get("name") == SBOM_TOOL_STEP]
        self.assertEqual(len(steps), 1, f"sbom.yml has no single {SBOM_TOOL_STEP!r} step")
        self.assertEqual(steps[0].get("env"), {"APPS": "${{ steps.apps.outputs.list }}"})
        pins = {var: str(self.job["env"][var]) for var in SBOM_TOOLS.values()}

        def rust(version, name="cargo-cyclonedx", shape="array"):
            tool = {"vendor": "CycloneDX", "name": name, "version": version}
            tools = [tool] if shape == "array" else {"components": [tool]}
            return {"bomFormat": "CycloneDX", "specVersion": "1.3", "metadata": {"tools": tools}}

        def node(version):
            return {"bomFormat": "CycloneDX", "specVersion": "1.7", "metadata": {"tools": {
                "components": [{"group": "@cyclonedx", "name": "cdxgen", "version": version,
                                "type": "application"}], "services": []}}}

        cv, cd = pins["CARGO_CYCLONEDX_VERSION"], pins["CDXGEN_VERSION"]
        good = {"sbom-rust.cdx.json": rust(cv), "sbom-node.cdx.json": node(cd),
                "sbom-flasher-rust.cdx.json": rust(cv), "sbom-flasher-node.cdx.json": node(cd)}

        def run(docs, apps="flasher:desktop"):
            with tempfile.TemporaryDirectory() as tmp:
                for name, doc in docs.items():
                    (Path(tmp) / name).write_text(json.dumps(doc), encoding="utf-8")
                return subprocess.run(
                    ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", steps[0]["run"]],
                    cwd=tmp, env={**os.environ, **pins, "APPS": apps},
                    capture_output=True, text=True, timeout=60)

        for label, docs, apps in (
            ("the pinned releases", good, "flasher:desktop"),
            ("no app this run", {k: good[k] for k in ("sbom-rust.cdx.json", "sbom-node.cdx.json")}, ""),
            ("a Rust document with the 1.5 tools shape",
             {**good, "sbom-rust.cdx.json": rust(cv, shape="object")}, "flasher:desktop"),
        ):
            with self.subTest(label):
                done = run(docs, apps)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        for label, name, doc in (
            ("the kernel's, another cargo-cyclonedx", "sbom-rust.cdx.json", rust("0.0.1")),
            ("an app's, another cargo-cyclonedx", "sbom-flasher-rust.cdx.json", rust("0.0.1")),
            ("the vision's, another cdxgen", "sbom-node.cdx.json", node("0.0.1")),
            ("an app's, another cdxgen", "sbom-flasher-node.cdx.json", node("0.0.1")),
            ("another tool at the pin", "sbom-rust.cdx.json", rust(cv, name="cdxgen")),
            ("no tools", "sbom-node.cdx.json", {"bomFormat": "CycloneDX", "specVersion": "1.7"}),
        ):
            with self.subTest(label):
                done = run({**good, name: doc})
                self.assertNotEqual(done.returncode, 0, done.stdout)
                self.assertIn(f"::error::{name} does not name", done.stdout)

    def test_the_readme_names_the_pinned_versions(self):
        job_env = self.job["env"]
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
        self.assertEqual(by[("canary_display", "watch-core3")]["version"], "3.3.11")
        self.assertEqual(by[("canary_display", "watch")]["libraries"]["GFX Library for Arduino"],
                         "1.4.9")
        self.assertEqual(by[("canary_wap", "xiao_sense")]["product"], "canary-wap")
        self.assertEqual(by[("canary_wap", "xiao_sense")]["libraries"]["NimBLE-Arduino"],
                         "2.5.2")
        # the display file's comments name 2.0.17 too; only two profiles pin it
        self.assertEqual(sum(p["version"] == "2.0.17" for p in self.sketch
                             if p["sketch"] == "canary_display"), 2)

    def test_workflow_rows_carry_their_library_pins_and_the_sketch_they_compile(self):
        rows = {(w["where"], w["version"]): w for w in self.workflow}
        display = rows[("firmware.yml:build-arduino-cli-display", "3.3.11")]
        self.assertEqual(display["libraries"]["lvgl"], "9.5.0")    # ${{ matrix.core.lvgl }}
        self.assertEqual(display["products"], ["canary-display"])
        wap = rows[("firmware.yml:build-arduino-cli", None)]
        self.assertEqual(wap["products"], ["canary-wap"])
        self.assertIsNone(wap["libraries"]["NimBLE-Arduino"])
        # one release job installs the WAP's pinned core and both display
        # cores; the WAP and the display core-3 row pin one version
        release = [w for w in self.workflow if w["where"] == "firmware-release.yml:build-sign-release"]
        self.assertEqual(sorted((w["version"] or "latest", tuple(w["products"])) for w in release),
                         [("2.0.17", ("canary-display",)), ("3.3.11", ("canary-display",)),
                          ("3.3.11", ("canary-wap",))])

    def test_the_agreement_holds_on_the_tree(self):
        self.assertEqual(gs.check_core_pin_agreement(self.workflow, self.sketch, self.cores), [])

    def test_a_workflow_pin_no_profile_carries_is_a_finding(self):
        rows = self.workflow + [{"version": "3.3.10", "where": "x.yml:job",
                                 "libraries": {}, "products": ["canary-wap"]}]
        findings = gs.check_core_pin_agreement(rows, self.sketch, self.cores)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("x.yml:job pins esp32:esp32 3.3.10", findings[0])

    def _wap_pinned_rows(self):
        return [w for w in self.workflow if w["version"] and "canary-wap" in w["products"]]

    def test_a_sketch_pin_neither_axis_knows_is_a_finding(self):
        doctored = [dict(p, version="3.3.9") if p["profile"] == "xiao_sense" else p
                    for p in self.sketch]
        findings = gs.check_core_pin_agreement(self.workflow, doctored, self.cores)
        # the profile itself, and each WAP row whose 3.3.11 it no longer answers
        rows = self._wap_pinned_rows()
        self.assertEqual(len(findings), 1 + len(rows), findings)
        profile = [f for f in findings if "xiao_sense" in f]
        self.assertEqual(len(profile), 1, findings)
        self.assertIn("3.3.9", profile[0])
        for w in rows:
            self.assertTrue(any(f.startswith(f"{w['where']} pins esp32:esp32 3.3.11 for canary-wap")
                                for f in findings), (w["where"], findings))

    def test_the_wap_release_rows_pin_the_sketch_core_and_the_pr_row_floats(self):
        # The release decision recorded in firmware/PLATFORMS.md (sweep F21,
        # #1802): the rows that make or measure a release pin the WAP
        # sketch's 3.3.11, and firmware.yml's PR row stays on the action's
        # "latest" as the early warning for the next core. A row moved either
        # way is a change to that decision, so it fails here.
        wap = {w["where"]: w["version"] for w in self.workflow if "canary-wap" in w["products"]}
        self.assertEqual(wap, {
            "csi_module_disable_matrix.yml:build-with-module-disabled": "3.3.11",
            "firmware-release.yml:build-sign-release": "3.3.11",
            "firmware.yml:build-arduino-cli": None,
            "flasher-release.yml:factory-images": "3.3.11",
            "ram_audit.yml:ram-audit": "3.3.11",
        })
        # the pinned rows answer the sketch on their own, PlatformIO aside
        self.assertEqual(gs.check_core_pin_agreement(self.workflow, self.sketch,
                                                     {k: set() for k in self.cores}), [])

    def test_the_wap_sketch_may_track_the_platformio_core_when_its_rows_float(self):
        # Rule 2's allowance, held on the rows as they stood before #1802 (every
        # WAP row on the action's latest): 3.3.11 is then no WAP row's pin
        # (the display's core-3 rows pin it, which answers only display
        # profiles), only platform_core3's core, and the sketch is legal
        # through it alone.
        floating = [dict(w, version=None) if "canary-wap" in w["products"] else w
                    for w in self.workflow]
        self.assertNotIn("3.3.11", {w["version"] for w in floating
                                    if "canary-wap" in w["products"]})
        self.assertIn("3.3.11", {w["version"] for w in floating})
        self.assertIn("3.3.11", self.cores["canary-wap"])
        self.assertEqual(gs.check_core_pin_agreement(floating, self.sketch, self.cores), [])
        findings = gs.check_core_pin_agreement(floating, self.sketch,
                                               {k: set() for k in self.cores})
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("xiao_sense", findings[0])
        # and the allowance is per product: the display's platform carrying
        # 3.3.11 does not excuse the WAP's sketch (today both carry it, so a
        # union of the map would pass unnoticed without this)
        findings = gs.check_core_pin_agreement(
            floating, self.sketch, {"canary-wap": set(), "canary-display": {"3.3.11"}})
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("xiao_sense", findings[0])
        self.assertEqual(gs.check_core_pin_agreement(
            floating, self.sketch, {"canary-wap": {"3.3.11"}, "canary-display": set()}), [])

    def test_a_row_pin_is_answered_only_by_the_profiles_of_the_product_it_builds(self):
        # review round on #1686: a WAP row moved to a core only a display
        # profile pins must not stay green on that profile. The WAP and the
        # display core-3 profiles now share one core (3.3.11), so the display side gets
        # a profile (and a row to answer it) on a core of its own.
        sketch = self.sketch + [{"version": "3.3.10", "product": "canary-display",
                                 "sketch": "canary_display", "profile": "x",
                                 "where": "x/sketch.yaml (x)", "libraries": {}}]
        display_row = {"version": "3.3.10", "where": "x.yml:display",
                       "libraries": {}, "products": ["canary-display"]}
        self.assertEqual(gs.check_core_pin_agreement(self.workflow + [display_row], sketch,
                                                     self.cores), [])
        rows = self.workflow + [display_row, {"version": "3.3.10", "where": "x.yml:wap",
                                              "libraries": {}, "products": ["canary-wap"]}]
        findings = gs.check_core_pin_agreement(rows, sketch, self.cores)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("x.yml:wap pins esp32:esp32 3.3.10 for canary-wap", findings[0])
        self.assertIn("its profiles pin: 3.3.11", findings[0])

    def test_a_profile_pin_is_answered_only_by_rows_that_build_its_product(self):
        # a WAP profile on 2.0.17 — a display row's pin, never a WAP row's, and
        # not the WAP's PlatformIO core — is a finding, not a coincidence
        doctored = [dict(p, version="2.0.17") if p["profile"] == "xiao_sense" else p
                    for p in self.sketch]
        findings = gs.check_core_pin_agreement(self.workflow, doctored, self.cores)
        # the profile, and each WAP row whose 3.3.11 it no longer answers
        self.assertEqual(len(findings), 1 + len(self._wap_pinned_rows()), findings)
        profile = [f for f in findings if "xiao_sense" in f]
        self.assertEqual(len(profile), 1, findings)
        self.assertIn("builds canary-wap (3.3.11)", profile[0])

    def test_a_library_off_its_core_line_is_a_finding(self):
        # lvgl 8.4 on a core-3 profile: one finding per workflow row on 3.3.11
        # that builds the display (firmware, firmware-release, flasher-release;
        # the WAP rows on that core build the WAP, so they do not count)
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
        self.assertIn("sketch.yaml (canary_wap/xiao_sense)", paths["framework:arduino-esp32@3.3.11"])
        self.assertIn("sketch.yaml (canary_display/watch-core3)",
                      paths["framework:arduino-esp32@3.3.11"])
        self.assertIn("sketch.yaml (canary_display/watch)", paths["framework:arduino-esp32@2.0.17"])
        self.assertNotIn("sketch.yaml", paths["framework:arduino-esp32@latest"])


class ReleaseRowLibraryPins(unittest.TestCase):
    """A2-19: a signed or factory image links the library versions its row
    names, never whatever Library Manager or the PlatformIO registry has as
    newest on the day it is built."""

    @classmethod
    def setUpClass(cls):
        cls.workflow = gs.arduino_core_pins()
        cls.doc = _doc()

    def test_every_release_row_library_names_one_exact_version(self):
        release = [w for w in self.workflow if gs._is_release_row(w)]
        self.assertEqual({w["where"].split(":")[0] for w in release}, set(gs.RELEASE_WORKFLOWS))
        for w in release:
            for name, ver in w["libraries"].items():
                self.assertRegex(ver or "", r"^\d+(\.\d+)+$", (w["where"], w["version"], name))
        self.assertEqual(gs.check_release_rows_pinned(self.workflow), [])

    def test_the_wap_release_rows_install_what_the_wap_profile_pins(self):
        pins = {"Crypto": "0.4.0", "ArduinoJson": "7.4.3", "NimBLE-Arduino": "2.5.2"}
        wap = {w["where"]: w["libraries"] for w in self.workflow
               if w["version"] == "3.3.11" and "canary-wap" in w["products"]
               and w["where"].split(":")[0] in gs.RELEASE_WORKFLOWS + ("ram_audit.yml",)}
        self.assertEqual(sorted(wap), ["firmware-release.yml:build-sign-release",
                                       "flasher-release.yml:factory-images",
                                       "ram_audit.yml:ram-audit"])
        for where, libs in wap.items():
            self.assertEqual(libs, pins, where)
        profile = [p for p in gs.sketch_core_pins(gs.load_json(gs.FLAVORS))
                   if p["profile"] == "xiao_sense"]
        self.assertEqual([p["libraries"] for p in profile], [pins])

    def test_an_unversioned_library_on_a_release_row_is_refused_and_a_pr_row_may_float(self):
        for where in ("firmware-release.yml:build-sign-release",
                      "flasher-release.yml:factory-images"):
            for bad in (None, "^2.5.0"):
                rows = [dict(w, libraries=dict(w["libraries"], **{"NimBLE-Arduino": bad}))
                        if w["where"] == where and "canary-wap" in w["products"] else w
                        for w in self.workflow]
                findings = gs.check_release_rows_pinned(rows)
                self.assertEqual(len(findings), 1, findings)
                self.assertIn(f"{where} (core 3.3.11) installs NimBLE-Arduino with no exact "
                              f"version", findings[0])
        # firmware.yml's PR row floats by design: it ships nothing
        pr = [w for w in self.workflow if w["where"] == "firmware.yml:build-arduino-cli"]
        self.assertTrue(pr and all(v is None for v in pr[0]["libraries"].values()), pr)
        self.assertEqual(gs.check_release_rows_pinned(pr), [])

    def test_an_unversioned_release_row_fails_generation(self):
        doctored = [dict(w, libraries=dict(w["libraries"], Crypto=None))
                    if gs._is_release_row(w) else w for w in self.workflow]
        with mock.patch.object(gs, "arduino_core_pins", return_value=doctored):
            with self.assertRaises(SystemExit) as cm:
                gs.build_document()
        self.assertIn("no exact version", str(cm.exception))
        self.assertIn("Crypto", str(cm.exception))

    def test_the_release_rows_libraries_are_components_of_the_products_they_build(self):
        comps = {c["bom-ref"]: c for c in self.doc["components"]}
        deps = {d["ref"]: set(d["dependsOn"]) for d in self.doc["dependencies"]}
        # the WAP rows and the display core-3 rows install one NimBLE 2.5.2
        self.assertIn("arduino-lib:NimBLE-Arduino@2.5.2", deps["product:canary-wap"])
        self.assertIn("arduino-lib:NimBLE-Arduino@2.5.2", deps["product:canary-display"])
        self.assertIn("arduino-lib:NimBLE-Arduino@1.4.3", deps["product:canary-display"])
        self.assertNotIn("arduino-lib:NimBLE-Arduino@1.4.3", deps["product:canary-wap"])
        self.assertFalse([r for r in comps if r.startswith("arduino-lib:NimBLE-Arduino@2.5.0")])
        arduino = [c for ref, c in comps.items() if ref.startswith("arduino-lib:")]
        self.assertTrue(arduino)
        for c in arduino:
            self.assertEqual(c["bom-ref"], f"arduino-lib:{c['name']}@{c['version']}")
            paths = {p["value"] for p in c["properties"] if p["name"] == "securacv:build_paths"}
            self.assertEqual(len(paths), 1, c["bom-ref"])
            for path in next(iter(paths)).split("; "):
                self.assertRegex(path, r"^arduino-cli \((firmware|flasher)-release\.yml:", path)

    def test_the_platformio_env_inis_pin_every_library_exactly(self):
        # Every lib_deps entry the inis under firmware/envs/platformio feed
        # is an exact pin, so an SBOM library used by an env of those
        # products has a version. firmware/canary/platformio.ini carries its
        # own lib_deps, pinned the same way, so no product's library is a
        # range: the set below stays empty and the test holds the whole tree.
        floating_products: set[str] = set()
        for c in self.doc["components"]:
            if not c["bom-ref"].startswith("lib:"):
                continue
            props = {p["name"]: p["value"] for p in c["properties"]}
            if props["securacv:exact_pin"] == "true":
                self.assertIn("version", c, c["bom-ref"])
                self.assertNotIn("securacv:resolved", props, c["bom-ref"])
                continue
            envs = props["securacv:envs"].split(",")
            self.assertEqual({e.split(":")[0] for e in envs} - floating_products, set(),
                             (c["bom-ref"], envs))
        for ini in sorted((REPO / "firmware/envs/platformio").glob("*.ini")):
            for section, opts in gs.parse_ini(ini).items():
                for spec in gs._lines(opts.get("lib_deps", "")):
                    if spec.startswith("${"):
                        continue
                    self.assertTrue(gs.parse_lib_spec(spec)["exact"], (ini.name, section, spec))


VENDORED_DIR = "WebServer-2.0.17-securacv"
VENDORED_REF = f"vendored:{VENDORED_DIR}"


class VendoredLibraries(unittest.TestCase):
    """firmware/third_party: a core-2 WebServer built from a patched copy
    (GHSA-8cmm-3887-r32j). The SBOM must say which products link it and what
    it was patched from, or a reader sees arduino-esp32 2.0.17 under those
    images and nothing about the fix."""

    @classmethod
    def setUpClass(cls):
        cls.doc = _doc()
        cls.comps = {c["bom-ref"]: c for c in cls.doc["components"]}
        cls.deps = {d["ref"]: set(d["dependsOn"]) for d in cls.doc["dependencies"]}
        cls.workflow = gs.arduino_core_pins()

    def _props(self, ref: str) -> dict[str, str]:
        return {p["name"]: p["value"] for p in self.comps[ref].get("properties", [])}

    def test_the_copy_is_a_component_with_its_manifest_version_and_pedigree(self):
        manifest = gs._properties(gs.THIRD_PARTY_DIR / VENDORED_DIR / "library.properties")
        c = self.comps[VENDORED_REF]
        self.assertEqual((c["group"], c["name"], c["version"]),
                         ("securacv", manifest["name"], manifest["version"]))
        self.assertEqual(c["version"], "2.0.17+securacv.1")
        ancestor, = c["pedigree"]["ancestors"]
        self.assertEqual(ancestor["purl"], "pkg:github/espressif/arduino-esp32@2.0.17#libraries/WebServer")
        self.assertEqual((ancestor["group"], ancestor["name"], ancestor["version"]),
                         ("espressif", "WebServer", "2.0.0"))
        patch, = c["pedigree"]["patches"]
        self.assertEqual(patch["type"], "backport")
        self.assertEqual([(i["type"], i["id"]) for i in patch["resolves"]],
                         [("security", "GHSA-8cmm-3887-r32j")])
        self.assertIn("not run on a device", c["pedigree"]["notes"])
        props = self._props(VENDORED_REF)
        self.assertEqual(props["securacv:first_party"], "false")
        self.assertTrue((REPO / props["securacv:provenance"]).is_file())
        self.assertNotIn("purl", c, "the copy is not upstream's package; its ancestor carries the purl")

    def test_exactly_the_products_whose_builds_link_it_depend_on_it(self):
        storage = gs.THIRD_PARTY_DIR.resolve()
        want_envs, want_products = set(), set()
        for product in gs.load_json(gs.FLAVORS):
            pdir = REPO / product["dir"]
            cfg = gs.PioConfig(pdir)
            for env in product.get("build_envs") or []:
                extra = {(pdir / e).resolve() for e in gs._lines(cfg.env(env).get("lib_extra_dirs", ""))}
                if storage in extra:
                    want_envs.add(f"{product['name']}:{env}")
                    want_products.add(product["name"])
        rows = [w for w in self.workflow if VENDORED_DIR in w["vendored"]]
        for w in rows:
            want_products.update(w["products"])
        self.assertEqual(want_products, {"canary-vision", "canary-display", "canary-sentinel"})
        self.assertEqual(set(self._props(VENDORED_REF)["securacv:envs"].split(",")), want_envs)
        self.assertIn("canary-sentinel:canary-sentinel-lite", want_envs)
        self.assertEqual({p for p, d in self.deps.items() if VENDORED_REF in d},
                         {f"product:{p}" for p in want_products})
        # Each product that links it still reaches the core it came from.
        for p in want_products:
            reach, todo = set(), [f"product:{p}"]
            while todo:
                ref = todo.pop()
                if ref not in reach:
                    reach.add(ref)
                    todo.extend(self.deps.get(ref, ()))
            self.assertIn("framework:arduino-esp32@2.0.17", reach, p)

    def test_the_core_2017_display_rows_pass_it_and_no_other_row_does(self):
        rows = {(w["where"], w["version"]): w["vendored"] for w in self.workflow
                if "canary-display" in w["products"]}
        for where in ("firmware-release.yml:build-sign-release", "flasher-release.yml:factory-images",
                      "firmware.yml:build-arduino-cli-display"):
            self.assertEqual(rows[(where, "2.0.17")], [VENDORED_DIR], where)
        for (where, version), vendored in rows.items():
            if version != "2.0.17":
                self.assertEqual(vendored, [], (where, version))
        self.assertFalse([w for w in self.workflow if w["vendored"] and w["version"] != "2.0.17"])
        self.assertEqual(self._props(VENDORED_REF)["securacv:build_paths"].split("; "),
                         ["arduino-cli (firmware-release.yml:build-sign-release)",
                          "arduino-cli (firmware.yml:build-arduino-cli-display)",
                          "arduino-cli (flasher-release.yml:factory-images)"])

    def test_only_a_library_flag_in_a_live_line_names_a_copy(self):
        entry = {"version": "2.0.17", "webserver": "firmware/third_party/X"}
        resolve = lambda t: gs._resolve_matrix(t, "core", entry)  # noqa: E731
        lib_env = {"env": {"LIB": "${{ matrix.core.webserver }}"},
                   "run": 'arduino-cli compile --library "$LIB" a.ino'}
        self.assertEqual(gs._vendored_passed([lib_env], resolve), ["X"])
        self.assertEqual(gs._vendored_passed([lib_env], lambda t: gs._resolve_matrix(
            t, "core", {"version": "3.3.11"})), [])
        self.assertEqual(gs._vendored_passed(
            [{"run": "arduino-cli compile --libraries firmware/third_party/X a.ino"}], resolve), [])
        self.assertEqual(gs._vendored_passed(
            [{"run": "# --library firmware/third_party/X\narduino-cli compile a.ino"}], resolve), [])

    def _refused(self, *patches) -> str:
        from contextlib import ExitStack
        with ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            with self.assertRaises(SystemExit) as cm:
                gs.build_document()
        return str(cm.exception)

    def test_a_directory_with_no_facts_and_facts_with_no_directory_are_refused(self):
        msg = self._refused(mock.patch.object(gs, "VENDORED_FACTS", {}))
        self.assertIn("has no VENDORED_FACTS entry", msg)
        extra = dict(gs.VENDORED_FACTS, **{"Gone-1.0.0-securacv": gs.VENDORED_FACTS[VENDORED_DIR]})
        msg = self._refused(mock.patch.object(gs, "VENDORED_FACTS", extra))
        self.assertIn("Gone-1.0.0-securacv", msg)

    def test_a_manifest_version_that_does_not_name_the_core_is_refused(self):
        facts = {VENDORED_DIR: dict(gs.VENDORED_FACTS[VENDORED_DIR], arduino_core="2.0.16")}
        msg = self._refused(mock.patch.object(gs, "VENDORED_FACTS", facts))
        self.assertIn("2.0.16+securacv.<n>", msg)

    def test_a_link_on_another_core_is_refused(self):
        core2 = next(lit for lit, f in gs.PLATFORM_FACTS.items() if f["arduino_core"] == "2.0.17")
        moved = dict(gs.PLATFORM_FACTS, **{core2: dict(gs.PLATFORM_FACTS[core2], arduino_core="3.3.11")})
        msg = self._refused(mock.patch.object(gs, "PLATFORM_FACTS", moved))
        self.assertIn("on core 3.3.11", msg)
        rows = [dict(w, vendored=[VENDORED_DIR]) if w["version"] == "3.3.11"
                and "canary-display" in w["products"] else w for w in self.workflow]
        msg = self._refused(mock.patch.object(gs, "arduino_core_pins", return_value=rows))
        self.assertIn("on core 3.3.11", msg)
        self.assertIn("--library", msg)

    def test_a_row_naming_an_unknown_directory_is_refused(self):
        rows = [dict(w, vendored=["Nope-1.0"]) if w["vendored"] else w for w in self.workflow]
        msg = self._refused(mock.patch.object(gs, "arduino_core_pins", return_value=rows))
        self.assertIn("firmware/third_party/Nope-1.0, which is not a library directory", msg)

    def test_a_copy_nothing_links_is_refused(self):
        real = gs.env_facts
        unlinked = lambda p, c, e: dict(real(p, c, e), lib_extra_dirs=[  # noqa: E731
            d for d in real(p, c, e)["lib_extra_dirs"] if "third_party" not in d])
        rows = [dict(w, vendored=[]) for w in self.workflow]
        msg = self._refused(mock.patch.object(gs, "env_facts", side_effect=unlinked),
                            mock.patch.object(gs, "arduino_core_pins", return_value=rows))
        self.assertIn("rather than record a patched library nothing builds", msg)


if __name__ == "__main__":
    sys.exit(unittest.main())
