#!/usr/bin/env python3
"""Pins docs/hardware/enclosure/gen_cad_params.py — the generator that lets a
device manifest OWN the board knobs of its case.

What is pinned and why:
  • the committed tree is a fixed point: check() is empty, the plan holds no
    change, and every owned case renders to its own bytes — the mechanism
    landed with zero .scad bytes moved, the reference conversion moved zero
    more, widening to the five display cases moved zero more, and this keeps
    all three provable. write() is only ever called inside a scratch copy of
    the tree (_Tree): with default arguments it would rewrite the real released
    cases the moment a manifest disagreed with one;
  • a changed value moves exactly one line and only its literal token: the
    help comment, the indent and every other line are byte-identical, and
    writing twice is the same as writing once (idempotence);
  • the eligibility set IS gen_builder_manifest.parse_scad's: with_lines=True
    points at the assignment it accepted and changes nothing else about its
    output, so a same-named local of a column-0 module can never be hit
    (parse_scad stops at the first `^module`);
  • the board registry (canary_board_lib.scad) parses completely — nine rows,
    seven facts — and a row that drifts from the literal shape is a failure,
    never a shorter registry; each reference form resolves; an unknown row,
    dim or fact fails naming the manifest and the library;
  • a reference is declared only where the knob's help comment already cites
    the registry (decision 4): the twenty-nine references are enumerated here
    with what each cites (the released cases' seventeen, the display cases'
    twelve — the Touch 1.69's glass pair crosswise, as its comment says), the
    knobs that merely equal a row stay numbers, and a mutated registry value
    moves exactly the lines that reference it — a case that cites the same
    row by comment but owns nothing (the C6 display) does not move;
  • the display cases no manifest can own yet are refused by construction,
    not by omission: the C6's `model` ternary, the 1.69's two-knob line, the
    7" frame's panel-library reads;
  • every refusal fires by name — a selector, a computed / [Hidden] knob, a
    type mismatch, a two-statement line, a knob assigned twice, a non-scalar
    value, a malformed reference, two manifests disagreeing on a shared key
    — and a refusal anywhere means nothing is written anywhere.

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
# The display cases a manifest owns knobs of (wave 3 widened to them with zero
# .scad bytes moved), and the one that cites the registry by comment only —
# its board_l / board_w are a `model` ternary the generator refuses.
DISPLAY = ["canary_c3_lcd147.scad", "canary_s3_lcd147.scad", "canary_watch_station.scad",
           "canary_dash_display.scad", "canary_s3_touch169.scad"]
C6 = "canary_c6_display.scad"
OWNED = sorted(["canary_sense_enclosure.scad", "canary_vision_enclosure.scad",
                "canary_wap_enclosure.scad", *DISPLAY])
LIB_NAME = "canary_board_lib.scad"
LIB = ENC / LIB_NAME
LIB_REL = "docs/hardware/enclosure/" + LIB_NAME
WAP = ENC / "canary_wap_enclosure.scad"
WAP_REL = "docs/hardware/enclosure/canary_wap_enclosure.scad"
VISION_REL = "docs/hardware/enclosure/canary_vision_enclosure.scad"
SENSE_REL = "docs/hardware/enclosure/canary_sense_enclosure.scad"
TOUCH169_REL = "docs/hardware/enclosure/canary_s3_touch169.scad"
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
    # the display cases: the two 1.47 sticks share the ws147 family row; the
    # watch's disc names the measured round_disp row; the 1.69's PCB names
    # the ws169 row and its glass slab the brd_ws169_glass_*() facts —
    # CROSSWISE, because the registry's axes follow the board's along-USB
    # length, which is that case's Y (the file's group comment says so)
    ("canary_c3_lcd147.scad", "board_l"): 'brd_l("ws147")',
    ("canary_c3_lcd147.scad", "board_w"): 'brd_w("ws147")',
    ("canary_c3_lcd147.scad", "pcb_t"): 'brd_t("ws147")',
    ("canary_s3_lcd147.scad", "board_l"): 'brd_l("ws147")',
    ("canary_s3_lcd147.scad", "board_w"): 'brd_w("ws147")',
    ("canary_s3_lcd147.scad", "pcb_t"): 'brd_t("ws147")',
    ("canary_watch_station.scad", "disc_d"): 'brd_l("round_disp")',
    ("canary_s3_touch169.scad", "glass_w"): "brd_ws169_glass_h()",
    ("canary_s3_touch169.scad", "glass_h"): "brd_ws169_glass_w()",
    ("canary_s3_touch169.scad", "pcb_w"): 'brd_w("ws169")',
    ("canary_s3_touch169.scad", "pcb_h"): 'brd_l("ws169")',
    ("canary_s3_touch169.scad", "pcb_t"): 'brd_t("ws169")',
}
NUMBERS = {
    "canary_wap_enclosure.scad": ["board_h", "board_clear", "stack_camera", "stack_plain"],
    "canary_vision_enclosure.scad": ["xiao_below", "vm_front_h", "board_clear", "stack_h"],
    "canary_sense_enclosure.scad": ["xiao_below", "vm_front_h", "ant_h", "pcb_t", "board_clear",
                                    "xiao_usb_z"],
    # measured stack numbers with no registry home
    "canary_watch_station.scad": ["disc_t", "disp_back", "xiao_t"],
    # the 4.3 panel has no registry row: MEASURE placeholders, owned as the
    # numbers they are today — documented, not blessed
    "canary_dash_display.scad": ["panel_l", "panel_w", "glass_t", "stack_t"],
    "canary_s3_touch169.scad": ["glass_t", "r_glass", "aa_w", "aa_h"],
}
XIAO_ROW = '["xiao",       21.0,  17.5, 1.2, "spec",'
WS147_ROW = '["ws147",      36.37, 20.32, 1.6, "drawing",'


def owned_scads() -> list[str]:
    """The file name of every .scad a manifest with cad.params names — read
    from the manifests, so a display manifest gaining params is copied into
    the scratch tree by construction instead of breaking every _Tree test."""
    names = set()
    for path in DEVICES.glob("*/device.json"):
        cad = json.loads(path.read_text(encoding="utf-8")).get("cad") or {}
        if "params" in cad and isinstance(cad.get("scad"), str):
            names.add(Path(cad["scad"]).name)
    return sorted(names)


class _Tree:
    """A scratch repo: devices/ copied, and under the same relative path every
    .scad a manifest owns knobs of (owned_scads()), the released set (the
    doorbell has no manifest and must stay untouched) and the board registry
    (a cad.params reference resolves from it) — so write() can be exercised
    without touching the real sources."""

    def __enter__(self) -> Path:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        shutil.copytree(DEVICES, root / "devices")
        enc = root / "docs" / "hardware" / "enclosure"
        enc.mkdir(parents=True)
        for name in sorted(set(owned_scads()) | set(RELEASED) | set(DISPLAY) | {C6, LIB_NAME}):
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


def _linter():
    """scripts/lint_device_manifests.py, loaded by path the way its own tests
    load it — the gate that imports check(), so a refusal here is proven to
    come back through it as a message."""
    script = REPO / "scripts" / "lint_device_manifests.py"
    if str(script.parent) not in sys.path:
        sys.path.insert(0, str(script.parent))
    spec = importlib.util.spec_from_file_location("lint_device_manifests", script)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


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
        self.assertEqual(sorted(Path(s).name for s in owned), OWNED)
        for scad_rel, keys in owned.items():
            path = REPO / scad_rel
            r = gcp.render(path, {k: o.value for k, o in keys.items()})
            self.assertEqual(r.errors, [], scad_rel)
            self.assertEqual(r.changes, [], scad_rel)
            self.assertEqual(r.text, path.read_bytes().decode("utf-8"), scad_rel)
        # the four released cases: the doorbell has no manifest, so nothing is
        # owned there and a render with nothing owned is the file itself
        for name in RELEASED:
            r = gcp.render(ENC / name, {})
            self.assertEqual((r.changes, r.errors), ([], []), name)
            self.assertEqual(r.text, (ENC / name).read_bytes().decode("utf-8"), name)

    def test_the_scratch_tree_copies_every_owned_case(self):
        # every released case but the doorbell (no manifest names it), plus
        # the five display cases that joined with pure JSON
        self.assertEqual(owned_scads(),
                         sorted((set(RELEASED) - {"canary_vision_doorbell.scad"}) | set(DISPLAY)))
        with _Tree() as root:
            enc = root / "docs/hardware/enclosure"
            for name in sorted(set(owned_scads()) | set(RELEASED) | set(DISPLAY) | {C6, LIB_NAME}):
                self.assertEqual((enc / name).read_bytes(), (ENC / name).read_bytes(), name)
            self.assertEqual(gcp.check(root / "devices", root), [])

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

    def test_write_mode_would_write_nothing_on_the_committed_tree(self):
        # NEVER gcp.write() with default arguments from a test: the moment a
        # manifest disagrees with its .scad, that rewrites the real released
        # cases. The fixed point is asserted on the plan — the whole run short
        # of writing — and then write() runs inside a scratch copy of the tree.
        plan, errors, owned = gcp._plan(DEVICES, REPO)
        self.assertEqual(errors, [])
        self.assertEqual(sorted(p.scad_rel for p in plan), sorted(owned))
        for scad_rel, path, r in plan:
            self.assertEqual(r.changes, [], scad_rel)
            self.assertEqual(r.text, path.read_bytes().decode("utf-8"), scad_rel)
        with _Tree() as root:
            enc = root / "docs/hardware/enclosure"
            before = {p.name: p.read_bytes() for p in enc.glob("*.scad")}
            self.assertEqual(gcp.write(root / "devices", root), ([], []))
            self.assertEqual({p.name: p.read_bytes() for p in enc.glob("*.scad")}, before)

    def test_cli_check_exit_code(self):
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gcp.main(["--check"]), 0)
        self.assertIn("54 manifest-owned knobs across 8 case file(s)", out.getvalue())
        self.assertIn("(29 of them resolved from canary_board_lib.scad)", out.getvalue())


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

    def test_a_commented_out_row_is_not_a_row_and_the_count_guard_keeps_its_meaning(self):
        # OpenSCAD sees neither a `//` row nor a `/* */` one; the row regex and
        # the row-start count used to see both, and a reference to a dead row
        # resolved to its stale number
        src = LIB.read_text(encoding="utf-8")
        heltec = ('    ["heltec_v3",  51.0,  26.0, 1.2, "spec",\n'
                  '     "Heltec WiFi LoRa 32 V3 — the solar relay pod\'s radio board"],\n')
        self.assertEqual(src.count(heltec), 1)
        with tempfile.TemporaryDirectory() as td:
            lib = Path(td) / LIB_NAME
            # both lines of the row behind `//`
            lib.write_text(src.replace(heltec, "".join("    // " + ln.lstrip() + "\n"
                                                       for ln in heltec.splitlines())),
                           encoding="utf-8")
            reg = gcp.parse_board_registry(lib)
            self.assertEqual(len(reg.rows), 8)
            self.assertNotIn("heltec_v3", reg.rows)
            with self.assertRaises(gcp.RefError) as cm:
                gcp.resolve_ref({"brd": "heltec_v3", "dim": "l"}, reg)
            self.assertIn('has no row "heltec_v3"', str(cm.exception))
            self.assertEqual(reg.rows["xiao"].line, 43)                # line numbers are the file's
            # the same row inside a block comment
            lib.write_text(src.replace(heltec, "    /*\n" + heltec + "    */\n"), encoding="utf-8")
            reg = gcp.parse_board_registry(lib)
            self.assertEqual(len(reg.rows), 8)
            self.assertNotIn("heltec_v3", reg.rows)
            # a row-shaped `//` comment inside the region is neither a row nor a start
            ghost = '    // ["ghost", 1.0, 2.0, 3.0, "spec", "not a row"],\n'
            lib.write_text(src.replace(heltec, ghost + heltec), encoding="utf-8")
            reg = gcp.parse_board_registry(lib)
            self.assertEqual(len(reg.rows), 9)
            self.assertNotIn("ghost", reg.rows)
            self.assertEqual(reg.rows["heltec_v3"].line, 60)            # shifted by the one line
            # the count guard still catches drift: 8 live rows, one of them drifted
            lib.write_text(src.replace(heltec, "".join("    // " + ln.lstrip() + "\n"
                                                       for ln in heltec.splitlines()))
                           .replace('["ov5647",     24.0,  25.0, 1.0, "spec",',
                                    '["ov5647",     24.0,  25.0, 1.0 + 0, "spec",'),
                           encoding="utf-8")
            with self.assertRaises(gcp.RegistryError) as cm:
                gcp.parse_board_registry(lib)
            self.assertIn("parsed 7 of 8 BRD_REGISTRY rows", str(cm.exception))
            # a `BRD_REGISTRY = [` mentioned in a comment above the real one is prose
            lib.write_text("// BRD_REGISTRY = [ … ]; is below\n" + src, encoding="utf-8")
            reg = gcp.parse_board_registry(lib)
            self.assertEqual((len(reg.rows), reg.rows["xiao"].line), (9, 44))

    def test_a_fact_in_a_comment_is_not_a_fact(self):
        src = LIB.read_text(encoding="utf-8")
        live = "function brd_xiao_w_measured() = 17.8;"
        self.assertEqual(src.count(live), 1)
        with tempfile.TemporaryDirectory() as td:
            lib = Path(td) / LIB_NAME
            lib.write_text(src.replace(live, "/* function brd_xiao_w_measured_old() = 17.5; */\n"
                                       "// function brd_ghost() = 1;\n" + live), encoding="utf-8")
            reg = gcp.parse_board_registry(lib)
            self.assertNotIn("brd_xiao_w_measured_old", reg.facts)
            self.assertNotIn("brd_ghost", reg.facts)
            self.assertEqual((reg.facts["brd_xiao_w_measured"].value,
                              reg.facts["brd_xiao_w_measured"].line), (17.8, 86))
            self.assertEqual(list(reg.facts)[:2], ["brd_xiao_w_measured", "brd_stack_sock_measured"])
            # a multi-line block comment holding a fact, and a `//` inside a string
            lib.write_text(src.replace(live, "/*\n   function brd_stale() = 1;\n*/\n" + live)
                           .replace('"OV5647 camera carrier, Pi-cam v1.3 form"',
                                    '"OV5647 carrier // Pi-cam v1.3 form"'), encoding="utf-8")
            reg = gcp.parse_board_registry(lib)
            self.assertNotIn("brd_stale", reg.facts)
            self.assertEqual(reg.rows["ov5647"].note, "OV5647 carrier // Pi-cam v1.3 form")
            self.assertEqual(len(reg.rows), 9)
        # the stripper itself: newlines and strings kept, comments blanked
        self.assertEqual(gcp._strip_comments('a = 1; // x\nb = "//"; /* c\nd */ e = 2;'),
                         'a = 1;     \nb = "//";     \n     e = 2;')

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
    def test_every_reference_cites_what_the_knobs_comment_already_says(self):
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
        # the solar relay pod's radio board has no case file; the seated-stack
        # guess is the doorbell's; the brass pillar facts are cited by comment
        # in the C3 (its own MEASURED 3.0) and the C6 (which owns nothing yet)
        self.assertEqual(rows, ["heltec_v3"])
        self.assertEqual(facts, ["brd_stack_sock_unmeasured", "brd_ws147_brass_c3",
                                 "brd_ws147_brass_c6"])
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gcp.main(["--check"]), 0)
        text = out.getvalue()
        self.assertIn("INFO: registry entries no manifest references", text)
        self.assertIn("rows: heltec_v3; facts: brd_stack_sock_unmeasured, brd_ws147_brass_c3, "
                      "brd_ws147_brass_c6", text)
        self.assertIn("not an error", text)
        # the display cases HAVE manifests (canary-display-*/device.json names
        # each case) — they own no knobs yet; the INFO must not say otherwise
        self.assertIn("Cases whose manifests own no knobs yet (the C6 display, the 7\" frame)", text)
        self.assertIn("cases with no manifest (the doorbell", text)
        self.assertNotIn("Cases no manifest owns", text)
        display = [p for p in DEVICES.glob("canary-display-*/device.json")
                   if (json.loads(p.read_text(encoding="utf-8")).get("cad") or {}).get("scad")]
        self.assertGreater(len(display), 0)


class DisplayCasesJoinTheSameChain(unittest.TestCase):
    """Wave 3 widened cad.params to the display cases with pure JSON — the
    same generator, the same registry, zero .scad bytes — and left the ones
    it cannot own refused, not forgotten."""

    def test_the_touch169_glass_pair_is_crosswise_on_purpose(self):
        owned, _ = gcp.load_params()
        t = owned[TOUCH169_REL]
        # the registry's w/h follow the board's along-USB length (this case's
        # Y), so the case's X-width is the registry's glass HEIGHT and vice
        # versa — the manifest states the join exactly as the file's comment does
        self.assertEqual((t["glass_w"].cite, t["glass_w"].value), ("brd_ws169_glass_h()", 41.13))
        self.assertEqual((t["glass_h"].cite, t["glass_h"].value), ("brd_ws169_glass_w()", 33.13))
        self.assertEqual((t["pcb_w"].cite, t["pcb_w"].value), ('brd_w("ws169")', 37.12))
        self.assertEqual((t["pcb_h"].cite, t["pcb_h"].value), ('brd_l("ws169")', 29.83))
        self.assertEqual((t["pcb_t"].cite, t["pcb_t"].value), ('brd_t("ws169")', 1.6))
        self.assertIn("crosswise", (ENC / "canary_s3_touch169.scad").read_text(encoding="utf-8"))
        for knob in ("glass_w", "glass_h", "pcb_w", "pcb_h", "pcb_t"):
            self.assertEqual(t[knob].slugs, ["canary-display-touch169"])

    def test_the_two_147_sticks_share_the_ws147_row(self):
        owned, _ = gcp.load_params()
        for name, slug in [("canary_c3_lcd147.scad", "canary-display-nightlight-c3"),
                           ("canary_s3_lcd147.scad", "canary-display-nightstand-s3")]:
            keys = owned["docs/hardware/enclosure/" + name]
            self.assertEqual(sorted(keys), ["board_l", "board_w", "pcb_t"])
            self.assertEqual({k: (o.cite, o.value, o.slugs) for k, o in keys.items()},
                             {"board_l": ('brd_l("ws147")', 36.37, [slug]),
                              "board_w": ('brd_w("ws147")', 20.32, [slug]),
                              "pcb_t": ('brd_t("ws147")', 1.6, [slug])})
            for o in keys.values():
                self.assertIn("drawing rung", o.where)

    def test_a_mutated_ws147_row_moves_both_sticks_and_not_the_c6(self):
        with _Tree() as root:
            lib_after = edit_lib(root, WS147_ROW, WS147_ROW.replace("36.37", "36.4"))
            errors = gcp.check(root / "devices", root)
            self.assertEqual(sorted(e.split(" ", 1)[0] for e in errors),
                             ["canary_c3_lcd147.scad:249:", "canary_s3_lcd147.scad:106:"], errors)
            for e in errors:
                self.assertIn('references brd_l("ws147") (canary_board_lib.scad:53, drawing rung): '
                              "registry says 36.4, file says 36.37", e)
            written, werr = gcp.write(root / "devices", root)
            self.assertEqual(werr, [])
            self.assertEqual(sorted((p.name, c.line, c.name, c.old_token, c.new_token)
                                    for p, c in written),
                             [("canary_c3_lcd147.scad", 249, "board_l", "36.37", "36.4"),
                              ("canary_s3_lcd147.scad", 106, "board_l", "36.37", "36.4")])
            self.assertEqual(gcp.check(root / "devices", root), [])
            self.assertEqual(moved_lines("canary_c3_lcd147.scad", root), [249])
            self.assertEqual(moved_lines("canary_s3_lcd147.scad", root), [106])
            # the C6 cites the same row by comment; its board_l is a `model`
            # ternary no manifest owns, so a registry correction does NOT reach
            # it — that is the honest leftover, visible as a line that stays put
            self.assertEqual(moved_lines(C6, root), [])
            for name in ("canary_s3_touch169.scad", "canary_watch_station.scad",
                         "canary_dash_display.scad", *RELEASED):
                self.assertEqual(moved_lines(name, root), [], name)
            self.assertEqual((root / LIB_REL).read_text(encoding="utf-8"), lib_after)

    def test_a_mutated_glass_fact_moves_only_the_crosswise_knob_that_names_it(self):
        with _Tree() as root:
            edit_lib(root, "function brd_ws169_glass_h() = 41.13;",
                     "function brd_ws169_glass_h() = 41.2;")
            errors = gcp.check(root / "devices", root)
            self.assertEqual([e.split(" ", 1)[0] for e in errors],
                             ["canary_s3_touch169.scad:63:"], errors)
            self.assertIn("glass_w = 41.13 in the .scad", errors[0])
            self.assertIn("references brd_ws169_glass_h() (canary_board_lib.scad:101): "
                          "registry says 41.2, file says 41.13", errors[0])
            written, _ = gcp.write(root / "devices", root)
            self.assertEqual([(p.name, c.line, c.name, c.new_token) for p, c in written],
                             [("canary_s3_touch169.scad", 63, "glass_w", "41.2")])
            self.assertEqual(moved_lines("canary_s3_touch169.scad", root), [63])

    def test_the_display_cases_no_manifest_owns_are_refused_by_construction(self):
        # the C6: board_l / board_w are a `model` ternary, `model` a selector
        r = gcp.render(ENC / C6, {"board_l": 36.37, "board_w": 20.32, "model": "1.47"})
        self.assertEqual(r.changes, [])
        self.assertEqual(len(r.errors), 3, r.errors)
        self.assertIn("board_l is not a literal Customizer knob", r.errors[0])
        self.assertIn("board_w is not a literal Customizer knob", r.errors[1])
        self.assertIn("model is a selector", r.errors[2])
        # the 7" frame reads its panel record from canary_panel_lib.scad
        r = gcp.render(ENC / "canary_s3_lcd7.scad", {"panel_variant": "lcd7", "PANEL": 1})
        self.assertEqual(r.changes, [])
        self.assertIn("panel_variant is a selector", r.errors[0])
        self.assertIn("PANEL is not a literal Customizer knob", r.errors[1])
        # and their manifests own nothing — the leftovers are listed in
        # devices/README.md, not papered over with a partial
        for slug in ("canary-display-nightstand-c6", "canary-display-dash7",
                     "canary-display-nightstand7"):
            m = json.loads((DEVICES / slug / "device.json").read_text(encoding="utf-8"))
            self.assertNotIn("params", m["cad"], slug)
        # the selectors of the owned display cases are not owned either
        for name, selectors in [("canary_c3_lcd147.scad", ("headers", "port")),
                                ("canary_s3_lcd147.scad", ("headers",))]:
            owned, _ = gcp.load_params()
            keys = owned["docs/hardware/enclosure/" + name]
            for sel in selectors:
                self.assertNotIn(sel, keys)
                self.assertIn("options", gcp.eligible(ENC / name)[sel])


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

    def test_crlf_survives_a_real_edit_and_only_the_token_moves(self):
        # read_text()'s universal newlines turned every CRLF into LF, so one
        # changed token rewrote a CRLF file LF on every line; bytes in, bytes out
        crlf = FIXTURE.replace("\n", "\r\n").encode("utf-8")
        self.assertEqual(crlf.count(b"\r\n"), FIXTURE.count("\n"))
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / "crlf.scad"
            f.write_bytes(crlf)
            r = gcp.render(f, {"n": 6})
            self.assertEqual((r.errors, [(c.line, c.old_token, c.new_token) for c in r.changes]),
                             ([], [(2, "5", "6")]))
            out = r.text.encode("utf-8")
            self.assertEqual(out.count(b"\r\n"), crlf.count(b"\r\n"))
            self.assertEqual(out.count(b"\n"), out.count(b"\r\n"))        # no bare LF crept in
            self.assertEqual(out, crlf.replace(b"n = 5;", b"n = 6;", 1))
            # an unchanged value reproduces the bytes exactly
            self.assertEqual(gcp.render(f, {"n": 5}).text.encode("utf-8"), crlf)
        # and through write(): a CRLF copy of a released case keeps every CRLF
        with _Tree() as root:
            wap = root / WAP_REL
            before = wap.read_bytes().replace(b"\n", b"\r\n")
            wap.write_bytes(before)
            edit(root, "canary-wap", lambda d: d["cad"]["params"].__setitem__("board_w", 17.8))
            written, errors = gcp.write(root / "devices", root)
            self.assertEqual(errors, [])
            self.assertEqual([(c.line, c.old_token, c.new_token) for _, c in written],
                             [(154, "17.5", "17.8")])
            after = wap.read_bytes()
            self.assertEqual(after.count(b"\r\n"), before.count(b"\r\n"))
            self.assertEqual(after.count(b"\n"), after.count(b"\r\n"))
            old, new = before.split(b"\r\n"), after.split(b"\r\n")
            self.assertEqual(len(old), len(new))
            self.assertEqual([i + 1 for i, (a, b) in enumerate(zip(old, new)) if a != b], [154])
            self.assertEqual(gcp.check(root / "devices", root), [])

    def test_module_local_of_the_same_name_is_never_touched(self):
        # a local of a COLUMN-0 module: parse_scad stops at the first `^module`,
        # which is the only module boundary it (and so this generator) knows
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
            self.assertIn("line 69 holds 2 statements", e)
            self.assertIn("split the line first", e)

    def test_a_line_with_two_statements_is_refused_whatever_they_are(self):
        # counted on the code of the line, not on the knobs the parser took
        # from it: `$fn` is skipped by parse_scad and `dep = mixed` is not a
        # literal, so one parsed knob used to mean "one knob line" — and
        # `$fn = 64; shared = 7;` was refused for the wrong reason (could not
        # locate) while `shared2 = 8; $fn = 32;` was rewritten
        src = """\
/* [Boards] */
$fn = 64; shared = 7;   // $fn first
shared2 = 8; $fn = 32;  // $fn after
mixed = 3; dep = mixed; // a literal and a computed value
semi = 5;     // comment with ; semicolons; and x = 9; are not code
sq = "a;b";   // a ; inside a string is not a statement
open = 4;     /* a block comment that opens here; and
               closes on the next line */
module m() {}
"""
        with tempfile.TemporaryDirectory() as td:
            f = fixture(Path(td), src)
            r = gcp.render(f, {"shared": 70, "shared2": 80, "mixed": 30, "semi": 55,
                               "sq": "c;d", "open": 40})
        refused = {e.split("cad.params.", 1)[1].split(":", 1)[0]: e for e in r.errors}
        self.assertEqual(sorted(refused), ["mixed", "shared", "shared2"], r.errors)
        for name, line in (("shared", 2), ("shared2", 3), ("mixed", 4)):
            self.assertIn(f"line {line} holds 2 statements", refused[name])
            self.assertIn("split the line first", refused[name])
        self.assertEqual([(c.line, c.name, c.old_token, c.new_token) for c in r.changes],
                         [(5, "semi", "5", "55"), (6, "sq", '"a;b"', '"c;d"'), (7, "open", "4", "40")])
        self.assertEqual(gcp._statements('a = 1; b = "x;y"; // c; d;'), 2)
        self.assertEqual(gcp._statements('a = 1;   // ; ; ;'), 1)
        self.assertEqual(gcp._statements('a = "\\";"; /* ; */'), 1)

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

    def test_a_non_canonical_cad_scad_is_an_error_never_an_exception(self):
        # `owned` was keyed by the manifest's literal string and check() looked
        # it up by the normalized path: KeyError, and through the linter a
        # traceback in place of the whole manifest gate
        schema = json.loads((DEVICES / "device.schema.json").read_text(encoding="utf-8"))
        self.assertEqual(gcp.SCAD_PATH_RE.pattern,
                         schema["properties"]["cad"]["properties"]["scad"]["pattern"])
        for spelling in ("docs/hardware/enclosure/./canary_wap_enclosure.scad",
                         "docs/hardware/enclosure/../enclosure/canary_wap_enclosure.scad",
                         "./docs/hardware/enclosure/canary_wap_enclosure.scad",
                         "docs/hardware/enclosure/Canary_WAP_enclosure.scad"):
            with _Tree() as root:
                edit(root, "canary-wap", lambda d: d["cad"].__setitem__("scad", spelling))
                owned, errors = gcp.load_params(root / "devices", root)
                self.assertNotIn(WAP_REL, owned, spelling)
                self.assertNotIn(spelling, owned)
                self.assertEqual(len(errors), 1, (spelling, errors))
                self.assertIn(f"devices/canary-wap: cad.scad {json.dumps(spelling)} is not a case "
                              f"file path", errors[0])
                self.assertIn("device.schema.json", errors[0])
                self.assertEqual(gcp.check(root / "devices", root), errors)      # no KeyError
                self.assertEqual(gcp.write(root / "devices", root), ([], errors))
                if spelling.startswith("docs/hardware/enclosure/./"):
                    # and through THE manifest gate, as its own tests drive it:
                    # devices/ scratch, the repo defaulted — a message, not a traceback
                    _, lint_errors = _linter().lint(devices_dir=root / "devices")
                    self.assertTrue(any("is not a case file path" in e for e in lint_errors),
                                    lint_errors)
                    self.assertTrue(any(".cad.scad:" in e and "does not match" in e
                                        for e in lint_errors), lint_errors)   # the schema's, too

    def test_missing_scad_is_reported(self):
        with _Tree() as root:
            (root / WAP_REL).unlink()
            errors = gcp.check(root / "devices", root)
        self.assertTrue(any(WAP_REL in e and "does not exist" in e for e in errors), errors)


class NoSilentPassForAnUnspellableValue(unittest.TestCase):
    """_token used to return None — "the file already says it" — for a value
    whose repr is exponent form, NaN or inf, so 0.00005 passed a --check
    against 0.6 and write() wrote nothing. Now it raises, and render() reports."""

    def test_an_exponent_form_number_is_an_error_not_unchanged(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda d: d["cad"]["params"].__setitem__("board_clear", 1e-5))
            errors = gcp.check(root / "devices", root)
            self.assertEqual(len(errors), 1, errors)
            self.assertIn("canary_wap_enclosure.scad: cad.params.board_clear (devices/canary-wap): "
                          "1e-05 cannot be spelled as a Customizer literal", errors[0])
            self.assertIn("no exponent form", errors[0])
            written, werr = gcp.write(root / "devices", root)
            self.assertEqual((written, werr), ([], errors))
        with tempfile.TemporaryDirectory() as td:
            f = fixture(Path(td))
            for owned in ({"f": 1e-5}, {"n": 0.00005}, {"f": 1e16}, {"f": float("nan")},
                          {"n": float("inf")}, {"n": -float("inf")}):
                r = gcp.render(f, owned)
                self.assertEqual(r.changes, [], owned)
                self.assertEqual(len(r.errors), 1, (owned, r.errors))
                self.assertIn("cannot be spelled as a Customizer literal", r.errors[0])
            # an integer-valued 1e16 against an integer-spelled token has a spelling
            r = gcp.render(f, {"n": 1e16})
            self.assertEqual((r.errors, [(c.old_token, c.new_token) for c in r.changes]),
                             ([], [("5", "10000000000000000")]))
        # the helper raises — it never answers None for these
        for value in (1e-5, 1e16, float("nan"), float("inf"), 10 ** 400):
            with self.assertRaises(gcp.Unspellable, msg=value):
                gcp._token(value, "0.6")
        self.assertIsNone(gcp._token(0.6, "0.6"))

    def test_a_string_holding_a_comment_opener_is_refused_before_it_is_written(self):
        # it used to write fine — and then --check failed as "not a literal
        # knob", because parse_scad splits the line at `//` or `/*` first
        with tempfile.TemporaryDirectory() as td:
            f = fixture(Path(td))
            for value in ("a//b", "a/*b", "//", "/* x */", "http://x"):
                r = gcp.render(f, {"s": value})
                self.assertEqual(r.changes, [], value)
                self.assertEqual(len(r.errors), 1, (value, r.errors))
                self.assertIn(f"cad.params.s: {json.dumps(value)} cannot be spelled as a Customizer "
                              f"literal", r.errors[0])
                self.assertIn("comment opener", r.errors[0])
            # `*/` alone is not an opener: it writes, and re-checks clean
            r = gcp.render(f, {"s": "a*/b"})
            self.assertEqual((r.errors, [(c.old_token, c.new_token) for c in r.changes]),
                             ([], [('"abc"', '"a*/b"')]))
            f.write_text(r.text, encoding="utf-8")
            again = gcp.render(f, {"s": "a*/b"})
            self.assertEqual((again.errors, again.changes), ([], []))
        # (no released case owns a string knob today — the fixture is the only
        # place this refusal can fire; a manifest that named one would reach
        # render() through the same path)

    def test_a_non_finite_number_is_refused_at_load_naming_manifest_and_key(self):
        # Python's json reads NaN and Infinity; a manifest carrying one is
        # refused before anything is rendered, and nan == nan being False can
        # never turn into an equality check that does not settle
        for literal in ("NaN", "Infinity", "-Infinity"):
            with _Tree() as root:
                path = root / "devices" / "canary-wap" / "device.json"
                text = path.read_text(encoding="utf-8")
                self.assertEqual(text.count('"board_clear": 0.6'), 1)
                path.write_text(text.replace('"board_clear": 0.6', f'"board_clear": {literal}'),
                                encoding="utf-8")
                owned, errors = gcp.load_params(root / "devices", root)
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(f"devices/canary-wap: cad.params.board_clear = {literal} is not a "
                              f"finite number", errors[0])
                self.assertNotIn("board_clear", owned[WAP_REL])
                self.assertIn("board_l", owned[WAP_REL])       # the rest still loads
                self.assertEqual(gcp.check(root / "devices", root), errors)
                self.assertEqual(gcp.write(root / "devices", root), ([], errors))


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


if __name__ == "__main__":
    sys.exit(unittest.main())
