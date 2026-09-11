#!/usr/bin/env python3
"""Pins scripts/regen_cad.py — the enclosure regeneration chain as one command.

What is pinned and why (nothing is executed: subprocess.run and shutil.which
are replaced for every test, and a recorder keeps what WOULD have run):
  • the STEPS order is exactly the derived order — a change to it is a
    deliberate edit of this test, because a step run out of order fails with
    a message that names the wrong cause (CLAUDE.md, "the rebuild is
    upstream of the catalog generators");
  • every step maps to a check form: the argv for the ten that have one,
    the regenerate-compare-restore form for gen_enclosures.py (which has no
    --check flag), and `render` alone has none — with the two steps that
    gate its output named in its note;
  • every command names a file that exists in the tree, so a renamed
    generator fails here rather than at the first real edit;
  • the OpenSCAD steps and --previews are refused BEFORE anything runs when
    `openscad` is missing, and the refusal is honest about there being no
    Actions button that renders and pushes STLs;
  • --check runs each check form in order and names the first failure with
    the resume command; --from skips exactly the earlier steps; --site
    reaches gen_builder_manifest.py in write mode only;
  • a full run STOPS (exit 3) after regenerating the sketch mirror when the
    figure headers moved, before gen_flash.py, naming the dist button — and
    continues when they did not;
  • the render log is judged by enclosure.yml's grep, verbatim;
  • the preview plan covers every value of a case's `part` enum with the
    selector sets render.sh uses, both views, through the README's command.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "regen_cad.py"
spec = importlib.util.spec_from_file_location("regen_cad", SCRIPT)
rc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rc)  # type: ignore[union-attr]

ORDER = ["gen_cad_params", "lint_design_lang", "render", "gen_assembled_dims", "gen_figures",
         "gen_device_glbs", "setup_regen", "gen_flash", "gen_builder_manifest", "gen_enclosures",
         "gen_stamp", "gen_mark_svg"]
ENC = "docs/hardware/enclosure"
CHECKS = {
    "gen_cad_params": ("python3", f"{ENC}/gen_cad_params.py", "--check"),
    "lint_design_lang": ("python3", "scripts/lint_design_lang.py"),
    "render": None,
    "gen_assembled_dims": ("python3", f"{ENC}/gen_assembled_dims.py", "--check"),
    "gen_figures": ("node", "canary-local/tools/figures/gen_figures.mjs", "--check"),
    "gen_device_glbs": ("node", "canary-local/tools/figures/gen_device_glbs.mjs", "--check"),
    "setup_regen": ("./setup.sh", "regen"),
    "gen_flash": ("python3", "canary-local/tools/gen_flash.py", "--check"),
    "gen_builder_manifest": ("python3", f"{ENC}/gen_builder_manifest.py", "--check"),
    "gen_enclosures": ("python3", "canary-local/tools/gen_enclosures.py"),
    "gen_stamp": ("python3", f"{ENC}/gen_stamp.py", "--check"),
    "gen_mark_svg": ("python3", f"{ENC}/gen_mark_svg.py", "--check"),
}
HEADERS_MOVED = " M firmware/common/core/fleet_figures.h\n M firmware/common/core/fleet_figures_art.h\n"


class Recorder:
    """A subprocess.run stand-in: records (argv, cwd), answers from rules.
    A rule is (needle, handler): the first rule whose needle is a substring
    of the joined argv answers; handler(argv, cwd) -> (returncode, stdout)
    and may have side effects (a fake generator writing a file)."""

    def __init__(self, rules=()):
        self.rules = list(rules)
        self.calls: list[tuple[list[str], str | None]] = []

    def __call__(self, argv, cwd=None, text=True, stdout=None, stderr=None, **kw):
        argv = list(argv)
        self.calls.append((argv, str(cwd) if cwd else None))
        joined = " ".join(argv)
        for needle, handler in self.rules:
            if needle in joined:
                code, out = handler(argv, cwd)
                return subprocess.CompletedProcess(argv, code, out if stdout else None, None)
        return subprocess.CompletedProcess(argv, 0, "" if stdout else None, None)

    def argvs(self, skip_git=True):
        return [a for a, _ in self.calls if not (skip_git and a[0] == "git")]


def const(code, out=""):
    return lambda argv, cwd: (code, out)


def run_main(args, rules=(), which="/usr/bin/openscad", repo=None):
    """main(args) with subprocess and which() replaced; (exit code, stdout, recorder)."""
    rec = Recorder(rules)
    patches = [mock.patch.object(rc.subprocess, "run", rec),
               mock.patch.object(rc.shutil, "which", lambda name: which)]
    if repo is not None:
        patches.append(mock.patch.object(rc, "REPO", repo))
    with patches[0], patches[1]:
        if repo is not None:
            with patches[2], redirect_stdout(io.StringIO()) as out:
                code = rc.main(args)
        else:
            with redirect_stdout(io.StringIO()) as out:
                code = rc.main(args)
    return code, out.getvalue(), rec


class TheOrderIsTheDerivedOrder(unittest.TestCase):
    def test_steps_in_order(self):
        self.assertEqual(list(rc.STEP_NAMES), ORDER)
        self.assertEqual(rc.RESUME_AFTER_DIST, "gen_flash")
        self.assertEqual(ORDER.index("setup_regen") + 1, ORDER.index("gen_flash"))
        self.assertLess(ORDER.index("gen_assembled_dims"), ORDER.index("gen_figures"))
        self.assertLess(ORDER.index("gen_figures"), ORDER.index("gen_device_glbs"))
        self.assertLess(ORDER.index("gen_figures"), ORDER.index("setup_regen"))
        self.assertLess(ORDER.index("gen_flash"), ORDER.index("gen_builder_manifest"))
        self.assertLess(ORDER.index("gen_builder_manifest"), ORDER.index("gen_enclosures"))

    def test_every_step_maps_to_its_check_form(self):
        self.assertEqual({s.name: s.check for s in rc.STEPS}, CHECKS)
        kinds = {s.name: s.check_kind for s in rc.STEPS}
        self.assertEqual([n for n, k in kinds.items() if k == "none"], ["render"])
        self.assertEqual([n for n, k in kinds.items() if k == "diff"], ["setup_regen", "gen_enclosures"])
        self.assertEqual(rc.STEP_BY_NAME["setup_regen"].outputs,
                         ("firmware/projects/canary-display/arduino/canary_display",))
        render = rc.STEP_BY_NAME["render"]
        self.assertIn("gen_assembled_dims --check", render.note)
        self.assertIn("gen_figures --check", render.note)
        self.assertIn("not deterministic", render.note)
        enclosures = rc.STEP_BY_NAME["gen_enclosures"]
        self.assertEqual(enclosures.outputs, (
            "canary-local/devices/enclosures.json", "canary-local/devices/catalog.json",
            "canary-local/devices/build.json", "canary-local/devices/workshop.json"))
        # the two report-only steps never run a write form
        for name in ("gen_stamp", "gen_mark_svg"):
            s = rc.STEP_BY_NAME[name]
            self.assertTrue(s.report_only)
            self.assertEqual(s.cmd, s.check)
            self.assertIn("--check", s.cmd)
        self.assertEqual([s.name for s in rc.STEPS if s.needs_openscad], ["render", "gen_assembled_dims"])
        self.assertEqual([s.name for s in rc.STEPS if s.conditional], ["setup_regen"])

    def test_every_command_names_a_file_in_the_tree(self):
        for s in rc.STEPS:
            for argv, cwd in ((s.cmd, s.cwd), (s.check or (), s.check_cwd or s.cwd)):
                if not argv:
                    continue
                target = argv[1] if argv[0] in ("python3", "node") else argv[0]
                path = (REPO / cwd / target).resolve()
                self.assertTrue(path.is_file(), f"{s.name}: {path} is not a file")
                if argv[0] not in ("python3", "node"):
                    self.assertTrue(os.access(path, os.X_OK), f"{s.name}: {path} not executable")
            for o in s.outputs:
                self.assertTrue((REPO / o).exists(), f"{s.name}: output {o} is not in the tree")
        for h in rc.FIGURE_HEADERS:
            self.assertTrue((REPO / h).is_file(), h)
        for o in rc.ENCLOSURE_OUTPUTS:
            self.assertTrue((REPO / o).is_file(), o)
        # the sketch-mirror check is what CI's guard does, against the tree instead of HEAD
        guard = (REPO / "firmware/scripts/check_display_arduino_sync.sh").read_text(encoding="utf-8")
        self.assertIn("./setup.sh regen", guard)
        self.assertIn('git diff --quiet -- "$SKETCH"', guard)

    def test_render_grep_is_enclosure_ymls(self):
        yml = (REPO / ".github/workflows/enclosure.yml").read_text(encoding="utf-8")
        self.assertIn("grep -E 'ERROR|WARNING|Object isn.t a valid 2-manifold' render.log", yml)
        self.assertEqual(rc.RENDER_FAIL_RE.pattern, "ERROR|WARNING|Object isn.t a valid 2-manifold")
        for line in ("ERROR: Parser error", "WARNING: variable x was assigned",
                     "Object isn't a valid 2-manifold and may cause problems"):
            self.assertTrue(rc.RENDER_FAIL_RE.search(line), line)
        self.assertFalse(rc.RENDER_FAIL_RE.search("Rendering canary_wap_enclosure_battery_base.stl ..."))
        self.assertFalse(rc.RENDER_FAIL_RE.search("CGAL Cache statistics"))

    def test_list_prints_the_steps(self):
        code, out, rec = run_main(["--list"])
        self.assertEqual(code, 0)
        self.assertEqual(rec.calls, [])
        for i, name in enumerate(ORDER, 1):
            self.assertIn(f"{i:>2}  {name}", out)


class OpenScadIsRefusedUpFront(unittest.TestCase):
    def test_write_run_is_refused_before_anything_runs(self):
        code, out, rec = run_main([], which=None)
        self.assertEqual(code, 1)
        self.assertEqual(rec.calls, [])
        self.assertIn("step 3 (render) and step 4 (gen_assembled_dims)", out)
        self.assertIn("no Actions button", out)
        self.assertIn('"Enclosure CAD"', out)
        self.assertIn("--from render", out)
        self.assertIn("2021.01", out)

    def test_previews_are_refused_too(self):
        with tempfile.TemporaryDirectory() as td:
            code, out, rec = run_main(["--from", "gen_flash", "--previews", td], which=None)
        self.assertEqual(code, 1)
        self.assertEqual(rec.calls, [])
        self.assertIn("--previews needs OpenSCAD", out)

    def test_check_is_refused_when_an_openscad_check_is_in_range_and_not_otherwise(self):
        code, out, rec = run_main(["--check"], which=None)
        self.assertEqual(code, 1)
        self.assertEqual(rec.calls, [])
        self.assertIn("step 4 (gen_assembled_dims)", out)
        self.assertNotIn("step 3 (render)", out)          # render has no check form to refuse
        code, out, rec = run_main(["--check", "--from", "gen_figures"], which=None)
        self.assertEqual(code, 0)
        self.assertEqual(rec.argvs()[0], list(CHECKS["gen_figures"]))

    def test_from_past_the_openscad_steps_runs_without_openscad(self):
        code, out, rec = run_main(["--from", "gen_figures"], which=None)
        self.assertEqual(code, 0)
        self.assertEqual(rec.argvs()[0], ["node", "canary-local/tools/figures/gen_figures.mjs"])


class CheckRunsEveryCheckFormInOrder(unittest.TestCase):
    def test_all_green(self):
        code, out, rec = run_main(["--check"])
        self.assertEqual(code, 0)
        self.assertEqual(rec.argvs(), [list(CHECKS[n]) for n in ORDER if CHECKS[n] is not None])
        self.assertIn("[3/12] render  check: (no check form)", out)
        self.assertIn("4 generated file(s) reproduce byte-for-byte", out)
        self.assertIn("check complete — 12 step(s)", out)
        # the check forms run where CI runs them
        cwds = {a[0] if a[0] != "python3" and a[0] != "node" else a[1]: c for a, c in rec.calls}
        self.assertEqual(cwds["./setup.sh"], str(rc.REPO / "firmware/projects/canary-display"))
        self.assertEqual(cwds[f"{ENC}/gen_cad_params.py"], str(rc.REPO))

    def test_first_failure_is_named_with_the_resume_command(self):
        rules = [("gen_figures.mjs --check", const(1))]
        code, out, rec = run_main(["--check"], rules)
        self.assertEqual(code, 1)
        self.assertEqual(rec.argvs(), [list(CHECKS[n]) for n in
                                       ("gen_cad_params", "lint_design_lang", "gen_assembled_dims",
                                        "gen_figures")])
        self.assertIn("FIRST FAILURE at step 5 (gen_figures)", out)
        self.assertIn("--from gen_figures", out)

    def test_gen_enclosures_check_restores_the_committed_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for o in rc.ENCLOSURE_OUTPUTS:
                (root / o).parent.mkdir(parents=True, exist_ok=True)
                (root / o).write_bytes(b"committed " + o.encode())

            def fake_generator(argv, cwd):
                (root / "canary-local/devices/catalog.json").write_bytes(b"regenerated differently")
                return 0, "OK: 38 sets"

            rules = [("gen_enclosures.py", fake_generator)]
            code, out, rec = run_main(["--check", "--from", "gen_enclosures"], rules, repo=root)
            self.assertEqual(code, 1)
            self.assertIn("FIRST FAILURE at step 10 (gen_enclosures)", out)
            self.assertIn("catalog.json would change", out)
            self.assertIn("tree's bytes were put back", out)
            for o in rc.ENCLOSURE_OUTPUTS:
                self.assertEqual((root / o).read_bytes(), b"committed " + o.encode(), o)

    def test_sketch_mirror_check_regenerates_against_the_tree_and_restores_it(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sketch = root / "firmware/projects/canary-display/arduino/canary_display"
            sketch.mkdir(parents=True)
            (sketch / "fleet_figures.h").write_bytes(b"uncommitted but current")
            (sketch / "main.cpp").write_bytes(b"main")

            def in_sync(argv, cwd):
                return 0, "Flattened"                     # regen reproduces the tree exactly

            code, out, rec = run_main(["--check", "--from", "setup_regen"],
                                      [("setup.sh regen", in_sync)], repo=root)
            self.assertEqual(code, 0)
            self.assertIn("2 generated file(s) reproduce byte-for-byte", out)
            self.assertEqual(rec.calls[0], (["./setup.sh", "regen"],
                                            str(root / "firmware/projects/canary-display")))

            def drifted(argv, cwd):
                (sketch / "fleet_figures.h").write_bytes(b"regenerated differently")
                (sketch / "main.cpp").unlink()
                (sketch / "new.cpp").write_bytes(b"added")
                return 0, "Flattened"

            code, out, rec = run_main(["--check", "--from", "setup_regen"],
                                      [("setup.sh regen", drifted)], repo=root)
            self.assertEqual(code, 1)
            self.assertIn("FIRST FAILURE at step 7 (setup_regen)", out)
            for name in ("fleet_figures.h", "main.cpp", "new.cpp"):
                self.assertIn(name, out)
            self.assertEqual(sorted(f.name for f in sketch.iterdir()), ["fleet_figures.h", "main.cpp"])
            self.assertEqual((sketch / "fleet_figures.h").read_bytes(), b"uncommitted but current")
            self.assertEqual((sketch / "main.cpp").read_bytes(), b"main")

    def test_site_is_ignored_under_check(self):
        with tempfile.TemporaryDirectory() as td:
            code, out, rec = run_main(["--check", "--from", "gen_builder_manifest", "--site", td])
        self.assertEqual(code, 0)
        self.assertIn("--site is ignored under --check", out)
        for argv in rec.argvs():
            self.assertNotIn("--site", argv)

    def test_previews_do_not_combine_with_check(self):
        with tempfile.TemporaryDirectory() as td, redirect_stdout(io.StringIO()), \
                mock.patch.object(sys, "stderr", io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                rc.main(["--check", "--previews", td])
        self.assertEqual(cm.exception.code, 2)

    def test_unknown_step_is_refused_by_the_parser(self):
        with redirect_stdout(io.StringIO()), mock.patch.object(sys, "stderr", io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                rc.main(["--from", "nope"])
        self.assertEqual(cm.exception.code, 2)


class AFullRunFollowsTheOrderAndStopsForTheDist(unittest.TestCase):
    RENDER_OK = [("render.sh", const(0, "Rendering a.stl ...\nRendering b.stl ...\nDone: released STLs\n"))]

    def test_headers_moved_stops_after_the_sketch_mirror_before_gen_flash(self):
        rules = self.RENDER_OK + [("git status --porcelain -- firmware/common/core", const(0, HEADERS_MOVED))]
        code, out, rec = run_main([], rules)
        self.assertEqual(code, rc.EXIT_STOPPED)
        self.assertEqual(code, 3)
        argvs = rec.argvs()
        self.assertEqual(argvs, [
            ["python3", f"{ENC}/gen_cad_params.py"],
            ["python3", "scripts/lint_design_lang.py"],
            ["./render.sh", "--no-png"],
            ["python3", f"{ENC}/gen_assembled_dims.py"],
            ["node", "canary-local/tools/figures/gen_figures.mjs"],
            ["node", "canary-local/tools/figures/gen_device_glbs.mjs"],
            ["./setup.sh", "regen"],
        ])
        cwd_of = {tuple(a): c for a, c in rec.calls}
        self.assertEqual(cwd_of[("./render.sh", "--no-png")], str(rc.REPO / ENC))
        self.assertEqual(cwd_of[("./setup.sh", "regen")], str(rc.REPO / "firmware/projects/canary-display"))
        self.assertIn("STOPPED at step 7 (setup_regen) — not a failure", out)
        self.assertIn('Actions -> "Rebuild emulator dist (pinned emsdk)"', out)
        self.assertIn("--from gen_flash", out)
        self.assertIn("fleet_figures.h, firmware/common/core/fleet_figures_art.h moved", out)
        self.assertIn("ZERO check runs", out)
        self.assertIn("action_required", out)
        self.assertIn("2 renders, log clean", out)

    def test_headers_unchanged_runs_through_to_the_report_only_checks(self):
        code, out, rec = run_main([], self.RENDER_OK)
        self.assertEqual(code, 0)
        argvs = rec.argvs()
        self.assertNotIn(["./setup.sh", "regen"], argvs)
        self.assertEqual(argvs[6:], [
            ["python3", "canary-local/tools/gen_flash.py"],
            ["python3", f"{ENC}/gen_builder_manifest.py"],
            ["python3", "canary-local/tools/gen_enclosures.py"],
            ["python3", f"{ENC}/gen_stamp.py", "--check"],
            ["python3", f"{ENC}/gen_mark_svg.py", "--check"],
        ])
        self.assertIn("did not move", out)
        self.assertIn("run complete — 12 step(s)", out)

    def test_from_skips_exactly_the_earlier_steps_and_site_reaches_the_builder(self):
        with tempfile.TemporaryDirectory() as td:
            code, out, rec = run_main(["--from", "gen_flash", "--site", td])
            self.assertEqual(code, 0)
            self.assertEqual(rec.argvs(), [
                ["python3", "canary-local/tools/gen_flash.py"],
                ["python3", f"{ENC}/gen_builder_manifest.py", "--site", td],
                ["python3", "canary-local/tools/gen_enclosures.py"],
                ["python3", f"{ENC}/gen_stamp.py", "--check"],
                ["python3", f"{ENC}/gen_mark_svg.py", "--check"],
            ])
        self.assertIn("steps 8..12 of 12 (from gen_flash)", out)

    def test_render_log_is_judged_by_the_ci_grep(self):
        for bad in ("WARNING: variable board_w was assigned on line 154 but was overwritten",
                    "Object isn't a valid 2-manifold and may cause problems!",
                    "ERROR: Parser error in file"):
            rules = [("render.sh", const(0, f"Rendering x.stl ...\n{bad}\nRendering y.stl ...\n"))]
            code, out, rec = run_main([], rules)
            self.assertEqual(code, 1, bad)
            self.assertIn("FAILED at step 3 (render)", out)
            self.assertIn("enclosure.yml fails on these", out)
            self.assertIn("--from render", out)
            self.assertEqual(rec.argvs()[-1], ["./render.sh", "--no-png"])   # nothing after it ran
        rules = [("render.sh", const(2, "openscad not found"))]
        code, out, rec = run_main([], rules)
        self.assertEqual(code, 1)
        self.assertIn("render.sh exited 2", out)

    def test_a_failing_step_names_itself_and_the_resume_command(self):
        rules = self.RENDER_OK + [("gen_device_glbs.mjs", const(1))]
        code, out, rec = run_main([], rules)
        self.assertEqual(code, 1)
        self.assertIn("FAILED at step 6 (gen_device_glbs)", out)
        self.assertIn("resume: python3 scripts/regen_cad.py --from gen_device_glbs", out)
        self.assertEqual(rec.argvs()[-1], ["node", "canary-local/tools/figures/gen_device_glbs.mjs"])


class PreviewsCoverEveryPartWithRenderShsSelectors(unittest.TestCase):
    def test_part_enum_is_the_builders(self):
        self.assertEqual(rc.part_enum(rc.ENC / "canary_wap_enclosure.scad"),
                         ["base", "lid", "all", "coupon", "gasket", "shield", "tray"])
        self.assertEqual(rc.part_enum(rc.ENC / "canary_vision_doorbell.scad"),
                         ["body", "face", "plate", "gasket", "all"])
        self.assertIsNone(rc.part_enum(rc.ENC / "canary_board_lib.scad"))
        self.assertEqual(rc.preview_plan("canary_board_lib.scad"), [])

    def test_wap_plan_is_render_shs_presets_times_parts_times_two_views(self):
        jobs = rc.preview_plan("canary_wap_enclosure.scad")
        self.assertEqual(len(jobs), 26)
        by_part = {}
        for j in jobs:
            by_part.setdefault(j.part, set()).add((j.label, j.view))
        presets = {"battery_full", "compact_plain", "battery_weather"}
        for part in ("base", "lid", "all"):
            self.assertEqual(by_part[part], {(p, v) for p in presets for v in ("top", "under")}, part)
        for part in ("gasket", "shield"):
            self.assertEqual(by_part[part], {("battery_weather", "top"), ("battery_weather", "under")})
        for part in ("coupon", "tray"):
            self.assertEqual(by_part[part], {("", "top"), ("", "under")})
        self.assertEqual(len({j.out for j in jobs}), 26)                 # distinct file names
        self.assertIn("preview_canary_wap_enclosure_battery_full_base_top.png", {j.out for j in jobs})
        self.assertIn("preview_canary_wap_enclosure_coupon_under.png", {j.out for j in jobs})

    def test_vision_sense_and_doorbell_plans(self):
        vision = rc.preview_plan("canary_vision_enclosure.scad")
        self.assertEqual(len(vision), 26)
        hosts = {(j.defines.get("host"), j.defines.get("preset")) for j in vision if j.part == "back"}
        self.assertEqual(hosts, {("xiao", "vision_indoor"), ("xiao", "vision_weather"),
                                 ("devkit", "vision_indoor")})
        self.assertEqual({j.label for j in vision if j.part == "hood"}, {"xiao_weather"})
        self.assertEqual({j.label for j in vision if j.part == "bracket"}, {""})
        sense = rc.preview_plan("canary_sense_enclosure.scad")
        self.assertEqual(len(sense), 12)
        self.assertTrue(all(j.defines == {} for j in sense))
        doorbell = rc.preview_plan("canary_vision_doorbell.scad")
        self.assertEqual(len(doorbell), 12)
        self.assertEqual({(j.label, j.defines.get("plate_wedge")) for j in doorbell if j.part == "plate"},
                         {("", None), ("wedge15", 15)})

    def test_argv_is_the_readmes_recipe(self):
        job = rc.preview_plan("canary_wap_enclosure.scad")[0]
        out = Path("/tmp/previews")
        argv = rc.preview_argv(job, out, xvfb=True)
        self.assertEqual(argv, [
            "xvfb-run", "-a", "openscad", "-o", "/tmp/previews/" + job.out, "--imgsize", "1400,1000",
            "--autocenter", "--viewall", "--camera=0,0,0,62,0,25,120", "--colorscheme", "Tomorrow Night",
            "-D", 'part="base"', "-D", 'preset="battery_full"', "canary_wap_enclosure.scad"])
        under = [j for j in rc.preview_plan("canary_wap_enclosure.scad") if j.view == "under"][0]
        self.assertIn("--camera=0,0,0,245,0,25,120", rc.preview_argv(under, out, xvfb=False))
        self.assertEqual(rc.preview_argv(under, out, xvfb=False)[0], "openscad")
        wedge = [j for j in rc.preview_plan("canary_vision_doorbell.scad") if j.label == "wedge15"][0]
        self.assertIn("plate_wedge=15", rc.preview_argv(wedge, out, xvfb=False))
        readme = (REPO / ENC / "README.md").read_text(encoding="utf-8")
        self.assertIn("--imgsize 1400,1000 --autocenter --viewall", readme)
        self.assertIn('--camera=0,0,0,ROTX,0,ROTZ,120 --colorscheme "Tomorrow Night"', readme)

    def test_changed_cases_come_from_git_and_libraries_are_named_not_rendered(self):
        status = (f" M {ENC}/canary_wap_enclosure.scad\n M {ENC}/canary_board_lib.scad\n"
                  f" M {ENC}/README.md\n")
        rec = Recorder([("git status --porcelain", const(0, status))])
        with mock.patch.object(rc.subprocess, "run", rec):
            cases, libs = rc.changed_cases(rc.REPO)
        self.assertEqual((cases, libs), (["canary_wap_enclosure.scad"], ["canary_board_lib.scad"]))
        self.assertEqual(rec.calls[0][0][:4], ["git", "status", "--porcelain", "--"])

    def test_previews_render_after_step_one_into_the_directory(self):
        with tempfile.TemporaryDirectory() as td:
            out_dir = Path(td) / "previews"

            def fake_openscad(argv, cwd):
                Path(argv[argv.index("-o") + 1]).parent.mkdir(parents=True, exist_ok=True)
                Path(argv[argv.index("-o") + 1]).write_bytes(b"PNG")
                return 0, ""

            rules = [("git status --porcelain -- docs/hardware", const(0, f" M {ENC}/canary_wap_enclosure.scad\n")),
                     ("render.sh", const(0, "Rendering a.stl ...\n")),
                     ("openscad -o", fake_openscad)]
            code, out, rec = run_main(["--previews", str(out_dir)], rules)
            self.assertEqual(code, 0)
            pngs = sorted(p.name for p in out_dir.glob("*.png"))
            self.assertEqual(len(pngs), 26)
            self.assertIn("preview_canary_wap_enclosure_battery_weather_gasket_under.png", pngs)
            argvs = rec.argvs()
            # previews come right after step 1, before the lint and the STL render
            self.assertEqual(argvs[0], ["python3", f"{ENC}/gen_cad_params.py"])
            renders = [a for a in argvs if "openscad" in a[:3] and "-o" in a]
            self.assertEqual(len(renders), 26)
            self.assertEqual(argvs.index(renders[0]), 1)
            self.assertEqual(argvs[27], ["python3", "scripts/lint_design_lang.py"])
            self.assertTrue(all(c == str(rc.REPO / ENC) for a, c in rec.calls if "-o" in a and "openscad" in a[:3]))
            self.assertIn("26 PNG(s) in", out)
            self.assertIn("never commit them", out)

    def test_no_changed_case_owes_no_previews(self):
        with tempfile.TemporaryDirectory() as td:
            rules = [("render.sh", const(0, "Rendering a.stl ...\n"))]
            code, out, rec = run_main(["--previews", td], rules)
        self.assertEqual(code, 0)
        self.assertIn("no case .scad changed — no previews owed", out)
        self.assertFalse(any("openscad" in a[:3] for a in rec.argvs()))

    def test_a_failed_preview_fails_the_run(self):
        with tempfile.TemporaryDirectory() as td:
            rules = [("git status --porcelain -- docs/hardware", const(0, f" M {ENC}/canary_sense_enclosure.scad\n")),
                     ("openscad -o", const(1, "ERROR: no GL context"))]
            code, out, rec = run_main(["--previews", td], rules)
        self.assertEqual(code, 1)
        self.assertIn("FAILED rendering previews (12 of 12)", out)


if __name__ == "__main__":
    sys.exit(unittest.main())
