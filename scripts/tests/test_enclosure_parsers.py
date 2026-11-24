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

The two lists of knobs still differ (C21): the Lab reads one `name = value;`
per line, so it misses a knob that shares its line, and it lists [Hidden]
groups, derived values and assignments past the first module, which the
builder does not. The direction that hides a real knob is the first, and it
is already counted: the builder keeps no help on a line that assigns two
knobs, so every knob the Lab misses on a shared line has no help, and
test_lint_design_lang.py's HELP_DEBT holds the count of knobs without help
per file, exactly. LabMisses holds that coupling for every knob the Lab
misses, whatever the reason: a knob the builder reads with help on a line
the Lab's match refuses (indented, above the first group header, with a
block comment or a second statement after its `;`, or with a `;` inside its
value) fails it. HELP_DEBT does not count such a knob, and the Groups tests compare only the knobs both
parsers list, so before LabMisses the Lab could lose it unseen. (A builder
that kept a shared line's help is caught here too, but HelpLines in
test_lint_design_lang.py already pins that.)

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
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
ENC = ROOT / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
import gen_builder_manifest  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_lint_design_lang import HELP_DEBT, knobs_without_help  # noqa: E402


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


def lab_misses(path: Path, lab_names: set[str]) -> list[tuple[int, str]]:
    """(line, knob) for each knob gen_builder_manifest.parse_scad reads that
    is not in lab_names (the Lab's list for the file), in file order."""
    return [(p["line"], p["name"])
            for g in gen_builder_manifest.parse_scad(path, with_lines=True)
            for p in g["params"] if p["name"] not in lab_names]


def lab_misses_with_help(path: Path, lab_names: set[str]) -> list[tuple[int, str]]:
    """The knobs the Lab misses that test_lint_design_lang.knobs_without_help
    does not count: the builder's parser reads them with help, so HELP_DEBT
    cannot see that the Lab lists none of them."""
    unhelped = set(knobs_without_help(path))
    return [(line, name) for line, name in lab_misses(path, lab_names) if name not in unhelped]


# The one line form gen_enclosures.py's parse_scad reads a knob from (its
# `^(\w+)\s*=\s*([^;]+);\s*(?://\s*(.*))?$` match, once a group header has
# opened). The tree case's failure message gives it as the remedy, and
# LabMisses' synthetic cases prove each form it names is refused as written
# and read once rewritten the way it says.
LAB_LINE = ("gen_enclosures.py's parse_scad reads a knob only from a line that starts with "
            "its name at the left margin, below the file's first `/* [Group] */` header, "
            "written `name = value;` with no `;` inside the value and nothing after the `;` "
            "but an optional `// help` (no block comment, no second statement)")


class LabMisses(unittest.TestCase):
    """C21: every knob the builder reads and the Lab does not is one HELP_DEBT
    counts. The Lab's parser skips a line that assigns two knobs, and the
    builder's keeps no help on one, so each knob the Lab misses has no help
    (86 in seven files when this test landed, every one on a shared line).
    What breaks that is a knob the builder reads with help and the Lab
    cannot read: a parser change (the builder keeping a shared line's help,
    which HelpLines in test_lint_design_lang.py pins as well), or a knob on a
    line the Lab's match refuses (LAB_LINE above says which line it reads:
    an indented knob is refused, and so is one above the first group header,
    one with a block comment or a second statement after its `;`, or one
    whose value holds a `;`). The tree case sees the second, which HELP_DEBT
    does not count and Groups, comparing only the knobs both parsers list,
    does not see. A new shared-line knob does not break it (it has no help, so
    HELP_DEBT moves), nor does a new derived value (it only grows the
    Lab-only list, which this class does not judge)."""

    def test_every_knob_the_lab_misses_is_one_help_debt_counts(self):
        scads = json.loads(ENCLOSURES_JSON.read_text(encoding="utf-8"))["scads"]
        self.assertGreater(len(scads), 20, "enclosures.json should list the case files")
        bad = []
        for scad, parsed in sorted(scads.items()):
            lab = {p["name"] for g in parsed["groups"] for p in g["params"]}
            for line, name in lab_misses_with_help(ENC / scad, lab):
                bad.append(f"{scad}:{line}: `{name}` is a knob gen_builder_manifest.parse_scad "
                           "reads with help (the parser the web builder and the design lint "
                           "share), but enclosures.json does not list it, and HELP_DEBT "
                           "(test_lint_design_lang.py) counts only knobs without help, so "
                           f"nothing else sees the Lab miss it. {LAB_LINE}. Write the line "
                           "that way and rerun canary-local/tools/gen_enclosures.py; if it "
                           "already reads that way, the committed enclosures.json is stale "
                           "(or the generator's parse_scad changed): rerun the generator "
                           "and commit what it writes")
        self.assertEqual(bad, [], "\n".join(bad))

    def test_every_lab_file_is_held_by_help_debt(self):
        # The count above only binds where HELP_DEBT holds the file: a Lab
        # file with no entry would let its shared-line knobs grow unseen.
        scads = json.loads(ENCLOSURES_JSON.read_text(encoding="utf-8"))["scads"]
        self.assertEqual(sorted(set(scads) - set(HELP_DEBT)), [],
                         "every file enclosures.json lists needs a HELP_DEBT entry in "
                         "test_lint_design_lang.py")

    SHARED = ("/* [G] */\n"
              "alone = 1.0;   // its own help\n"
              "a = 1.0;  b = 2.0;   // help for both, which neither keeps\n")

    def _lab_and_probe(self, d: str) -> tuple[Path, set[str]]:
        p = Path(d) / "canary_probe.scad"
        p.write_text(self.SHARED, encoding="utf-8")
        return p, {x["name"] for g in LAB_PARSE(p)["groups"] for x in g["params"]}

    def test_a_shared_line_reaches_neither_the_lab_nor_the_builder_help(self):
        with tempfile.TemporaryDirectory() as d:
            p, lab = self._lab_and_probe(d)
            self.assertEqual(lab, {"alone"})
            self.assertEqual(lab_misses(p, lab), [(3, "a"), (3, "b")])
            self.assertEqual(knobs_without_help(p), ["a", "b"])
            self.assertEqual(lab_misses_with_help(p, lab), [])

    # Each line form the Lab refuses and the builder reads with help, as
    # written and as LAB_LINE says to write it (the knob above the first
    # header is the fifth; it moves below the header).
    REFUSED = (
        ("  ind = 2.0;   // indented",
         "ind = 2.0;   // indented"),
        ("blk = 4.0;  /* mm */  // a block comment after its ;",
         "blk = 4.0;   // a block comment after its ;, in mm"),
        ("two = 5.0; echo(two);   // a second statement",
         "two = 5.0;   // a second statement\necho(two);"),
        ('txt = "a;b";   // a ; inside the value',
         'txt = "a,b";   // a ; inside the value'),
    )
    TOP = "top = 1.0;   // above any group header"
    OK = "ok = 3.0;   // alone, at the margin"

    def _probe(self, d: str, lines: list[str]) -> tuple[Path, set[str]]:
        p = Path(d) / "canary_probe.scad"
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return p, {x["name"] for g in LAB_PARSE(p)["groups"] for x in g["params"]}

    def test_a_helped_knob_the_lab_cannot_read_is_caught(self):
        with tempfile.TemporaryDirectory() as d:
            p, lab = self._probe(d, [self.TOP, "/* [G] */",
                                     *(w for w, _ in self.REFUSED), self.OK])
            self.assertEqual(lab, {"ok"})
            self.assertEqual(knobs_without_help(p), [], "the builder reads each with help")
            self.assertEqual(lab_misses_with_help(p, lab),
                             [(1, "top"), (3, "ind"), (4, "blk"), (5, "two"), (6, "txt")])

    def test_the_remedy_reads_every_form_it_names(self):
        # The tree case's message gives LAB_LINE as the fix: following it
        # must make the Lab read the knob, with the builder's help intact.
        for form in ("left margin", "below the file's first", "no `;` inside the value",
                     "block comment", "second statement"):
            self.assertIn(form, LAB_LINE)
        with tempfile.TemporaryDirectory() as d:
            p, lab = self._probe(d, ["/* [G] */", self.TOP,
                                     *(r for _, r in self.REFUSED), self.OK])
            self.assertEqual(lab, {"top", "ind", "blk", "two", "txt", "ok"})
            self.assertEqual(knobs_without_help(p), [])
            self.assertEqual(lab_misses(p, lab), [])

    def test_a_builder_that_kept_shared_help_is_caught(self):
        # One parser change this class catches (HelpLines pins it too): the
        # builder starts keeping a shared line's help. knobs_without_help
        # (and so HELP_DEBT) stops counting a and b, while the Lab still
        # lists neither.
        real = gen_builder_manifest.parse_scad

        def keeps_shared_help(path, with_lines=False):
            groups = real(path, with_lines)
            for g in groups:
                for x in g["params"]:
                    x.setdefault("desc", "help for both, which neither keeps")
            return groups

        with tempfile.TemporaryDirectory() as d:
            p, lab = self._lab_and_probe(d)
            with mock.patch.object(gen_builder_manifest, "parse_scad", keeps_shared_help):
                self.assertEqual(knobs_without_help(p), [])
                self.assertEqual(lab_misses_with_help(p, lab), [(3, "a"), (3, "b")])


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
