#!/usr/bin/env python3
"""Pins docs/hardware/enclosure/gen_cad_params.py — the generator that lets a
device manifest OWN the board knobs of its case.

What is pinned and why:
  • the committed tree is a fixed point: check() is empty, write() writes
    nothing, and every owned case renders to its own bytes — the mechanism
    landed with zero .scad bytes moved, and this keeps it provable;
  • a changed value moves exactly one line and only its literal token: the
    help comment, the indent and every other line are byte-identical, and
    writing twice is the same as writing once (idempotence);
  • the eligibility set IS gen_builder_manifest.parse_scad's: with_lines=True
    points at the assignment it accepted and changes nothing else about its
    output, so a same-named local inside a module can never be hit;
  • every refusal fires by name — a selector, a computed / [Hidden] knob, a
    type mismatch, a two-knob line, a knob assigned twice, a non-scalar
    value, two manifests disagreeing on a shared key — and a refusal anywhere
    means nothing is written anywhere.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import re
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs" / "hardware" / "enclosure"
DEVICES = REPO / "devices"
SCRIPT = ENC / "gen_cad_params.py"

# The generator imports parse_scad from its own directory; the linter puts
# that directory on sys.path, and so does the generator itself — loading it
# by path here mirrors how the linter's own tests load the linter.
if str(ENC) not in sys.path:
    sys.path.insert(0, str(ENC))
spec = importlib.util.spec_from_file_location("gen_cad_params", SCRIPT)
gcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gcp)  # type: ignore[union-attr]
from gen_builder_manifest import parse_scad  # noqa: E402

RELEASED = ["canary_wap_enclosure.scad", "canary_vision_enclosure.scad",
            "canary_vision_doorbell.scad", "canary_sense_enclosure.scad"]
WAP = ENC / "canary_wap_enclosure.scad"
WAP_REL = "docs/hardware/enclosure/canary_wap_enclosure.scad"
VISION_REL = "docs/hardware/enclosure/canary_vision_enclosure.scad"
FIXTURE = """\
/* [Boards] */
n = 5;   // an integer-spelled knob
f = 5.0; // a decimal-spelled knob
s = "abc"; // a string knob
b = true;  // a bool knob
host = "xiao"; // ["xiao","devkit"] a selector
aa_dx = 0.0; aa_dy = 0.0;   // two knobs on one line
h = n + 1;  // computed
twice = 1;
twice = 2;
/* [Hidden] */
secret = 1;
module m() { n = 3; }
"""


class _Tree:
    """A scratch repo: devices/ copied, and the .scad files the tests own
    copied under the same relative path — so write() can be exercised
    without touching the real sources."""

    def __enter__(self) -> Path:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        shutil.copytree(DEVICES, root / "devices")
        enc = root / "docs" / "hardware" / "enclosure"
        enc.mkdir(parents=True)
        for name in RELEASED + ["canary_s3_touch169.scad"]:
            shutil.copyfile(ENC / name, enc / name)
        return root

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


def edit(root: Path, slug: str, fn) -> None:
    path = root / "devices" / slug / "device.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    fn(data)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def fixture(tmp: Path, text: str = FIXTURE) -> Path:
    path = tmp / "fixture.scad"
    path.write_text(text, encoding="utf-8")
    return path


class CommittedTreeIsAFixedPoint(unittest.TestCase):
    def test_check_is_clean(self):
        self.assertEqual(gcp.check(), [])

    def test_every_owned_case_renders_to_its_own_bytes(self):
        owned, errors = gcp.load_params()
        self.assertEqual(errors, [])
        self.assertEqual(sorted(Path(s).name for s in owned),
                         ["canary_sense_enclosure.scad", "canary_vision_enclosure.scad",
                          "canary_wap_enclosure.scad"])
        for scad_rel, keys in owned.items():
            path = REPO / scad_rel
            r = gcp.render(path, {k: o.value for k, o in keys.items()})
            self.assertEqual(r.errors, [], scad_rel)
            self.assertEqual(r.changes, [], scad_rel)
            self.assertEqual(r.text, path.read_text(encoding="utf-8"), scad_rel)
        # the four released cases: the doorbell has no manifest, so nothing is
        # owned there and a render with nothing owned is the file itself
        for name in RELEASED:
            r = gcp.render(ENC / name, {})
            self.assertEqual((r.changes, r.errors), ([], []), name)
            self.assertEqual(r.text, (ENC / name).read_text(encoding="utf-8"), name)

    def test_shared_case_is_a_union_of_subsets(self):
        owned, _ = gcp.load_params()
        vision = owned[VISION_REL]
        self.assertEqual(vision["dk_l"].slugs, ["canary-vision-devkit"])
        self.assertEqual(vision["xiao_l"].slugs, ["canary-vision"])
        self.assertEqual(sorted(vision), sorted(
            ["vm_l", "vm_w", "xiao_l", "xiao_w", "stack_sock_h", "xiao_below", "vm_front_h",
             "cam_w", "cam_h", "pcb_t", "board_clear", "dk_l", "dk_w", "stack_h"]))
        # same case, same host: the XIAO S3 manifest asserts nothing of its own
        s3 = json.loads((DEVICES / "canary-vision-xiao-s3" / "device.json")
                        .read_text(encoding="utf-8"))
        self.assertEqual(s3["cad"]["scad"], VISION_REL)
        self.assertNotIn("params", s3["cad"])

    def test_write_mode_writes_nothing_on_the_committed_tree(self):
        before = {p: p.read_bytes() for p in ENC.glob("*.scad")}
        written, errors = gcp.write()
        self.assertEqual((written, errors), ([], []))
        self.assertEqual({p: p.read_bytes() for p in ENC.glob("*.scad")}, before)

    def test_cli_check_exit_code(self):
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gcp.main(["--check"]), 0)
        self.assertIn("31 manifest-owned knobs across 3 case file(s)", out.getvalue())


class EligibilityIsTheBuildersParser(unittest.TestCase):
    def test_with_lines_points_at_the_assignment_and_changes_nothing_else(self):
        with_lines = parse_scad(WAP, with_lines=True)
        plain = parse_scad(WAP)
        lines = WAP.read_text(encoding="utf-8").splitlines()
        for g in with_lines:
            for p in g["params"]:
                self.assertRegex(lines[p["line"] - 1], rf"^\s*{re.escape(p['name'])}\s*=")
        stripped = [{"name": g["name"],
                     "params": [{k: v for k, v in p.items() if k != "line"}
                                for p in g["params"]]} for g in with_lines]
        self.assertEqual(stripped, plain)
        self.assertFalse(any("line" in p for g in plain for p in g["params"]))
        by = {p["name"]: p["line"] for g in with_lines for p in g["params"]}
        self.assertEqual(by["board_l"], 153)
        self.assertEqual(by["board_w"], 154)
        self.assertNotIn("board_stack_h", by)          # computed: not a knob

    def test_eligible_is_the_parsers_set(self):
        params = gcp.eligible(WAP)
        names = {p["name"] for g in parse_scad(WAP) for p in g["params"]}
        self.assertEqual(set(params), names)
        self.assertIn("options", params["preset"])
        self.assertEqual(params["board_w"]["type"], "number")


class RenderMovesOnlyTheToken(unittest.TestCase):
    def test_changed_value_moves_one_line_and_only_its_token(self):
        r = gcp.render(WAP, {"board_w": 17.8})
        self.assertEqual(r.errors, [])
        old = WAP.read_text(encoding="utf-8").splitlines(keepends=True)
        new = r.text.splitlines(keepends=True)
        self.assertEqual(len(old), len(new))
        moved = [i for i, (a, b) in enumerate(zip(old, new)) if a != b]
        self.assertEqual(moved, [153])                              # 0-based: line 154
        self.assertEqual([(c.line, c.name, c.old_token, c.new_token) for c in r.changes],
                         [(154, "board_w", "17.5", "17.8")])
        self.assertEqual(new[153], old[153].replace("= 17.5;", "= 17.8;", 1))
        self.assertIn("// PCB width (along Y)", new[153])       # the help text survived

    def test_write_is_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            once = gcp.render(WAP, {"board_w": 17.8})
            copy = Path(td) / "wap.scad"
            copy.write_text(once.text, encoding="utf-8")
            twice = gcp.render(copy, {"board_w": 17.8})
            self.assertEqual(twice.text, once.text)
            self.assertEqual((twice.changes, twice.errors), ([], []))

    def test_equal_value_keeps_the_line_whatever_its_spelling(self):
        # 21 (a JSON integer) against `21.0` in the file: numerically equal, untouched
        r = gcp.render(WAP, {"board_l": 21, "board_w": 17.50})
        self.assertEqual((r.changes, r.errors), ([], []))
        self.assertEqual(r.text, WAP.read_text(encoding="utf-8"))

    def test_integer_vs_decimal_spelling_is_preserved(self):
        with tempfile.TemporaryDirectory() as td:
            f = fixture(Path(td))
            cases = [({"n": 6.0}, [(2, "5", "6")]),         # integer spelling kept
                     ({"f": 6}, [(3, "5.0", "6.0")]),       # decimal spelling kept
                     ({"n": 6.5}, [(2, "5", "6.5")]),       # a fraction cannot stay an integer
                     ({"s": 'x"y'}, [(4, '"abc"', '"x\\"y"')]),
                     ({"b": False}, [(5, "true", "false")]),
                     ({"n": 5, "f": 5, "s": "abc", "b": True}, [])]
            for owned, want in cases:
                r = gcp.render(f, owned)
                self.assertEqual(r.errors, [], owned)
                self.assertEqual([(c.line, c.old_token, c.new_token) for c in r.changes],
                                 want, owned)

    def test_module_local_of_the_same_name_is_never_touched(self):
        with tempfile.TemporaryDirectory() as td:
            r = gcp.render(fixture(Path(td)), {"n": 9})
            self.assertEqual([(c.line, c.new_token) for c in r.changes], [(2, "9")])
            self.assertIn("module m() { n = 3; }", r.text)


class Refusals(unittest.TestCase):
    def test_selector_is_refused(self):
        for path, knob, value in [(WAP, "preset", "compact_plain"),
                                  (ENC / "canary_vision_enclosure.scad", "host", "xiao"),
                                  (ENC / "canary_sense_enclosure.scad", "radar", "bha2")]:
            r = gcp.render(path, {knob: value})
            self.assertEqual(r.changes, [])
            self.assertEqual(len(r.errors), 1, r.errors)
            self.assertIn(f"cad.params.{knob} is a selector", r.errors[0])
            self.assertIn("render time", r.errors[0])

    def test_computed_hidden_or_unknown_knob_is_refused(self):
        r = gcp.render(WAP, {"board_stack_h": 8.0})
        self.assertIn("board_stack_h is not a literal Customizer knob", r.errors[0])
        self.assertIn("lint_design_lang.py", r.errors[0])
        with tempfile.TemporaryDirectory() as td:
            r = gcp.render(fixture(Path(td)), {"h": 2, "secret": 2, "nope": 1})
        self.assertEqual(len(r.errors), 3, r.errors)
        for e in r.errors:
            self.assertIn("is not a literal Customizer knob", e)

    def test_type_mismatch_is_refused(self):
        r = gcp.render(WAP, {"board_w": "17.5"})
        self.assertEqual(r.changes, [])
        self.assertIn("board_w is a string in the manifest but the knob is a number",
                      r.errors[0])
        with tempfile.TemporaryDirectory() as td:
            r = gcp.render(fixture(Path(td)), {"s": 3, "b": 1})
        self.assertEqual(len(r.errors), 2, r.errors)

    def test_two_assignment_line_is_refused(self):
        r = gcp.render(ENC / "canary_s3_touch169.scad", {"aa_dx": 0.0, "aa_dy": 0.0})
        self.assertEqual(r.changes, [])
        self.assertEqual(len(r.errors), 2, r.errors)
        for e in r.errors:
            self.assertIn("line 69 assigns more than one knob", e)
            self.assertIn("split the line first", e)

    def test_knob_assigned_twice_is_refused(self):
        with tempfile.TemporaryDirectory() as td:
            r = gcp.render(fixture(Path(td)), {"twice": 3})
        self.assertEqual(r.changes, [])
        self.assertIn("assigned more than once at top level (lines 9, 10)", r.errors[0])

    def test_two_manifests_disagreeing_on_a_shared_key_fail_naming_both(self):
        with _Tree() as root:
            edit(root, "canary-vision-devkit",
                 lambda d: d["cad"]["params"].__setitem__("xiao_l", 22.0))
            errors = gcp.check(root / "devices", root)
        self.assertEqual(len(errors), 1, errors)
        for needle in ("cad.params.xiao_l", "devices/canary-vision ", "devices/canary-vision-devkit",
                       "21.0", "22.0", "must agree"):
            self.assertIn(needle, errors[0])

    def test_check_names_knob_manifest_value_and_scad_value(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda d: d["cad"]["params"].__setitem__("board_w", 17.8))
            errors = gcp.check(root / "devices", root)
        self.assertEqual(len(errors), 1, errors)
        for needle in ("canary_wap_enclosure.scad:154", "board_w = 17.5 in the .scad",
                       "devices/canary-wap cad.params says 17.8", "gen_cad_params.py"):
            self.assertIn(needle, errors[0])

    def test_non_scalar_value_is_refused(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda d: d["cad"]["params"].__setitem__("board_l", [21]))
            errors = gcp.check(root / "devices", root)
        self.assertTrue(any("board_l = [21] is not a number, string or boolean" in e
                            for e in errors), errors)

    def test_missing_scad_is_reported(self):
        with _Tree() as root:
            (root / WAP_REL).unlink()
            errors = gcp.check(root / "devices", root)
        self.assertTrue(any(WAP_REL in e and "does not exist" in e for e in errors), errors)


class WriteMode(unittest.TestCase):
    def test_write_changes_the_file_and_check_is_then_clean(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda d: d["cad"]["params"].__setitem__("board_w", 17.8))
            self.assertEqual(len(gcp.check(root / "devices", root)), 1)
            written, errors = gcp.write(root / "devices", root)
            self.assertEqual(errors, [])
            self.assertEqual([(p.name, c.line, c.old_token, c.new_token) for p, c in written],
                             [("canary_wap_enclosure.scad", 154, "17.5", "17.8")])
            self.assertEqual(gcp.check(root / "devices", root), [])
            # only the one token moved
            old = WAP.read_text(encoding="utf-8").splitlines(keepends=True)
            new = (root / WAP_REL).read_text(encoding="utf-8").splitlines(keepends=True)
            self.assertEqual([i for i, (a, b) in enumerate(zip(old, new)) if a != b], [153])
            # and writing again is a no-op
            self.assertEqual(gcp.write(root / "devices", root), ([], []))

    def test_a_refusal_anywhere_writes_nothing_anywhere(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda d: d["cad"]["params"].__setitem__("board_w", 17.8))
            edit(root, "canary-sense", lambda d: d["cad"]["params"].__setitem__("radar", "bha2"))
            before = {p.name: p.read_bytes()
                      for p in (root / "docs/hardware/enclosure").glob("*.scad")}
            written, errors = gcp.write(root / "devices", root)
            self.assertEqual(written, [])
            self.assertTrue(any("cad.params.radar" in e and "is a selector" in e
                                for e in errors), errors)
            after = {p.name: p.read_bytes()
                     for p in (root / "docs/hardware/enclosure").glob("*.scad")}
        self.assertEqual(after, before)


if __name__ == "__main__":
    sys.exit(unittest.main())
