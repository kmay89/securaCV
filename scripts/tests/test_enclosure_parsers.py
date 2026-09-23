#!/usr/bin/env python3
"""The web builder and the Lab read one knob the same way.

Two parsers read the enclosure .scad Customizer annotations:
gen_builder_manifest.parse_scad (the securacv.com builder's manifest) and
canary-local/tools/gen_enclosures.py's parse_scad (the Lab's enclosure
catalog). They disagreed on ranges: the builder takes the LAST
`[min:step:max]` bracket anywhere in a knob's trailing comment, the Lab took
only a LEADING one, so the house form `help  // [min:step:max]` gave 43
knobs a slider in the builder and a raw "// [..]" tail on the Lab's help
(C10). These tests hold the two to one answer — range and help — for every
knob in the tree, and for the three forms a range is written in.

gen_enclosures.py runs its writers at import, so its parse_scad is lifted
out of the source with `ast` rather than imported.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import ast
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENC = ROOT / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
import gen_builder_manifest  # noqa: E402


def _lab_parse_scad():
    src = (ROOT / "canary-local" / "tools" / "gen_enclosures.py").read_text(encoding="utf-8")
    fn = next(n for n in ast.parse(src).body
              if isinstance(n, ast.FunctionDef) and n.name == "parse_scad")
    ns = {"re": re, "Path": Path}
    exec(compile(ast.Module([fn], []), "gen_enclosures.parse_scad", "exec"), ns)
    return ns["parse_scad"]


LAB_PARSE = _lab_parse_scad()


def readings(path: Path) -> dict:
    """{knob: ((builder range, builder help), (lab range, lab help))} for the
    knobs both parsers read. A range is [min, step, max] as floats, or None;
    help is whitespace-collapsed the way the builder stores it."""
    builder = {p["name"]: p for g in gen_builder_manifest.parse_scad(path) for p in g["params"]}
    lab = {p["name"]: p for g in LAB_PARSE(path)["groups"] for p in g["params"]}
    out = {}
    for name, b in builder.items():
        lp = lab.get(name)
        if lp is None or "options" in b:     # option lists are a different contract
            continue
        b_rng = [float(b[k]) for k in ("min", "step", "max")] if "min" in b else None
        l_help = re.sub(r"\s+", " ", lp.get("comment", "")).strip(" -—")
        out[name] = ((b_rng, b.get("desc", "")), (lp.get("range"), l_help))
    return out


class RangeForms(unittest.TestCase):
    def test_every_form_reads_the_same(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "canary_probe.scad"
            p.write_text("/* [G] */\n"
                         "a = 6.5;  // opening height: boot clearance  // [4:0.5:8]\n"
                         "b = 6.5;  // [4:0.5:8] opening height: boot clearance\n"
                         "c = 6.5;  // [4:0.5:8]\n"
                         "d = 6.5;  // plain help, no range\n", encoding="utf-8")
            r = readings(p)
        for name in "abcd":
            self.assertEqual(r[name][0], r[name][1], name)
        self.assertEqual(r["a"][1], ([4.0, 0.5, 8.0], "opening height: boot clearance"))
        self.assertEqual(r["c"][1], ([4.0, 0.5, 8.0], ""))
        self.assertEqual(r["d"][1], (None, "plain help, no range"))


class Tree(unittest.TestCase):
    def test_both_parsers_agree_on_every_knob(self):
        bad = []
        for path in sorted(ENC.glob("canary_*.scad")):
            for name, (b, lab) in readings(path).items():
                if b != lab:
                    bad.append(f"{path.name}:{name}: builder {b} vs Lab {lab}")
        self.assertEqual(bad, [], "\n".join(bad))


if __name__ == "__main__":
    unittest.main()
