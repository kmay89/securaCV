#!/usr/bin/env python3
"""Pins docs/hardware/enclosure/gen_cad_params.py — the generator that lets a
device manifest OWN the board knobs of its case.

What is pinned and why:
  • the committed tree is a fixed point: check() is empty, write() writes
    nothing, and every owned case renders to its own bytes — the mechanism
    landed with zero .scad bytes moved, the reference conversion moved zero
    more, and this keeps both provable;
  • a changed value moves exactly one line and only its literal token: the
    help comment, the indent and every other line are byte-identical, and
    writing twice is the same as writing once (idempotence);
  • the eligibility set IS gen_builder_manifest.parse_scad's: with_lines=True
    points at the assignment it accepted and changes nothing else about its
    output, so a same-named local inside a module can never be hit;
  • the board registry (canary_board_lib.scad) parses completely — nine rows,
    seven facts — and a row that drifts from the literal shape is a failure,
    never a shorter registry; each reference form resolves; an unknown row,
    dim or fact fails naming the manifest and the library;
  • a reference is declared only where the knob's help comment already cites
    the registry (decision 4): the seventeen references are enumerated here
    with what each cites, the knobs that merely equal a row stay numbers, and
    a mutated registry value moves exactly the lines that reference it;
  • every refusal fires by name — a selector, a computed / [Hidden] knob, a
    type mismatch, a two-knob line, a knob assigned twice, a non-scalar
    value, a malformed reference, two manifests disagreeing on a shared key
    — and a refusal anywhere means nothing is written anywhere;
  • --dry-run SLUG:KNOB=VALUE prints the unified diff a manifest edit would
    write, through the same resolver and the same refusals, and writes
    nothing — a number, a reference, several edits at once, no change, and
    each refusal.

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
LIB_NAME = "canary_board_lib.scad"
LIB = ENC / LIB_NAME
LIB_REL = "docs/hardware/enclosure/" + LIB_NAME
WAP = ENC / "canary_wap_enclosure.scad"
WAP_REL = "docs/hardware/enclosure/canary_wap_enclosure.scad"
VISION_REL = "docs/hardware/enclosure/canary_vision_enclosure.scad"
SENSE_REL = "docs/hardware/enclosure/canary_sense_enclosure.scad"
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

# Decision 4, enumerated: every cad.params reference in the tree, and what it
# stands for. A knob is a reference ONLY where its help comment already cites
# the registry; the WAP's board_h (1.2 == brd_t("xiao") by coincidence, its
# comment says "PCB thickness") and the Sense's pcb_t (no comment at all) stay
# numbers even though the spec's first draft listed them.
REFS = {
    ("canary_wap_enclosure.scad", "board_l"): 'brd_l("xiao")',
    ("canary_wap_enclosure.scad", "board_w"): 'brd_w("xiao")',
    ("canary_vision_enclosure.scad", "dk_l"): 'brd_l("dk_c3")',
    ("canary_vision_enclosure.scad", "dk_w"): 'brd_w("dk_c3")',
    ("canary_vision_enclosure.scad", "vm_l"): 'brd_l("grove_v2")',
    ("canary_vision_enclosure.scad", "vm_w"): 'brd_w("grove_v2")',
    ("canary_vision_enclosure.scad", "xiao_l"): 'brd_l("xiao")',
    ("canary_vision_enclosure.scad", "xiao_w"): "brd_xiao_w_measured()",
    ("canary_vision_enclosure.scad", "stack_sock_h"): "brd_stack_sock_measured()",
    ("canary_vision_enclosure.scad", "cam_w"): 'brd_w("ov5647")',
    ("canary_vision_enclosure.scad", "cam_h"): 'brd_l("ov5647")',
    ("canary_vision_enclosure.scad", "pcb_t"): 'brd_t("grove_v2")',
    ("canary_sense_enclosure.scad", "vm_l"): 'brd_l("mr60")',
    ("canary_sense_enclosure.scad", "vm_w"): 'brd_w("mr60")',
    ("canary_sense_enclosure.scad", "xiao_l"): 'brd_l("xiao")',
    ("canary_sense_enclosure.scad", "xiao_w"): 'brd_w("xiao")',
    ("canary_sense_enclosure.scad", "stack_sock_h"): "brd_stack_sock_measured()",
}
NUMBERS = {
    "canary_wap_enclosure.scad": ["board_h", "board_clear", "stack_camera", "stack_plain"],
    "canary_vision_enclosure.scad": ["xiao_below", "vm_front_h", "board_clear", "stack_h"],
    "canary_sense_enclosure.scad": ["xiao_below", "vm_front_h", "ant_h", "pcb_t", "board_clear",
                                    "xiao_usb_z"],
}
XIAO_ROW = '["xiao",       21.0,  17.5, 1.2, "spec",'


class _Tree:
    """A scratch repo: devices/ copied, and the .scad files the tests own
    copied under the same relative path — the board registry among them,
    because a cad.params reference resolves from it — so write() can be
    exercised without touching the real sources."""

    def __enter__(self) -> Path:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        shutil.copytree(DEVICES, root / "devices")
        enc = root / "docs" / "hardware" / "enclosure"
        enc.mkdir(parents=True)
        for name in RELEASED + ["canary_s3_touch169.scad", LIB_NAME]:
            shutil.copyfile(ENC / name, enc / name)
        return root

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


def edit(root: Path, slug: str, fn) -> None:
    path = root / "devices" / slug / "device.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    fn(data)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def edit_lib(root: Path, old: str, new: str) -> str:
    """Replace one literal in the scratch registry; returns the new source."""
    lib = root / LIB_REL
    src = lib.read_text(encoding="utf-8")
    assert src.count(old) == 1, old
    out = src.replace(old, new)
    lib.write_text(out, encoding="utf-8")
    return out


def fixture(tmp: Path, text: str = FIXTURE) -> Path:
    path = tmp / "fixture.scad"
    path.write_text(text, encoding="utf-8")
    return path


def moved_lines(name: str, root: Path) -> list[int]:
    """1-based lines of the scratch copy of `name` that differ from the tree's."""
    old = (ENC / name).read_text(encoding="utf-8").splitlines(keepends=True)
    new = (root / "docs/hardware/enclosure" / name).read_text(encoding="utf-8") \
        .splitlines(keepends=True)
    assert len(old) == len(new), name
    return [i + 1 for i, (a, b) in enumerate(zip(old, new)) if a != b]


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
        self.assertIn("(17 of them resolved from canary_board_lib.scad)", out.getvalue())


class BoardRegistry(unittest.TestCase):
    def test_committed_lib_parses_nine_rows_and_seven_facts(self):
        reg = gcp.parse_board_registry()
        self.assertEqual(reg.path, LIB)
        self.assertEqual(list(reg.rows), ["xiao", "grove_v2", "ov5647", "mr60", "dk_c3",
                                          "ws147", "ws169", "round_disp", "heltec_v3"])
        xiao = reg.rows["xiao"]
        self.assertEqual((xiao.dims, xiao.status, xiao.line),
                         ({"l": 21.0, "w": 17.5, "t": 1.2}, "spec", 43))
        self.assertIn("brd_xiao_w_measured()", xiao.note)
        self.assertEqual(reg.rows["grove_v2"].dims, {"l": 40.0, "w": 20.0, "t": 1.0})
        self.assertEqual(reg.rows["grove_v2"].status, "measured")
        self.assertEqual(reg.rows["dk_c3"].dims, {"l": 39.0, "w": 25.4, "t": 1.0})
        self.assertEqual(reg.rows["ws147"].dims, {"l": 36.37, "w": 20.32, "t": 1.6})
        self.assertEqual(reg.rows["heltec_v3"].line, 59)
        for row in reg.rows.values():
            self.assertIn(row.status, {"measured", "drawing", "spec", "unmeasured"}, row.id)
        self.assertEqual(list(reg.facts), ["brd_xiao_w_measured", "brd_stack_sock_measured",
                                           "brd_stack_sock_unmeasured", "brd_ws147_brass_c3",
                                           "brd_ws147_brass_c6", "brd_ws169_glass_w",
                                           "brd_ws169_glass_h"])
        self.assertEqual((reg.facts["brd_xiao_w_measured"].value,
                          reg.facts["brd_xiao_w_measured"].line), (17.8, 84))
        self.assertEqual(reg.facts["brd_stack_sock_measured"].value, 6.5)
        self.assertEqual(reg.facts["brd_stack_sock_unmeasured"].value, 11.5)
        self.assertEqual(reg.facts["brd_ws169_glass_h"].value, 41.13)
        # the accessors take an argument and are not facts; nor is _brd_find
        for name in ("brd_l", "brd_w", "brd_t", "brd_status", "brd_note", "_brd_find"):
            self.assertNotIn(name, reg.facts)

    def test_malformed_row_fails_naming_the_file(self):
        src = LIB.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as td:
            lib = Path(td) / LIB_NAME
            # a row wrapped differently: its thickness becomes an expression
            bad = src.replace('["ov5647",     24.0,  25.0, 1.0, "spec",',
                              '["ov5647",     24.0,  25.0, 1.0 + 0, "spec",')
            self.assertNotEqual(bad, src)
            lib.write_text(bad, encoding="utf-8")
            with self.assertRaises(gcp.RegistryError) as cm:
                gcp.parse_board_registry(lib)
            self.assertIn("canary_board_lib.scad: parsed 8 of 9 BRD_REGISTRY rows",
                          str(cm.exception))
            self.assertIn("shorter registry", str(cm.exception))
            # a duplicate id
            lib.write_text(src.replace('["heltec_v3",', '["xiao",'), encoding="utf-8")
            with self.assertRaises(gcp.RegistryError) as cm:
                gcp.parse_board_registry(lib)
            self.assertIn('canary_board_lib.scad:59: BRD_REGISTRY row "xiao" is defined twice '
                          '(also line 43)', str(cm.exception))
            # no registry at all, and no file at all
            lib.write_text("function brd_x() = 1;\n", encoding="utf-8")
            with self.assertRaises(gcp.RegistryError) as cm:
                gcp.parse_board_registry(lib)
            self.assertIn("no `BRD_REGISTRY = [`", str(cm.exception))
            lib.unlink()
            with self.assertRaises(gcp.RegistryError) as cm:
                gcp.parse_board_registry(lib)
            self.assertIn("canary_board_lib.scad: the board registry cannot be read",
                          str(cm.exception))

    def test_each_reference_form_resolves(self):
        reg = gcp.parse_board_registry()
        cases = [({"brd": "xiao", "dim": "l"}, 21.0, 'brd_l("xiao")', ":43, spec rung"),
                 ({"brd": "xiao", "dim": "w"}, 17.5, 'brd_w("xiao")', ":43, spec rung"),
                 ({"brd": "xiao", "dim": "t"}, 1.2, 'brd_t("xiao")', ":43, spec rung"),
                 ({"brd": "grove_v2", "dim": "w"}, 20.0, 'brd_w("grove_v2")',
                  ":45, measured rung"),
                 ({"brd": "mr60", "dim": "l"}, 44.0, 'brd_l("mr60")', ":49, spec rung"),
                 ({"brd_fn": "brd_xiao_w_measured"}, 17.8, "brd_xiao_w_measured()", ":84"),
                 ({"brd_fn": "brd_stack_sock_measured"}, 6.5, "brd_stack_sock_measured()",
                  ":90")]
        for ref, want, cite, where in cases:
            value, got_cite, got_where = gcp.resolve_ref(ref, reg)
            self.assertEqual((value, got_cite), (want, cite), ref)
            self.assertEqual(got_where, "canary_board_lib.scad" + where, ref)

    def test_unknown_row_dim_or_fact_and_a_bad_shape_fail_naming_the_registry_file(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda d: d["cad"]["params"].update({
                "board_l": {"brd": "nope", "dim": "l"},
                "board_w": {"brd": "xiao", "dim": "h"},
                "board_h": {"brd_fn": "brd_nope"},
                "board_clear": {"brd": "xiao"},
                "stack_camera": {"brd": "xiao", "dim": "l", "extra": 1},
                "stack_plain": {"brd_fn": "core_wall"},
            }))
            owned, errors = gcp.load_params(root / "devices", root)
        self.assertEqual(len(errors), 6, errors)
        by = {}
        for e in errors:
            self.assertTrue(e.startswith("devices/canary-wap: cad.params."), e)
            self.assertIn("canary_board_lib.scad", e)
            by[e.split("cad.params.", 1)[1].split(" ", 1)[0]] = e
        self.assertIn('references brd_l("nope") but canary_board_lib.scad BRD_REGISTRY has no '
                      'row "nope" (rows: xiao, grove_v2, ov5647', by["board_l"])
        self.assertIn('names dim "h" — a dim is "l"', by["board_w"])
        self.assertIn("references brd_nope() but canary_board_lib.scad defines no numeric "
                      "`function brd_nope() = <number>;` (facts: brd_xiao_w_measured",
                      by["board_h"])
        self.assertIn("is not a registry reference", by["board_clear"])
        self.assertIn("is not a registry reference", by["stack_camera"])
        self.assertIn('names "core_wall" — a fact is a brd_<name>', by["stack_plain"])
        self.assertEqual(owned[WAP_REL], {})          # six refusals, nothing owned
        # a registry that does not parse is ONE error, and the numbers still load
        with _Tree() as root:
            (root / LIB_REL).write_text("// nothing here\n", encoding="utf-8")
            owned, errors = gcp.load_params(root / "devices", root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("no `BRD_REGISTRY = [`", errors[0])
        self.assertIn("no cad.params reference resolves until it parses", errors[0])
        self.assertIn("board_clear", owned[WAP_REL])
        self.assertNotIn("board_l", owned[WAP_REL])


class ManifestsCarryTheJoin(unittest.TestCase):
    def test_the_seventeen_references_each_cite_what_the_knobs_comment_already_says(self):
        owned, errors = gcp.load_params()
        self.assertEqual(errors, [])
        refs = {(Path(s).name, k): o for s, keys in owned.items() for k, o in keys.items()
                if o.ref is not None}
        self.assertEqual({k: o.cite for k, o in refs.items()}, REFS)
        for (scad, knob), o in refs.items():
            # the knob's own line plus its comment-only continuation lines
            lines = (ENC / scad).read_text(encoding="utf-8").splitlines()
            start = gcp.eligible(ENC / scad)[knob]["line"]
            block = [lines[start - 1]]
            for ln in lines[start:]:
                if not ln.strip().startswith("//"):
                    break
                block.append(ln)
            text = "\n".join(block)
            # the accessor or the fact by name — or board_selfcheck(), the
            # registry's own self-check, which is how the Vision module's vm_w
            # comment cites the row that pins it
            fn = o.cite.split("(")[0]
            self.assertTrue(fn in text or "board_selfcheck()" in text,
                            f"{scad}: {knob} references {o.cite} but its comment does not "
                            f"cite the registry:\n{text}")
            self.assertRegex(o.where, r"^canary_board_lib\.scad:\d+")
            # and the resolved number is the file's literal (the fixed point above)
            self.assertEqual(float(o.value), float(gcp.eligible(ENC / scad)[knob]["default"]))

    def test_a_knob_whose_comment_does_not_cite_the_registry_stays_a_number(self):
        owned, _ = gcp.load_params()
        by_name = {Path(s).name: keys for s, keys in owned.items()}
        for scad, knobs in NUMBERS.items():
            for knob in knobs:
                o = by_name[scad][knob]
                self.assertIsNone(o.ref, (scad, knob))
                self.assertEqual((o.cite, o.where), ("", ""), (scad, knob))
        # in particular the two coincidental equalities the first draft listed
        self.assertEqual(by_name["canary_wap_enclosure.scad"]["board_h"].value, 1.2)
        self.assertEqual(by_name["canary_sense_enclosure.scad"]["pcb_t"].value, 1.0)
        self.assertEqual(len(REFS) + sum(len(v) for v in NUMBERS.values()),
                         sum(len(k) for k in owned.values()))

    def test_the_xiao_width_decision_is_which_registry_entry_the_manifest_names(self):
        owned, _ = gcp.load_params()
        sense, vision, wap = owned[SENSE_REL], owned[VISION_REL], owned[WAP_REL]
        self.assertEqual((sense["xiao_w"].cite, sense["xiao_w"].value), ('brd_w("xiao")', 17.5))
        self.assertEqual((wap["board_w"].cite, wap["board_w"].value), ('brd_w("xiao")', 17.5))
        self.assertEqual((vision["xiao_w"].cite, vision["xiao_w"].value),
                         ("brd_xiao_w_measured()", 17.8))
        self.assertIn("spec rung", sense["xiao_w"].where)

    def test_unreferenced_registry_entries_are_info_not_errors(self):
        owned, _ = gcp.load_params()
        rows, facts = gcp.unreferenced(owned, gcp.parse_board_registry())
        self.assertEqual(rows, ["ws147", "ws169", "round_disp", "heltec_v3"])
        self.assertEqual(facts, ["brd_stack_sock_unmeasured", "brd_ws147_brass_c3",
                                 "brd_ws147_brass_c6", "brd_ws169_glass_w", "brd_ws169_glass_h"])
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gcp.main(["--check"]), 0)
        text = out.getvalue()
        self.assertIn("INFO: registry entries no manifest references", text)
        self.assertIn("rows: ws147, ws169, round_disp, heltec_v3", text)
        self.assertIn("not an error", text)


class ARegistryCorrectionReachesTheCases(unittest.TestCase):
    def test_a_mutated_row_moves_exactly_the_referencing_lines(self):
        with _Tree() as root:
            lib_after = edit_lib(root, XIAO_ROW, XIAO_ROW.replace("21.0", "21.4"))
            errors = gcp.check(root / "devices", root)
            self.assertEqual(sorted(e.split(" ", 1)[0] for e in errors),
                             ["canary_sense_enclosure.scad:92:", "canary_vision_enclosure.scad:120:",
                              "canary_wap_enclosure.scad:153:"], errors)
            for e in errors:
                self.assertIn('references brd_l("xiao") (canary_board_lib.scad:43, spec rung): '
                              "registry says 21.4, file says 21.0", e)
                self.assertIn("gen_cad_params.py", e)
            written, werr = gcp.write(root / "devices", root)
            self.assertEqual(werr, [])
            self.assertEqual(sorted((p.name, c.line, c.name, c.old_token, c.new_token)
                                    for p, c in written),
                             [("canary_sense_enclosure.scad", 92, "xiao_l", "21.0", "21.4"),
                              ("canary_vision_enclosure.scad", 120, "xiao_l", "21.0", "21.4"),
                              ("canary_wap_enclosure.scad", 153, "board_l", "21.0", "21.4")])
            self.assertEqual(gcp.check(root / "devices", root), [])
            self.assertEqual(moved_lines("canary_wap_enclosure.scad", root), [153])
            self.assertEqual(moved_lines("canary_vision_enclosure.scad", root), [120])
            self.assertEqual(moved_lines("canary_sense_enclosure.scad", root), [92])
            # the doorbell cites the same row by comment and has no manifest
            self.assertEqual(moved_lines("canary_vision_doorbell.scad", root), [])
            # the registry was read, never written
            self.assertEqual((root / LIB_REL).read_text(encoding="utf-8"), lib_after)

    def test_a_mutated_fact_moves_only_the_lines_that_name_it(self):
        with _Tree() as root:
            # the seated-stack fact: named by the Vision and the Sense, not the WAP
            edit_lib(root, "function brd_stack_sock_measured()   = 6.5;",
                     "function brd_stack_sock_measured()   = 6.2;")
            errors = gcp.check(root / "devices", root)
            self.assertEqual(sorted(e.split(" ", 1)[0] for e in errors),
                             ["canary_sense_enclosure.scad:94:", "canary_vision_enclosure.scad:122:"],
                             errors)
            for e in errors:
                self.assertIn("references brd_stack_sock_measured() (canary_board_lib.scad:90): "
                              "registry says 6.2, file says 6.5", e)
            written, werr = gcp.write(root / "devices", root)
            self.assertEqual(werr, [])
            self.assertEqual(sorted((p.name, c.line) for p, c in written),
                             [("canary_sense_enclosure.scad", 94),
                              ("canary_vision_enclosure.scad", 122)])
            self.assertEqual(moved_lines("canary_wap_enclosure.scad", root), [])
        with _Tree() as root:
            # the measured XIAO width: the Vision pins name it; the WAP and
            # Sense clips name the spec row, so their 17.5 stays put
            edit_lib(root, "function brd_xiao_w_measured() = 17.8;",
                     "function brd_xiao_w_measured() = 17.9;")
            errors = gcp.check(root / "devices", root)
            self.assertEqual([e.split(" ", 1)[0] for e in errors],
                             ["canary_vision_enclosure.scad:121:"], errors)
            self.assertIn("registry says 17.9, file says 17.8", errors[0])
            written, _ = gcp.write(root / "devices", root)
            self.assertEqual([(p.name, c.line, c.new_token) for p, c in written],
                             [("canary_vision_enclosure.scad", 121, "17.9")])
            self.assertEqual(moved_lines("canary_wap_enclosure.scad", root), [])
            self.assertEqual(moved_lines("canary_sense_enclosure.scad", root), [])

    def test_write_after_a_registry_change_is_idempotent(self):
        with _Tree() as root:
            edit_lib(root, XIAO_ROW, XIAO_ROW.replace("17.5", "17.6"))
            first, _ = gcp.write(root / "devices", root)
            self.assertEqual(sorted((p.name, c.line) for p, c in first),
                             [("canary_sense_enclosure.scad", 93),
                              ("canary_wap_enclosure.scad", 154)])
            self.assertEqual(gcp.write(root / "devices", root), ([], []))
            self.assertEqual(gcp.check(root / "devices", root), [])


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

    def test_reference_on_a_string_knob_is_a_type_mismatch(self):
        # a reference always resolves to a number; a string knob cannot take one
        with _Tree() as root:
            edit(root, "canary-sense", lambda d: d["cad"]["params"].__setitem__(
                "radar", {"brd": "mr60", "dim": "l"}))
            errors = gcp.check(root / "devices", root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("cad.params.radar", errors[0])
        # the selector refusal comes first — it is the more useful message
        self.assertIn("is a selector", errors[0])

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
        # the Vision's side is a reference: shown as written, with what it resolved to
        for needle in ("cad.params.xiao_l", "devices/canary-vision ", "devices/canary-vision-devkit",
                       '{"brd": "xiao", "dim": "l"} (= brd_l("xiao") = 21.0)', "22.0",
                       "must agree"):
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
        self.assertTrue(any("board_l = [21] is not a number, string, boolean or registry "
                            "reference" in e for e in errors), errors)

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
            self.assertEqual(moved_lines("canary_wap_enclosure.scad", root), [154])
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

    def test_an_unresolvable_reference_anywhere_writes_nothing_anywhere(self):
        with _Tree() as root:
            edit_lib(root, XIAO_ROW, XIAO_ROW.replace("21.0", "21.4"))      # a real change
            edit(root, "canary-sense", lambda d: d["cad"]["params"].__setitem__(
                "pcb_t", {"brd": "mr60", "dim": "h"}))                     # and a bad ref
            before = {p.name: p.read_bytes()
                      for p in (root / "docs/hardware/enclosure").glob("*.scad")}
            written, errors = gcp.write(root / "devices", root)
            self.assertEqual(written, [])
            self.assertTrue(any('cad.params.pcb_t' in e and 'names dim "h"' in e
                                for e in errors), errors)
            after = {p.name: p.read_bytes()
                     for p in (root / "docs/hardware/enclosure").glob("*.scad")}
        self.assertEqual(after, before)


class DryRun(unittest.TestCase):
    """--dry-run: the diff a manifest edit would write, nothing written."""

    def _run(self, *specs):
        before = {p: p.read_bytes() for p in ENC.glob("*.scad")}
        before.update({p: p.read_bytes() for p in DEVICES.glob("*/device.json")})
        args = []
        for spec in specs:
            args += ["--dry-run", spec]
        with redirect_stdout(io.StringIO()) as out:
            code = gcp.main(args)
        after = {p: p.read_bytes() for p in before}
        self.assertEqual(after, before, "a dry run wrote something")
        return code, out.getvalue()

    def test_parse(self):
        self.assertEqual(gcp.parse_dry_run("canary-wap:board_w=17.8"), ("canary-wap", "board_w", 17.8))
        self.assertEqual(gcp.parse_dry_run("canary-wap:board_w=18"), ("canary-wap", "board_w", 18))
        self.assertEqual(gcp.parse_dry_run('canary-wap:board_w={"brd_fn":"brd_xiao_w_measured"}'),
                         ("canary-wap", "board_w", {"brd_fn": "brd_xiao_w_measured"}))
        self.assertEqual(gcp.parse_dry_run("canary-sense:radar=fda2"), ("canary-sense", "radar", "fda2"))
        self.assertEqual(gcp.parse_dry_run('canary-sense:radar="fda2"'), ("canary-sense", "radar", "fda2"))
        self.assertEqual(gcp.parse_dry_run("x:b=true"), ("x", "b", True))
        for bad in ("board_w=17.8", "canary-wap:board_w", "canary-wap:=1", "Canary:board_w=1", ""):
            with self.assertRaises(ValueError, msg=bad):
                gcp.parse_dry_run(bad)

    def test_a_number_prints_the_one_line_diff_and_writes_nothing(self):
        code, out = self._run("canary-wap:board_w=17.8")
        self.assertEqual(code, 0)
        self.assertIn("devices/canary-wap cad.params.board_w = 17.8", out)
        self.assertIn("would write (nothing written)", out)
        self.assertIn("--- a/docs/hardware/enclosure/canary_wap_enclosure.scad", out)
        self.assertIn("+++ b/docs/hardware/enclosure/canary_wap_enclosure.scad", out)
        self.assertIn("-board_w        = 17.5;  // PCB width (along Y)", out)
        self.assertIn("+board_w        = 17.8;  // PCB width (along Y)", out)
        self.assertEqual(out.count("\n-board_w"), 1)
        self.assertEqual(out.count("\n+board_w"), 1)
        self.assertIn("1 knob(s) on 1 line(s) in 1 file(s)", out)
        self.assertIn("owes PNG previews of every part of: canary_wap_enclosure.scad", out)
        self.assertIn("scripts/regen_cad.py --previews", out)

    def test_a_reference_shows_what_it_resolves_to(self):
        code, out = self._run('canary-wap:board_w={"brd_fn":"brd_xiao_w_measured"}')
        self.assertEqual(code, 0)
        self.assertIn('cad.params.board_w = {"brd_fn": "brd_xiao_w_measured"} '
                      "(= brd_xiao_w_measured() = 17.8)", out)
        self.assertIn("+board_w        = 17.8;", out)

    def test_several_edits_diff_every_file_they_touch(self):
        code, out = self._run("canary-wap:board_w=17.8", "canary-sense:xiao_l=21.4")
        self.assertEqual(code, 0)
        self.assertIn("--- a/docs/hardware/enclosure/canary_sense_enclosure.scad", out)
        self.assertIn("--- a/docs/hardware/enclosure/canary_wap_enclosure.scad", out)
        self.assertIn("+xiao_l   = 21.4;", out)
        self.assertIn("2 knob(s) on 2 line(s) in 2 file(s)", out)
        self.assertIn("canary_sense_enclosure.scad, canary_wap_enclosure.scad", out)

    def test_no_change_says_so(self):
        code, out = self._run("canary-wap:board_w=17.5", "canary-wap:board_l=21")
        self.assertEqual(code, 0)
        self.assertIn("no change", out)
        self.assertIn("already equals the .scad literal(s)", out)
        self.assertNotIn("---", out)

    def test_refusals_are_the_writes_refusals(self):
        code, out = self._run("canary-wap:preset=compact_plain")
        self.assertEqual(code, 1)
        self.assertIn("refused — 1 problem(s)", out)
        self.assertIn("cad.params.preset (devices/canary-wap) is a selector", out)
        self.assertNotIn("---", out)
        code, out = self._run("canary-vision-devkit:xiao_l=22")
        self.assertEqual(code, 1)
        self.assertIn("must agree on a shared key", out)
        code, out = self._run("canary-wap:board_w=17.8", "nope:x=1")
        self.assertEqual(code, 1)
        self.assertIn("devices/nope: no such manifest", out)
        self.assertNotIn("+board_w", out)               # one refusal, no diff at all
        code, out = self._run("canary-wap:board_stack_h=9")
        self.assertEqual(code, 1)
        self.assertIn("is not a literal Customizer knob", out)
        code, out = self._run("canary-wap:board_w=x")
        self.assertEqual(code, 1)
        self.assertIn("is a string in the manifest but the knob is a number", out)

    def test_a_manifest_without_a_case_cannot_be_dry_run(self):
        # the Glance AMOLED has a flasher product and no cad block
        code, out = self._run("canary-display-amoled241:x=1")
        self.assertEqual(code, 1)
        self.assertIn("devices/canary-display-amoled241: cad.params without a cad.scad", out)

    def test_dry_run_api_returns_the_diff_the_changes_and_the_owned_map(self):
        diff, changes, errors, owned = gcp.dry_run([("canary-wap", "board_w", 17.8)])
        self.assertEqual(errors, [])
        self.assertEqual([(p.name, c.line, c.old_token, c.new_token) for p, c in changes],
                         [("canary_wap_enclosure.scad", 154, "17.5", "17.8")])
        self.assertTrue(diff.startswith("--- a/docs/hardware/enclosure/canary_wap_enclosure.scad\n"))
        self.assertEqual(owned[WAP_REL]["board_w"].value, 17.8)
        self.assertIsNone(owned[WAP_REL]["board_w"].ref)
        # and nothing else in the owned map moved
        base, _ = gcp.load_params()
        self.assertEqual({k: o.value for k, o in owned[SENSE_REL].items()},
                         {k: o.value for k, o in base[SENSE_REL].items()})

    def test_bad_shape_and_check_are_parser_errors(self):
        for args in (["--dry-run", "bad"], ["--check", "--dry-run", "a:b=1"]):
            with redirect_stdout(io.StringIO()), \
                    __import__("unittest.mock").mock.patch.object(sys, "stderr", io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    gcp.main(args)
            self.assertEqual(cm.exception.code, 2, args)


if __name__ == "__main__":
    sys.exit(unittest.main())
