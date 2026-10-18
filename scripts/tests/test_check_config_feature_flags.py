#!/usr/bin/env python3
"""Pins firmware/scripts/check_config_feature_flags.py's skip list (sweep F258).

What is pinned and why:
  • the committed tree is green;
  • firmware/configs/canary-wap/ is gone and SKIPPED_PROJECTS no longer names
    it: no build compiled that directory, the WAP's flags are its sketch's
    build_config.h, and the Lab's generators read the sketch now;
  • a SKIPPED_PROJECTS entry whose directory is gone FAILS the check, by name —
    an exemption must not outlive what it exempts;
  • a configs/ project with no firmware/projects/ tree to read it fails unless
    it is skipped with a reason, so deleting the skip list's last entry did not
    quietly stop the check from noticing the next uncompiled directory.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "firmware" / "scripts" / "check_config_feature_flags.py"


def load():
    spec = importlib.util.spec_from_file_location("check_config_feature_flags", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(mod) -> tuple[int, str]:
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = mod.main()
    return rc, buf.getvalue()


class SkipList(unittest.TestCase):
    def test_the_tree_is_green(self):
        rc, out = run(load())
        self.assertEqual(rc, 0, out)

    def test_the_wap_directory_and_its_exemption_are_gone(self):
        mod = load()
        self.assertFalse((REPO / "firmware/configs/canary-wap").exists())
        self.assertNotIn("canary-wap", mod.SKIPPED_PROJECTS)

    def test_a_stale_skip_entry_fails_by_name(self):
        mod = load()
        with mock.patch.dict(mod.SKIPPED_PROJECTS, {"canary-wap": "no build compiles it"}):
            rc, out = run(mod)
        self.assertEqual(rc, 1)
        self.assertIn("stale SKIPPED_PROJECTS entry canary-wap", out)

    def test_an_uncompiled_configs_project_still_fails_unless_skipped(self):
        mod = load()
        with tempfile.TemporaryDirectory() as tmp:
            configs = Path(tmp) / "configs"
            shutil.copytree(mod.CONFIGS, configs)
            stray = configs / "canary-nowhere" / "default"
            stray.mkdir(parents=True)
            (stray / "config.h").write_text("#define FEATURE_NOTHING 1\n")
            with mock.patch.object(mod, "CONFIGS", configs):
                rc, out = run(mod)
                self.assertEqual(rc, 1)
                self.assertIn("firmware/configs/canary-nowhere/ has no firmware/projects/canary-nowhere/", out)
                with mock.patch.dict(mod.SKIPPED_PROJECTS, {"canary-nowhere": "a test fixture"}):
                    rc, out = run(mod)
                self.assertEqual(rc, 0, out)


if __name__ == "__main__":
    unittest.main()
