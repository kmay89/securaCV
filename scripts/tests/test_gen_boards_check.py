"""canary-local/tools/gen_boards.py --check — boards.json vs boards.config.json.

The generator's GLBs are not byte-gated and tests/boards.test.js holds the
geometry facts to the committed meshes; `--check` covers the other half, the
fields copied from the authoring config. Runs `check()` on a scratch pair:
the real config against the real boards.json passes (and the real tree is
clean), while a config edit that was not regenerated — a pinout row, a pose,
a device's board list, a renamed board, a stray row — is named. The
geometry facts are taken from the committed row, so no cascadio, numpy or
trimesh is needed (the module imports them lazily for exactly this reason).

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "canary-local" / "tools"
sys.path.insert(0, str(TOOLS))  # _tooling, as the script itself finds it
spec = importlib.util.spec_from_file_location("gen_boards", TOOLS / "gen_boards.py")
gb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gb)  # type: ignore[union-attr]


class Scratch:
    """A copy of the real config + boards.json + (empty) GLB files under a
    temp repo, so check() can be run against an edited pair."""

    def __init__(self):
        self.tmp = Path(tempfile.mkdtemp())
        (self.tmp / "boards").mkdir()
        (self.tmp / "canary-local" / "devices").mkdir(parents=True)
        (self.tmp / "canary-local" / "boards").mkdir(parents=True)
        shutil.copy(REPO / "boards" / "boards.config.json", self.cfg)
        shutil.copy(REPO / "canary-local" / "devices" / "boards.json", self.out)
        for b in json.loads(self.cfg.read_text())["boards"]:
            (self.tmp / "canary-local" / "boards" / f"{b['id']}.glb").write_bytes(b"glTF")

    @property
    def cfg(self):
        return self.tmp / "boards" / "boards.config.json"

    @property
    def out(self):
        return self.tmp / "canary-local" / "devices" / "boards.json"

    def edit_cfg(self, fn):
        d = json.loads(self.cfg.read_text())
        fn(d)
        self.cfg.write_text(json.dumps(d))

    def edit_out(self, fn):
        d = json.loads(self.out.read_text())
        fn(d)
        self.out.write_text(json.dumps(d))

    def problems(self):
        # glb_rel() anchors on the module's OUT_GLB_DIR / REPO; point both at the scratch
        saved = gb.OUT_GLB_DIR, gb.REPO
        gb.OUT_GLB_DIR, gb.REPO = self.tmp / "canary-local" / "boards", self.tmp
        try:
            return gb.check(self.cfg, self.out, self.tmp)
        finally:
            gb.OUT_GLB_DIR, gb.REPO = saved

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


class GenBoardsCheck(unittest.TestCase):

    def setUp(self):
        self.s = Scratch()
        self.addCleanup(self.s.close)

    def test_real_tree_is_clean(self):
        self.assertEqual(gb.check(), [])
        self.assertEqual(self.s.problems(), [])

    def test_a_pinout_row_edited_in_the_config_is_named(self):
        self.s.edit_cfg(lambda d: d["boards"][0]["pinout"][0].__setitem__("label", "edited"))
        bad = self.s.problems()
        bid = json.loads(self.s.cfg.read_text())["boards"][0]["id"]
        self.assertEqual(len(bad), 1, bad)
        self.assertTrue(bad[0].startswith(f"{bid}.pinout:"), bad[0])

    def test_a_pose_edited_in_the_config_is_named(self):
        self.s.edit_cfg(lambda d: d["boards"][1]["pose"].__setitem__("rx", 9.9))
        bad = self.s.problems()
        self.assertEqual(len(bad), 1, bad)
        self.assertIn(".pose:", bad[0])

    def test_a_device_moved_between_boards_moves_the_index_too(self):
        self.s.edit_cfg(lambda d: d["boards"][0]["devices"].append("canary-nowhere"))
        bad = self.s.problems()
        self.assertEqual(len(bad), 1, bad)  # the row first; the index is compared once rows agree
        self.assertIn(".devices:", bad[0])
        # a stale index alone (rows agree) is named as the index
        self.s.edit_cfg(lambda d: d["boards"][0]["devices"].remove("canary-nowhere"))
        self.s.edit_out(lambda d: d["device_board"].__setitem__("canary-nowhere", ["ghost"]))
        bad = self.s.problems()
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("device_board", bad[0])

    def test_a_board_added_or_renamed_in_the_config_is_named(self):
        self.s.edit_cfg(lambda d: d["boards"][0].__setitem__("id", "renamed_board"))
        bad = self.s.problems()
        kinds = sorted(b.split(":")[0] for b in bad)
        self.assertIn("renamed_board", kinds, bad)               # in the config, not in boards.json
        self.assertTrue(any("not in boards.config.json" in b for b in bad), bad)  # the old id is stray

    def test_a_missing_glb_is_named(self):
        (self.s.tmp / "canary-local" / "boards" / "waveshare_4_3b.glb").unlink()
        bad = self.s.problems()
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("committed GLB missing", bad[0])

    def test_a_row_without_geometry_facts_never_passes(self):
        self.s.edit_out(lambda d: d["boards"]["waveshare_4_3b"].pop("triangles"))
        bad = self.s.problems()
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("lacks ['triangles']", bad[0])

    def test_missing_boards_json_is_named(self):
        self.s.out.unlink()
        bad = self.s.problems()
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("missing", bad[0])


if __name__ == "__main__":
    unittest.main()
