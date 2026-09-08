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
  • --check goes red on a byte of drift.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import re
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path

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


if __name__ == "__main__":
    sys.exit(unittest.main())
