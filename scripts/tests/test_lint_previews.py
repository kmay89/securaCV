"""scripts/lint_previews.py — the enclosure preview gallery's three rots.

Runs the linter's `problems()` on a scratch tree: a README, a render.sh, a
docs/hardware page, the three catalogs and a preview directory. A clean
tree passes; each kind of rot is named exactly once, by name; and the
render.sh parser reads every line form the real script uses — the plain
`png "…"` line, the `(SRC=…; png "…")` one-liner and the multi-line
`(SRC=…` subshell whose `png` lines follow on their own (the parser's first
version let a multi-line block's first line swallow every `png` after it,
so the five Vision previews read as "unrendered" on a clean tree).

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("lint_previews", REPO / "scripts" / "lint_previews.py")
lp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lp)  # type: ignore[union-attr]

RENDER_SH = '''#!/usr/bin/env bash
png() { :; }
if [[ "${1:-}" != "--no-png" ]]; then
  png "preview_all.png"     -D 'preset="battery_full"'    -D 'part="all"'
  (SRC="$VSRC"
   png "preview_vision_xiao_indoor.png"  -D 'host="xiao"'   -D 'part="all"'
   png "preview_vision_knob.png"         -D 'part="knob"')
  (SRC="$DSRC"; png "preview_doorbell.png" -D 'part="all"')
  (SRC="canary_watch_station.scad"; png "preview_dev_station.png" -D 'part="all"')
fi
'''

README = '''# Enclosures
| A | B | C |
|---|---|---|
| WAP | full | <img src="./preview_all.png" width="240"> |
| Vision | indoor | <img src="./preview_vision_xiao_indoor.png" width="240"> |
| Vision | knob | <img src="./preview_vision_knob.png" width="240"> |
| Doorbell | plate | <img src="./preview_doorbell.png" width="240"> |
<img src="./preview_dev_station.png" width="320">
'''

ALL = ["preview_all.png", "preview_vision_xiao_indoor.png", "preview_vision_knob.png",
       "preview_doorbell.png", "preview_dev_station.png"]


def scratch(tmp: Path, *, readme=README, render=RENDER_SH, pngs=ALL, extra_md="",
            catalog_names=None) -> Path:
    enc = tmp / "docs/hardware/enclosure"
    enc.mkdir(parents=True)
    (enc / "README.md").write_text(readme)
    (enc / "render.sh").write_text(render)
    for name in pngs:
        (enc / name).write_bytes(b"\x89PNG")
    if extra_md:
        (tmp / "docs/hardware/canary_vision_pro_recamera.md").write_text(extra_md)
    dev = tmp / "canary-local/devices"
    dev.mkdir(parents=True)
    names = ALL if catalog_names is None else catalog_names
    for rel in lp.CATALOG_RELS:
        (tmp / rel).write_text('{"previews": [' + ", ".join(f'"./{n}"' for n in names) + "]}")
    return tmp


class LintPreviews(unittest.TestCase):

    def run_on(self, **kw) -> list[str]:
        with tempfile.TemporaryDirectory() as d:
            return lp.problems(scratch(Path(d), **kw))

    def test_clean_tree_passes(self):
        self.assertEqual(self.run_on(), [])

    def test_recipe_parser_reads_every_line_form(self):
        # plain, multi-line subshell (two lines), one-liner subshell (two)
        self.assertEqual(lp.RECIPE.findall(RENDER_SH),
                         ["preview_all.png", "preview_vision_xiao_indoor.png", "preview_vision_knob.png",
                          "preview_doorbell.png", "preview_dev_station.png"])

    def test_missing_png_is_named(self):
        bad = self.run_on(pngs=[n for n in ALL if n != "preview_vision_knob.png"])
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("missing:", bad[0])
        self.assertIn("preview_vision_knob.png", bad[0])
        self.assertIn("README.md", bad[0])

    def test_orphan_png_is_named(self):
        bad = self.run_on(pngs=ALL + ["preview_dev_forgotten.png"])
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("orphan:", bad[0])
        self.assertIn("preview_dev_forgotten.png", bad[0])

    def test_readme_image_without_a_recipe_is_named(self):
        render = RENDER_SH.replace('''  (SRC="$DSRC"; png "preview_doorbell.png" -D 'part="all"')\n''', "")
        bad = self.run_on(render=render)
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("unrendered:", bad[0])
        self.assertIn("preview_doorbell.png", bad[0])

    def test_recipe_nothing_references_is_named(self):
        render = RENDER_SH.replace("fi\n", '''  png "preview_dev_gone.png" -D 'part="all"'\nfi\n''')
        bad = self.run_on(render=render)
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("stale-recipe:", bad[0])
        self.assertIn("preview_dev_gone.png", bad[0])

    def test_another_hardware_page_keeps_a_png_alive(self):
        # canary_vision_pro_recamera.md embeds ./enclosure/preview_dev_visionpro.png
        # on the real tree: referenced from there and rendered, it is not an orphan
        render = RENDER_SH.replace("fi\n", '''  png "preview_dev_visionpro.png" -D 'part="plate"'\nfi\n''')
        md = '<img src="./enclosure/preview_dev_visionpro.png" width="320">\n'
        self.assertEqual(self.run_on(render=render, pngs=ALL + ["preview_dev_visionpro.png"],
                                     extra_md=md), [])
        # ...and without that page it is an orphan, and its recipe is stale
        bad = self.run_on(render=render, pngs=ALL + ["preview_dev_visionpro.png"])
        self.assertEqual([b.split(":")[0] for b in bad], ["orphan", "stale-recipe"], bad)

    def test_catalog_reference_alone_is_a_missing_png_not_a_pass(self):
        # a generated catalog naming a PNG that is not on disk: the Lab would
        # show a broken image, so it is "missing" even though the README is clean
        bad = self.run_on(catalog_names=ALL + ["preview_dev_phantom.png"])
        self.assertEqual(len(bad), 1, bad)
        self.assertIn("missing:", bad[0])
        self.assertIn("catalog.json", bad[0])

    def test_real_tree_is_clean(self):
        self.assertEqual(lp.problems(REPO), [])


if __name__ == "__main__":
    unittest.main()
