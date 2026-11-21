"""gen_enclosures.py: what a reader sees for a knob and for a package.

A knob's label is the first line of its SCAD comment, and a package's
contents are a README table cell. Both reached the Lab's Workshop raw:
twelve labels stopped wherever the comment's column ran out ("…(the pod
hangs USB-down on", "…plate's edge — the", "The single most load-"), and two
packages printed Markdown link syntax at the reader. This holds the two
functions that tidy them, on the shapes that shipped, and holds the committed
catalogs to the result.

Run by canary-local.yml's `unittest discover -s canary-local/tools/tests`.
"""
from __future__ import annotations

import importlib.util
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

LAB = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(LAB / "tools"))  # gen_enclosures imports its neighbors by name
spec = importlib.util.spec_from_file_location("gen_enclosures", LAB / "tools" / "gen_enclosures.py")
G = importlib.util.module_from_spec(spec)
sys.modules["gen_enclosures"] = G
spec.loader.exec_module(G)  # type: ignore[union-attr]


def strings(node, path=""):
    if isinstance(node, dict):
        for key, value in node.items():
            yield from strings(value, f"{path}.{key}")
    elif isinstance(node, list):
        for i, value in enumerate(node):
            yield from strings(value, f"{path}[{i}]")
    elif isinstance(node, str):
        yield path, node


class TidyLabel(unittest.TestCase):
    CUT = [
        ("Ø2 drains, straight out through the BOTTOM wall (the pod hangs USB-down on",
         "Ø2 drains, straight out through the BOTTOM wall"),
        ("M2 brass heat-set inserts in the post ends (service-grade threads;",
         "M2 brass heat-set inserts in the post ends"),
        ("poka-yoke: a rib on the +Y (camera-end) bore wall and a slot in the plate's edge — the",
         "poka-yoke: a rib on the +Y (camera-end) bore wall and a slot in the plate's edge"),
        ("one BLIND keyhole in the back (wall hang) — the pocket",
         "one BLIND keyhole in the back (wall hang)"),
        ("Ø2 weep straight out through the bottom wall, over the gasket groove (straight down when hung):",
         "Ø2 weep straight out through the bottom wall, over the gasket groove (straight down when hung)"),
        ("the drop collar — see 4 above. The single most load-",
         "the drop collar — see 4 above"),
        ("a fingernail notch in the drum rim at pry_ang (mid-wall between two snap windows,",
         "a fingernail notch in the drum rim at pry_ang"),
    ]

    def test_a_label_cut_by_its_column_ends_where_it_was_whole(self):
        for raw, want in self.CUT:
            self.assertEqual(G.tidy_label(raw, True), want, raw)

    def test_a_comment_that_fits_its_line_is_left_alone(self):
        for raw, _ in self.CUT:
            self.assertEqual(G.tidy_label(raw, False), raw.strip(), raw)
        for whole in ("camera window in the face", "gasket seal (IP-style, needs the TPU ring)",
                      "status LED — a light pipe to the face", "Qi coil pocket, 50 mm"):
            self.assertEqual(G.tidy_label(whole, True), whole)

    def test_it_never_returns_nothing(self):
        self.assertEqual(G.tidy_label("(see below", True), "(see below")
        self.assertEqual(G.tidy_label("", True), "")

    def test_a_consequence_on_a_later_line_is_found(self):
        G._CONTINUATIONS[G.ENC / "x.scad"] = {"knob": "its pole) -> adds a drain"}
        self.addCleanup(G._CONTINUATIONS.pop, G.ENC / "x.scad", None)
        self.assertEqual(G.option_text("x.scad", {"name": "knob", "comment": "Ø2 drains (the pod hangs on"}),
                         ("Ø2 drains", "adds a drain"))
        self.assertEqual(G.option_text("x.scad", {"name": "other", "comment": "a vent -> breathes"}),
                         ("a vent", "breathes"))

    def test_continuations_are_read_from_a_real_file(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "case.scad"
            f.write_text(
                "opt_weep = true;         // Ø2 drains, straight out (the pod hangs USB-down on\n"
                "                         // its pole), a web above the gasket groove\n"
                "sma_boss = true;         // raised land around the SMA\n"
                "// a remark at the margin about what follows\n"
                "wall = 2.0;\n", encoding="utf-8")
            self.assertEqual(G.comment_continuations(f),
                             {"opt_weep": "its pole), a web above the gasket groove"})


class PlainText(unittest.TestCase):
    def test_a_markdown_link_is_shown_as_its_text(self):
        self.assertEqual(G.plain_text("pinned (see [display research](../display_research.md)); v0.3"),
                         "pinned (see display research); v0.3")
        self.assertEqual(G.plain_text("a wall cradle ([`canary_cradle_lib.scad`](./canary_cradle_lib.scad))"),
                         "a wall cradle (canary_cradle_lib.scad)")
        self.assertEqual(G.plain_text("**70 × 40** mm"), "70 × 40 mm")
        self.assertEqual(G.plain_text("an array [0] (not a link)"), "an array [0] (not a link)")


class TheCommittedCatalogs(unittest.TestCase):
    def test_no_markdown_link_reaches_a_reader(self):
        for name in ("workshop.json", "catalog.json", "build.json"):
            data = json.loads((LAB / "devices" / name).read_text(encoding="utf-8"))
            for path, text in strings(data):
                self.assertIsNone(re.search(r"\]\([^)\s]+\)", text), f"{name}{path}: {text[:80]}")

    def test_no_option_label_ends_inside_a_parenthesis_or_on_a_dangling_mark(self):
        data = json.loads((LAB / "devices" / "catalog.json").read_text(encoding="utf-8"))
        seen = 0
        for product in data["products"]:
            for option in product.get("options", []):
                label = option["label"]
                seen += 1
                self.assertEqual(label.count("("), label.count(")"),
                                 f"{product.get('id')}: {label}")
                self.assertFalse(label.rstrip().endswith((",", ";", ":", "—", "–", "-")),
                                 f"{product.get('id')}: {label}")
        self.assertGreater(seen, 100)


if __name__ == "__main__":
    unittest.main()
