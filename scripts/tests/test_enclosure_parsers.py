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

They disagreed on groups too: the Lab matched a `/* [Group] */` header only
when it closed on its own line, so the 52 multi-line headers in 10 case files
were invisible to it and 333 knobs sat in the group before (the WAP's
tol_slide under "Shell", where the builder says "Print tolerances"). The Lab
now takes the builder's own GROUP_RE, and Groups holds the two parsers to one
group for every knob both read.

gen_enclosures.py runs its writers at import, so its parse_scad is lifted
out of the source with `ast` rather than imported, together with its
`from gen_builder_manifest import ...` line. A lifted copy sees only what is
lifted, so a later rebinding of GROUP_RE in the generator would pass it while
the real run regrouped every knob. Two tests close that: Groups holds the
committed enclosures.json (CI's "Enclosure catalog drift" step keeps it equal
to the real generator's output) to the builder's groups, and Binding holds
the generator to that one import as GROUP_RE's only binding.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import ast
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENC = ROOT / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
import gen_builder_manifest  # noqa: E402


LAB_SRC = ROOT / "canary-local" / "tools" / "gen_enclosures.py"
ENCLOSURES_JSON = ROOT / "canary-local" / "devices" / "enclosures.json"


def _lab_parse_scad():
    src = LAB_SRC.read_text(encoding="utf-8")
    body = ast.parse(src).body
    fn = next(n for n in body
              if isinstance(n, ast.FunctionDef) and n.name == "parse_scad")
    # what the generator imports from the builder's parser, as it imports it
    shared = [n for n in body
              if isinstance(n, ast.ImportFrom) and n.module == "gen_builder_manifest"]
    ns = {"re": re, "Path": Path}
    exec(compile(ast.Module([*shared, fn], []), "gen_enclosures.parse_scad", "exec"), ns)
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


def groups_of(path: Path) -> tuple[dict, dict]:
    """({knob: group} as the builder reads it, {knob: group} as the Lab
    does). A knob either parser reads twice keeps the group of its last
    reading, the way each parser's consumers look a knob up by name."""
    builder = {p["name"]: g["name"]
               for g in gen_builder_manifest.parse_scad(path) for p in g["params"]}
    lab = {p["name"]: g["name"] for g in LAB_PARSE(path)["groups"] for p in g["params"]}
    return builder, lab


class Groups(unittest.TestCase):
    def test_both_parsers_put_every_knob_in_one_group(self):
        bad, both = [], 0
        for path in sorted(ENC.glob("canary_*.scad")):
            builder, lab = groups_of(path)
            for name in builder.keys() & lab.keys():
                both += 1
                if builder[name] != lab[name]:
                    bad.append(f"{path.name}:{name}: builder '{builder[name]}' vs Lab '{lab[name]}'")
        self.assertGreater(both, 1000, "the parsers should share well over a thousand knobs")
        self.assertEqual(bad, [], "\n".join(bad))

    def test_the_committed_lab_catalog_groups_like_the_builder(self):
        # The output itself, not a lifted copy of the parser: whatever
        # binding the generator really ran, this is what it wrote.
        scads = json.loads(ENCLOSURES_JSON.read_text(encoding="utf-8"))["scads"]
        self.assertGreater(len(scads), 20, "enclosures.json should list the case files")
        bad, both = [], 0
        for scad, parsed in sorted(scads.items()):
            builder = {p["name"]: g["name"]
                       for g in gen_builder_manifest.parse_scad(ENC / scad) for p in g["params"]}
            lab = {p["name"]: g["name"] for g in parsed["groups"] for p in g["params"]}
            for name in builder.keys() & lab.keys():
                both += 1
                if builder[name] != lab[name]:
                    bad.append(f"{scad}:{name}: builder '{builder[name]}' vs "
                               f"enclosures.json '{lab[name]}'")
        self.assertGreater(both, 1000, "the catalog should share well over a thousand knobs")
        self.assertEqual(bad, [], "\n".join(bad))
        wap = {p["name"]: g["name"]
               for g in scads["canary_wap_enclosure.scad"]["groups"] for p in g["params"]}
        self.assertEqual(wap["tol_slide"], "Print tolerances")

    def test_the_wap_tolerances_are_print_tolerances(self):
        # The WAP's header is two lines long; before C10 the Lab filed these
        # under "Shell", the group before it.
        builder, lab = groups_of(ENC / "canary_wap_enclosure.scad")
        for name in ("tol_slide", "tol_press", "tol_hole"):
            self.assertEqual(lab[name], "Print tolerances", name)
            self.assertEqual(builder[name], "Print tolerances", name)

    def test_header_forms(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "canary_probe.scad"
            p.write_text("/* [One line] — the whole note */\n"
                         "a = 1;  // in the one-line group\n"
                         "/* [Two lines] — the note starts here\n"
                         "ghost = 2;\n"
                         " *  and runs on, a clause ending on a dash —\n"
                         "   and closes here */\n"
                         "b = 3;  // in the two-line group\n"
                         "/*[Tight]*/\n"
                         "c = 4;\n", encoding="utf-8")
            builder, lab = groups_of(p)
            lab_groups = {g["name"]: g for g in LAB_PARSE(p)["groups"]}
        want = {"a": "One line", "b": "Two lines", "c": "Tight"}
        self.assertEqual(lab, want)
        self.assertEqual(builder, want)
        self.assertNotIn("ghost", lab, "a header's own lines are never a knob")
        self.assertEqual(lab_groups["One line"]["note"], "the whole note")
        self.assertEqual(lab_groups["Two lines"]["note"],
                         "the note starts here ghost = 2; "
                         "and runs on, a clause ending on a dash — and closes here")
        self.assertEqual(lab_groups["Tight"]["note"], "")


class SplitLines(unittest.TestCase):
    """The Lab's parser reads one assignment per line and skips a line that
    holds two. C10c-2 split such lines one knob per line, so the Lab lists
    knobs it never showed."""

    def test_the_169_skirt_reaches_the_lab_with_its_own_help(self):
        # Before the split, `skirt_wall = 1.6; skirt_dep = back_stack;
        # // must not exceed back_stack` was one line, and the Lab listed
        # neither knob.
        lab = {p["name"]: p for g in LAB_PARSE(ENC / "canary_s3_touch169.scad")["groups"]
               for p in g["params"]}
        missing = {"skirt_wall", "skirt_dep"} - lab.keys()
        self.assertEqual(missing, set(), "the Lab should list both skirt knobs")
        help_text = lab["skirt_wall"].get("comment", "")
        self.assertIn("skirt", help_text)
        self.assertNotIn("back_stack", help_text)


class Binding(unittest.TestCase):
    """GROUP_RE reaches the generator's parse_scad by one road: the import
    from gen_builder_manifest. Any other binding of the name anywhere in
    gen_enclosures.py (an assignment, a `global`, a parameter, a second
    import or alias) is one the lifted parser in this file would not see."""

    def test_group_re_is_bound_only_by_the_builder_import(self):
        tree = ast.parse(LAB_SRC.read_text(encoding="utf-8"))
        imports, other = [], []
        for node in ast.walk(tree):
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                for a in node.names:
                    bound = a.asname or a.name.split(".")[0]
                    if bound == "GROUP_RE":
                        mod = getattr(node, "module", None)
                        (imports if (mod, a.name) == ("gen_builder_manifest", "GROUP_RE")
                         else other).append(f"line {node.lineno}: import binds GROUP_RE")
            elif isinstance(node, ast.Name) and node.id == "GROUP_RE" \
                    and isinstance(node.ctx, (ast.Store, ast.Del)):
                other.append(f"line {node.lineno}: GROUP_RE assigned")
            elif isinstance(node, (ast.Global, ast.Nonlocal)) and "GROUP_RE" in node.names:
                other.append(f"line {node.lineno}: global/nonlocal GROUP_RE")
            elif isinstance(node, ast.ExceptHandler) and node.name == "GROUP_RE":
                other.append(f"line {node.lineno}: except ... as GROUP_RE")
            elif isinstance(node, ast.arg) and node.arg == "GROUP_RE":
                other.append(f"line {node.lineno}: GROUP_RE as a parameter")
            elif isinstance(node, (ast.FunctionDef, ast.ClassDef)) and node.name == "GROUP_RE":
                other.append(f"line {node.lineno}: GROUP_RE defined")
        self.assertEqual(len(imports), 1, "gen_enclosures.py should import GROUP_RE "
                                          "from gen_builder_manifest exactly once")
        self.assertEqual(other, [], "\n".join(other))


if __name__ == "__main__":
    unittest.main()
