#!/usr/bin/env python3
"""A scratch .scad beside the case files is never a catalog product.

gen_enclosures.py's catalog globs docs/hardware/enclosure/*.scad. The
enclosure generators that measure a case through scad_probe.py
(gen_assembled_dims.py, gen_hardware.py) write a hidden
`.tmp_probe_<label>.scad` into that same folder — an `include <case>` plus
Customizer overrides — and delete it when the render returns. Running the
catalog while one is on disk (a parallel regen, a killed measurement) used to
add it as a product with the case's whole parameter inventory. Pinned here:
the name rule, and a catalog built with strays on disk is byte-for-byte the
catalog built without them.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

# NOTE: importing gen_enclosures RUNS it — build_main() and catalog_main() are
# module-level calls, so this import (and test_bom_overlay.py's, which unittest
# discovery imports first) rewrites the real build.json, workshop.json and
# catalog.json: on a current tree the bytes do not move, on a stale one they
# are silently brought current. So the catalog on disk is never evidence of
# anything here; whether the COMMITTED catalog is current is canary-local.yml's
# "Enclosure catalog drift" step (regenerate, then `git diff --exit-code`),
# which runs before this suite in the same job.
import gen_enclosures as ge  # noqa: E402

# What scad_probe.probe() writes, verbatim in shape: the include, the
# overrides, the body — a file that parses as a full case.
PROBE_TEXT = 'include <canary_wap_enclosure.scad>\npreset = "battery_full";\n\npart = "lid";\n'


class ScratchNames(unittest.TestCase):
    def test_hidden_and_tmp_named_files_are_scratch(self):
        for name in (".tmp_probe_device_canary_wap.scad", ".tmp_assembled_x.scad",
                     ".anything.scad", "tmp_probe.scad", "TMP_x.scad", "_tmp_x.scad"):
            self.assertTrue(ge.is_scratch_scad(name), name)
        for name in ("canary_wap_enclosure.scad", "canary_board_lib.scad",
                     "canary_s3_lcd7_stamp.scad"):
            self.assertFalse(ge.is_scratch_scad(name), name)

    def test_case_scads_skips_them(self):
        with tempfile.TemporaryDirectory() as td:
            enc = Path(td)
            for name in ("canary_a.scad", ".tmp_probe_a.scad", "tmp_b.scad", "notes.txt"):
                (enc / name).write_text("// x\n", encoding="utf-8")
            self.assertEqual([p.name for p in ge.case_scads(enc)], ["canary_a.scad"])

    def test_the_committed_folder_has_no_scratch_file(self):
        # a scratch file committed by accident would be skipped here and
        # carried nowhere else — say so instead of hiding it. The names come
        # from the index, not the disk: a probe in flight (a parallel regen)
        # is on disk for the length of one render, and is not a commit.
        try:
            listed = subprocess.run(
                ["git", "ls-files", "--", "*.scad"], cwd=ge.ENC, check=True,
                capture_output=True, text=True).stdout.split()
        except (OSError, subprocess.CalledProcessError):
            listed = [p.name for p in ge.ENC.glob("*.scad")]  # no git: the disk
        stray = [Path(n).name for n in listed if ge.is_scratch_scad(Path(n).name)]
        self.assertEqual(stray, [])


class CatalogIgnoresStrays(unittest.TestCase):
    def _catalog(self, enc: Path, out: Path) -> dict:
        # the generator's module-level paths, pointed at `enc`, writing `out`:
        # THIS call never writes the real catalog.json (REPO only names it in
        # the log line) — the import above already did, see the note there
        with mock.patch.object(ge, "ENC", enc), mock.patch.object(ge, "CATALOG_JSON", out), \
                mock.patch.object(ge, "REPO", out.parent), mock.patch("builtins.print"):
            ge.catalog_main()
        return json.loads(out.read_text(encoding="utf-8"))

    def test_a_stray_probe_file_adds_no_product(self):
        with tempfile.TemporaryDirectory() as td:
            enc = Path(td) / "enclosure"
            enc.mkdir()
            for p in ge.ENC.glob("*.scad"):
                shutil.copy2(p, enc / p.name)
            shutil.copy2(ge.ENC / "README.md", enc / "README.md")
            clean = self._catalog(enc, Path(td) / "clean.json")
            (enc / ".tmp_probe_device_canary_wap.scad").write_text(PROBE_TEXT, encoding="utf-8")
            (enc / "tmp_scratch.scad").write_text(PROBE_TEXT, encoding="utf-8")
            dirty = self._catalog(enc, Path(td) / "dirty.json")
            real = self._catalog(ge.ENC, Path(td) / "real.json")
        ids = [p["id"] for p in dirty["products"]]
        self.assertFalse([i for i in ids if "tmp" in i], ids)
        self.assertEqual(dirty, clean)
        # and the copy IS the real folder: the clean run equals a run over
        # docs/hardware/enclosure itself (a copy missing a file would make
        # clean and dirty equally wrong, and the comparison above vacuous)
        self.assertEqual(clean, real)


if __name__ == "__main__":
    sys.exit(unittest.main())
