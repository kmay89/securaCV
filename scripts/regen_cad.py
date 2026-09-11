#!/usr/bin/env python3
"""regen_cad.py — the enclosure regeneration chain as ONE command, in the order the tree proves.

    python3 scripts/regen_cad.py                     # run every step (writes); stops before the dist rebuild
    python3 scripts/regen_cad.py --check             # each step's CHECK form, in order; the first failure is named
    python3 scripts/regen_cad.py --from gen_flash    # resume after the emulator dist came back
    python3 scripts/regen_cad.py --previews DIR      # + PNG previews of every part of every case whose bytes moved
    python3 scripts/regen_cad.py --site DIR          # + the website carries (gen_builder_manifest.py --site DIR)
    python3 scripts/regen_cad.py --list              # the steps, numbered, with their check forms

WHY ONE COMMAND. A dimension edit in devices/<slug>/device.json `cad.params`
moves one literal in a case .scad, and from there ten committed, byte-gated
files move in a FIXED order. That order lived in prose (CLAUDE.md,
devices/README.md, gen_cad_params.py's REGEN_ORDER) and in the CI step
order, and nowhere runnable — and run out of order, a failure names a cause
that has nothing to do with the edit: a catalog generated BEFORE the
emulator dist rebuild bakes in the old fw_version and later fails as "a
board's chip changed". This script is that order, derived from what each
generator READS (its docstring, its imports), not from memory:

   1  gen_cad_params        manifest -> the case .scad literals            (writes the knob)
   2  lint_design_lang      the literal-knob canon still holds              (a lint; nothing written)
   3  render                render.sh --no-png: every released and dev STL  (needs OpenSCAD)
   4  gen_assembled_dims    fit-checked unions -> assembled_dims.json       (needs OpenSCAD)
   5  gen_figures           STL bboxes + assembled_dims -> figures.json, SVGs,
                            fleet_figures.h / fleet_figures_art.h, FleetFigures.swift / FleetSolids.swift
   6  gen_device_glbs       figures.json verdicts -> the two flashers' .glb
   7  setup_regen           IF fleet_figures.h or fleet_figures_art.h moved: the Arduino sketch
                            mirror (firmware/projects/canary-display/setup.sh regen), then STOP —
                            the emulator dist is upstream of the catalogs and only Actions can build it
   8  gen_flash             dist meta + manifests -> flash.json
   9  gen_builder_manifest  curated .scad -> builder_manifest.json  [+ --site DIR: the website carries]
  10  gen_enclosures        README tables + .scad knobs -> enclosures / catalog / build / workshop .json
  11  gen_stamp             report-only: --check (a STAMP_REV bump is a human decision, gen_stamp.py)
  12  gen_mark_svg          report-only: --check (the mark does not move with a board knob)

THE STOP AT STEP 7 IS THE POINT. The emulator compiles fleet_figure.cpp
against fleet_figures.h, and gen_flash.py stamps each artifact's
fw_version from canary-local/emulator/dist/*.meta.json into flash.json — so
when the figure headers moved, the dist must be rebuilt (Actions ->
"Rebuild emulator dist (pinned emsdk)", dispatched on YOUR branch; emsdk
6.0.3 exactly, which most machines cannot install) and pulled BEFORE steps
8–10 run. The script regenerates the sketch mirror, prints exactly that,
and exits 3 (not a failure: a resume point). `--from gen_flash` finishes
the chain once the dist is back. CLAUDE.md's four dist-retrigger symptoms
apply and are printed at the stop.

--check. Every step is run in its CHECK form and the first failure is named
with the resume command. `render` has no check form — OpenSCAD's STL bytes
are not deterministic, so nothing byte-gates them; the two steps after it
gate their bounding boxes and seams (gen_assembled_dims.py --check at
0.01 mm, gen_figures.mjs --check) and enclosure.yml re-renders and greps
the log in CI. gen_enclosures.py has no --check flag: its check form is the
one canary-local.yml uses — regenerate, compare the four JSON outputs —
except that the committed bytes are put back afterwards, so --check writes
nothing anywhere. --site is ignored under --check (gen_builder_manifest.py
--site writes the carries whether or not --check is given).

--previews DIR. The rule of record (AGENTS.md "Before you commit";
docs/hardware/enclosure/README.md "Preview renders"): every .scad change
ships PNG previews of every affected part, shared with the requester, never
committed. After step 1, every case .scad whose bytes moved (git status)
is rendered for EVERY value of its `part` enum — the enum the builder's
parser reads, so a new part is previewed without editing this file — with
the -D selector sets render.sh uses for that file (WAP presets, Vision
host x preset, the doorbell's wedge), at ROTX 62 (top three-quarter) and
ROTX 245 (underside), through the README's own command. A changed library
(no `part` knob) is named, not rendered: every case that uses it owes
previews, and that list is the requester's to judge.

OPENSCAD. Steps 3 and 4 and --previews are refused, before anything runs,
when `openscad` is not on PATH. There is no Actions button that renders the
STLs and pushes them back: the "Enclosure CAD" workflow (enclosure.yml)
re-renders every part, re-measures the assembled envelopes and runs the fit
gate on every push that touches docs/hardware/enclosure/** — it names the
drift, it does not fix it. Install OpenSCAD 2021.01 (the version CI
installs) and resume with --from render.

Exit codes: 0 done · 1 a step failed or was refused · 3 stopped at step 7
for the dist rebuild (resume with --from gen_flash).

Unit-tested by scripts/tests/test_regen_cad.py with subprocess mocked: the
order, the check mapping, the refusals and --from run there without
executing anything.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple

REPO = Path(__file__).resolve().parent.parent
ENC_REL = "docs/hardware/enclosure"
ENC = REPO / ENC_REL

# The two headers whose move means "the emulator dist and the Arduino sketch
# mirror are stale" (firmware/projects/canary-display/setup.sh copies them
# into the sketch; canary-local/emulator/build.sh compiles against them).
FIGURE_HEADERS = ("firmware/common/core/fleet_figures.h",
                  "firmware/common/core/fleet_figures_art.h")
# gen_enclosures.py's outputs — the four files canary-local.yml diffs.
ENCLOSURE_OUTPUTS = ("canary-local/devices/enclosures.json", "canary-local/devices/catalog.json",
                     "canary-local/devices/build.json", "canary-local/devices/workshop.json")
# enclosure.yml's render gate, verbatim: OpenSCAD exits 0 on geometry
# warnings, so the log is grepped.
RENDER_FAIL_RE = re.compile(r"ERROR|WARNING|Object isn.t a valid 2-manifold")

DIST_BUTTON = 'Actions -> "Rebuild emulator dist (pinned emsdk)"'
CAD_WORKFLOW = '"Enclosure CAD" (.github/workflows/enclosure.yml)'
EXIT_STOPPED = 3


class Step(NamedTuple):
    name: str
    cmd: tuple[str, ...]            # the write form (argv), run in `cwd`
    cwd: str                        # repo-relative
    check: tuple[str, ...] | None   # the check form (argv), or None (see check_kind)
    check_kind: str                 # "argv" | "diff" | "none"
    needs_openscad: bool
    reads: str                      # what it reads -> what it writes, one line
    note: str = ""                  # for "none": why there is no check form and what covers it
    outputs: tuple[str, ...] = ()   # for "diff": the files regenerated and compared
    report_only: bool = False       # cmd IS the check form; the write form is a human's call
    conditional: bool = False       # step 7: runs only when FIGURE_HEADERS moved
    check_cwd: str | None = None    # where the check form runs, when not `cwd` (step 7's is a repo-root script)


STEPS: tuple[Step, ...] = (
    Step("gen_cad_params",
         ("python3", f"{ENC_REL}/gen_cad_params.py"), ".",
         ("python3", f"{ENC_REL}/gen_cad_params.py", "--check"), "argv", False,
         "devices/*/device.json cad.params (+ canary_board_lib.scad) -> the case .scad literals"),
    Step("lint_design_lang",
         ("python3", "scripts/lint_design_lang.py"), ".",
         ("python3", "scripts/lint_design_lang.py"), "argv", False,
         "every canonical knob default conforms to canary_core_lib or explains itself (a lint)"),
    Step("render",
         ("./render.sh", "--no-png"), ENC_REL,
         None, "none", True,
         "every case .scad -> the committed released STLs (+ gitignored dev_*.stl, template SVGs)",
         note=("no check form: OpenSCAD's STL bytes are not deterministic, so nothing byte-gates "
               "them; gen_assembled_dims --check and gen_figures --check gate their bounding boxes "
               "and seams, and enclosure.yml re-renders and greps the log in CI")),
    Step("gen_assembled_dims",
         ("python3", f"{ENC_REL}/gen_assembled_dims.py"), ".",
         ("python3", f"{ENC_REL}/gen_assembled_dims.py", "--check"), "argv", True,
         "each device's fit-checked assembled union (OpenSCAD) -> assembled_dims.json"),
    Step("gen_figures",
         ("node", "canary-local/tools/figures/gen_figures.mjs"), ".",
         ("node", "canary-local/tools/figures/gen_figures.mjs", "--check"), "argv", False,
         "committed STL bboxes + assembled_dims.json + manifests -> figures.json, SVGs, "
         "fleet_figures*.h, FleetFigures.swift, FleetSolids.swift, figures.picker.json"),
    Step("gen_device_glbs",
         ("node", "canary-local/tools/figures/gen_device_glbs.mjs"), ".",
         ("node", "canary-local/tools/figures/gen_device_glbs.mjs", "--check"), "argv", False,
         "figures.json verdicts + massing -> canary-local/models/*.glb and desktop/src/models/*.glb"),
    Step("setup_regen",
         ("./setup.sh", "regen"), "firmware/projects/canary-display",
         ("firmware/scripts/check_display_arduino_sync.sh",), "argv", False,
         "fleet_figures.h / fleet_figures_art.h -> the Arduino sketch mirror; then STOP for the dist",
         conditional=True, check_cwd="."),
    Step("gen_flash",
         ("python3", "canary-local/tools/gen_flash.py"), ".",
         ("python3", "canary-local/tools/gen_flash.py", "--check"), "argv", False,
         "emulator dist *.meta.json (fw_version) + manifests + figures.picker.json -> flash.json"),
    Step("gen_builder_manifest",
         ("python3", f"{ENC_REL}/gen_builder_manifest.py"), ".",
         ("python3", f"{ENC_REL}/gen_builder_manifest.py", "--check"), "argv", False,
         "the curated .scad + deps + colorways -> builder_manifest.json (sha256 pins) "
         "[--site DIR: the website's scad/ carries, cad-dims.json, builder-data.js]"),
    Step("gen_enclosures",
         ("python3", "canary-local/tools/gen_enclosures.py"), ".",
         ("python3", "canary-local/tools/gen_enclosures.py"), "diff", False,
         "README variant tables + every .scad's knobs -> enclosures / catalog / build / workshop .json",
         outputs=ENCLOSURE_OUTPUTS),
    Step("gen_stamp",
         ("python3", f"{ENC_REL}/gen_stamp.py", "--check"), ".",
         ("python3", f"{ENC_REL}/gen_stamp.py", "--check"), "argv", False,
         "report-only: the 7\" case's build stamp still matches its geometry digest "
         "(a STAMP_REV bump is a human decision; this never regenerates it)",
         report_only=True),
    Step("gen_mark_svg",
         ("python3", f"{ENC_REL}/gen_mark_svg.py", "--check"), ".",
         ("python3", f"{ENC_REL}/gen_mark_svg.py", "--check"), "argv", False,
         "report-only: the house-mark SVG still matches canary_mark_lib.scad",
         report_only=True),
)
STEP_NAMES = tuple(s.name for s in STEPS)
STEP_BY_NAME = {s.name: s for s in STEPS}
RESUME_AFTER_DIST = "gen_flash"

# --previews: the -D selector sets render.sh uses per case file, per part —
# (label, {knob: value}). A part not listed renders with the file's defaults;
# a file not listed renders every part with defaults. Values are quoted the
# way -D wants them (strings quoted, numbers bare).
_WAP_PRESETS = [("battery_full", {"preset": "battery_full"}),
                ("compact_plain", {"preset": "compact_plain"}),
                ("battery_weather", {"preset": "battery_weather"})]
_WAP_WEATHER = [("battery_weather", {"preset": "battery_weather"})]
_VISION_SETS = [("xiao_indoor", {"host": "xiao", "preset": "vision_indoor"}),
                ("xiao_weather", {"host": "xiao", "preset": "vision_weather"}),
                ("devkit_indoor", {"host": "devkit", "preset": "vision_indoor"})]
_VISION_WEATHER = [("xiao_weather", {"host": "xiao", "preset": "vision_weather"})]
PREVIEW_SETS: dict[str, dict[str, list[tuple[str, dict]]]] = {
    "canary_wap_enclosure.scad": {
        "base": _WAP_PRESETS, "lid": _WAP_PRESETS, "all": _WAP_PRESETS,
        "gasket": _WAP_WEATHER, "shield": _WAP_WEATHER,
    },
    "canary_vision_enclosure.scad": {
        "back": _VISION_SETS, "front": _VISION_SETS, "all": _VISION_SETS,
        "gasket": _VISION_WEATHER, "hood": _VISION_WEATHER,
    },
    "canary_vision_doorbell.scad": {
        "plate": [("", {}), ("wedge15", {"plate_wedge": 15})],
    },
    "canary_sense_enclosure.scad": {},
}
# README "Preview renders": ROTX 62 -> top three-quarter, 245 -> underside; ROTZ 25.
PREVIEW_VIEWS = (("top", 62), ("under", 245))
PREVIEW_ROTZ = 25


class PreviewJob(NamedTuple):
    scad: str            # file name, in ENC
    part: str
    label: str           # selector-set label ("" for defaults)
    defines: dict        # -D knobs, `part` excluded
    view: str            # top | under
    rotx: int
    out: str             # PNG file name


# ---------------------------------------------------------------------------
# running things (everything a test mocks goes through here)
# ---------------------------------------------------------------------------

def _run(argv: list[str] | tuple[str, ...], cwd: Path | None = None,
         capture: bool = False) -> subprocess.CompletedProcess:
    kw: dict = {"cwd": str(cwd) if cwd else None, "text": True}
    if capture:
        kw.update(stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return subprocess.run(list(argv), **kw)


def _git_modified(paths: tuple[str, ...] | list[str], repo: Path | None = None) -> list[str]:
    """Repo-relative paths under `paths` that git sees as changed (any status)."""
    r = _run(["git", "status", "--porcelain", "--", *paths], cwd=repo or REPO, capture=True)
    out = r.stdout or ""
    files = []
    for line in out.splitlines():
        if len(line) > 3:
            files.append(line[3:].split(" -> ")[-1].strip().strip('"'))
    return sorted(set(files))


def openscad_available() -> bool:
    return shutil.which("openscad") is not None


def _define(k: str, v) -> str:
    if isinstance(v, bool):
        return f"{k}={'true' if v else 'false'}"
    if isinstance(v, (int, float)):
        return f"{k}={v}"
    return f"{k}={json.dumps(v)}"


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# messages
# ---------------------------------------------------------------------------

def openscad_missing_message(what: str) -> str:
    return (
        f"regen_cad: {what} needs OpenSCAD and `openscad` is not on PATH — refused before anything ran.\n"
        f"  There is no Actions button that renders the STLs and pushes them back: {CAD_WORKFLOW}\n"
        f"  re-renders every part, re-measures the assembled envelopes (gen_assembled_dims.py --check)\n"
        f"  and runs the fit gate on every push touching {ENC_REL}/** — it names the drift, it does\n"
        f"  not fix it. Install OpenSCAD 2021.01 (the version CI installs: `sudo apt-get install\n"
        f"  openscad`) and resume with: python3 scripts/regen_cad.py --from render"
    )


def stop_message(moved: list[str]) -> str:
    return "\n".join([
        "",
        f"regen_cad: STOPPED at step {STEP_NAMES.index('setup_regen') + 1} (setup_regen) — not a failure.",
        f"  {', '.join(moved)} moved, so the emulator dist is stale and it is UPSTREAM of the catalogs:",
        "  gen_flash.py stamps each artifact's fw_version from canary-local/emulator/dist/*.meta.json.",
        "  The Arduino sketch mirror has been regenerated (firmware/projects/canary-display/setup.sh regen).",
        "",
        "  Now:",
        "    1. commit what moved so far and push YOUR branch (never main — the rebuild pushes to the",
        "       branch it is dispatched on);",
        f"    2. {DIST_BUTTON}, dispatched on that branch, then pull its commit;",
        f"    3. python3 scripts/regen_cad.py --from {RESUME_AFTER_DIST}   # steps 8–12",
        "",
        "  CLAUDE.md's four dist-retrigger symptoms apply to that push (\"Generated files\"):",
        "    - the bot's push does not retrigger CI (default GITHUB_TOKEN) — re-running the failed job",
        "      checks out the ORIGINAL commit, so push again yourself instead;",
        "    - that push must touch a path canary-local.yml / firmware.yml watch, or the PR sits at",
        "      ZERO check runs and looks queued;",
        "    - runs parked at `action_required` are waiting for a human's Approve, not for a commit;",
        "    - the drift check ignores meta.json; the fw_version stamp is caught by the page logic tests.",
    ])


# ---------------------------------------------------------------------------
# steps
# ---------------------------------------------------------------------------

def _hdr(i: int, step: Step, argv: tuple[str, ...], mode: str, cwd: str | None = None) -> str:
    cwd = cwd or step.cwd
    where = "" if cwd == "." else f"   (in {cwd})"
    return f"[{i}/{len(STEPS)}] {step.name}  {mode}: {' '.join(argv)}{where}"


def run_check(step: Step, i: int, repo: Path) -> tuple[bool, str]:
    """(ok, detail) for one step's check form."""
    if step.check_kind == "none":
        print(_hdr(i, step, ("(no check form)",), "check"))
        print(f"      skipped — {step.note}")
        return True, ""
    if step.needs_openscad and not openscad_available():
        print(_hdr(i, step, step.check or (), "check"))
        return False, openscad_missing_message(f"step {i} ({step.name}) --check")
    if step.check_kind == "diff":
        print(_hdr(i, step, step.check or (), "check"))
        return _check_by_diff(step, repo)
    print(_hdr(i, step, step.check or (), "check", step.check_cwd or step.cwd))
    r = _run(step.check or (), cwd=repo / (step.check_cwd or step.cwd))
    if r.returncode != 0:
        return False, f"{step.name} --check exited {r.returncode}"
    return True, ""


def _check_by_diff(step: Step, repo: Path) -> tuple[bool, str]:
    """Regenerate, compare the outputs, and put the committed bytes back —
    canary-local.yml's gate for gen_enclosures.py, minus the dirty tree."""
    paths = [repo / o for o in step.outputs]
    before = {p: (p.read_bytes() if p.exists() else None) for p in paths}
    r = _run(step.cmd, cwd=repo / step.cwd, capture=True)
    if r.stdout:
        print(r.stdout.rstrip())
    stale = []
    for p, old in before.items():
        new = p.read_bytes() if p.exists() else None
        if new != old:
            stale.append(str(p.relative_to(repo)))
            if old is None:
                p.unlink()
            else:
                p.write_bytes(old)
    if r.returncode != 0:
        return False, f"{step.name} exited {r.returncode}"
    if stale:
        return False, (f"{step.name}: generated catalog stale — {', '.join(stale)} would change "
                       f"(the committed bytes were put back)")
    print(f"      {len(paths)} generated file(s) reproduce byte-for-byte")
    return True, ""


def run_render(step: Step, i: int, repo: Path) -> tuple[bool, str]:
    """render.sh --no-png with enclosure.yml's failure grep over the log."""
    print(_hdr(i, step, step.cmd, "run"))
    r = _run(step.cmd, cwd=repo / step.cwd, capture=True)
    log = r.stdout or ""
    log_path = Path(tempfile.gettempdir()) / "regen_cad_render.log"
    try:
        log_path.write_text(log, encoding="utf-8")
    except OSError:
        log_path = Path("(unwritable)")
    rendered = sum(1 for ln in log.splitlines() if ln.startswith("Rendering "))
    bad = [ln for ln in log.splitlines() if RENDER_FAIL_RE.search(ln)]
    if r.returncode != 0:
        print(log[-4000:])
        return False, f"render.sh exited {r.returncode} (log: {log_path})"
    if bad:
        print("\n".join(bad[:40]))
        return False, (f"render.sh log matches /{RENDER_FAIL_RE.pattern}/ ({len(bad)} line(s) — "
                       f"enclosure.yml fails on these; log: {log_path})")
    print(f"      {rendered} renders, log clean (no ERROR / WARNING / non-manifold), log: {log_path}")
    return True, ""


def run_setup_regen(step: Step, i: int, repo: Path) -> tuple[str, str]:
    """('skipped' | 'stopped' | 'failed', detail)."""
    moved = _git_modified(FIGURE_HEADERS, repo)
    if not moved:
        print(_hdr(i, step, step.cmd, "run"))
        print(f"      skipped — {' and '.join(Path(h).name for h in FIGURE_HEADERS)} did not move: "
              f"the sketch mirror and the emulator dist stay as they are")
        return "skipped", ""
    print(_hdr(i, step, step.cmd, "run"))
    r = _run(step.cmd, cwd=repo / step.cwd)
    if r.returncode != 0:
        return "failed", f"setup.sh regen exited {r.returncode}"
    print(stop_message(moved))
    return "stopped", ""


def run_write(step: Step, i: int, repo: Path, site: Path | None) -> tuple[bool, str]:
    argv = tuple(step.cmd)
    if step.name == "gen_builder_manifest" and site is not None:
        argv = argv + ("--site", str(site))
    print(_hdr(i, step, argv, "check" if step.report_only else "run"))
    r = _run(argv, cwd=repo / step.cwd)
    if r.returncode != 0:
        return False, f"{step.name} exited {r.returncode}"
    return True, ""


# ---------------------------------------------------------------------------
# previews
# ---------------------------------------------------------------------------

def _parse_scad():
    if str(ENC) not in sys.path:
        sys.path.insert(0, str(ENC))
    from gen_builder_manifest import parse_scad  # noqa: E402  (the builder's parser, shared)
    return parse_scad


def part_enum(scad: Path) -> list[str] | None:
    """The `part` knob's option list, as the builder's parser reads it; None
    when the file has no `part` selector (a library, a single-part tool)."""
    for g in _parse_scad()(scad):
        for p in g["params"]:
            if p["name"] == "part" and "options" in p:
                return list(p["options"])
    return None


def preview_plan(scad_name: str, parts: list[str] | None = None) -> list[PreviewJob]:
    """Every (part x selector set x view) preview owed for one case file."""
    parts = parts if parts is not None else part_enum(ENC / scad_name)
    if parts is None:
        return []
    sets = PREVIEW_SETS.get(scad_name, {})
    stem = Path(scad_name).stem
    jobs: list[PreviewJob] = []
    for part in parts:
        for label, defines in sets.get(part, [("", {})]):
            for view, rotx in PREVIEW_VIEWS:
                tag = "_".join(x for x in (stem, label, part, view) if x)
                jobs.append(PreviewJob(scad_name, part, label, dict(defines), view, rotx,
                                       f"preview_{tag}.png"))
    return jobs


def preview_argv(job: PreviewJob, out_dir: Path, xvfb: bool | None = None) -> list[str]:
    """docs/hardware/enclosure/README.md "Preview renders", verbatim, for one job."""
    if xvfb is None:
        xvfb = not os.environ.get("DISPLAY") and shutil.which("xvfb-run") is not None
    argv = ["xvfb-run", "-a"] if xvfb else []
    argv += ["openscad", "-o", str(out_dir / job.out), "--imgsize", "1400,1000", "--autocenter",
             "--viewall", f"--camera=0,0,0,{job.rotx},0,{PREVIEW_ROTZ},120",
             "--colorscheme", "Tomorrow Night", "-D", _define("part", job.part)]
    for k, v in job.defines.items():
        argv += ["-D", _define(k, v)]
    argv.append(job.scad)
    return argv


def changed_cases(repo: Path) -> tuple[list[str], list[str]]:
    """(.scad files with a `part` enum that git sees as changed, changed .scad
    files without one) — the previews owed, and the libraries to name."""
    changed = _git_modified((f"{ENC_REL}/*.scad",), repo)
    cases, libs = [], []
    for rel in changed:
        name = Path(rel).name
        if not name.endswith(".scad"):
            continue
        path = repo / rel
        if path.exists() and part_enum(path) is not None:
            cases.append(name)
        else:
            libs.append(name)
    return cases, libs


def render_previews(out_dir: Path, repo: Path) -> tuple[int, list[str]]:
    """Render every owed preview into out_dir; (count rendered, failures)."""
    cases, libs = changed_cases(repo)
    if libs:
        print(f"      changed .scad without a `part` selector: {', '.join(libs)} — every case that "
              f"uses it owes previews (render those with --previews after listing them, or by the "
              f"README recipe); not rendered automatically")
    if not cases:
        print("      no case .scad changed — no previews owed")
        return 0, []
    out_dir.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    n = 0
    for name in cases:
        jobs = preview_plan(name)
        print(f"      {name}: {len(jobs)} previews ({len(jobs) // 2} part x selector sets, "
              f"both views) -> {out_dir}")
        for job in jobs:
            r = _run(preview_argv(job, out_dir), cwd=repo / ENC_REL, capture=True)
            ok = r.returncode == 0 and (out_dir / job.out).exists()
            if not ok:
                failures.append(f"{job.out}: exit {r.returncode}\n{(r.stdout or '')[-800:]}")
            else:
                n += 1
    return n, failures


# ---------------------------------------------------------------------------
# the chain
# ---------------------------------------------------------------------------

def list_steps() -> str:
    rows = []
    for i, s in enumerate(STEPS, 1):
        chk = {"argv": " ".join(s.check or ()), "diff": " ".join(s.check or ()) + "  + byte-compare "
               + ", ".join(Path(o).name for o in s.outputs) + " (committed bytes restored)",
               "none": "(none — " + s.note.split(";")[0] + ")"}[s.check_kind]
        flags = " ".join(f for f, on in (("[openscad]", s.needs_openscad), ("[report-only]", s.report_only),
                                          ("[conditional: stops]", s.conditional)) if on)
        rows.append(f"{i:>2}  {s.name:<21} {flags}\n      does:  {s.reads}\n      check: {chk}")
    return "\n".join(rows)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="run every step's CHECK form, in order; the first failure is named. Writes nothing")
    ap.add_argument("--from", dest="start", metavar="STEP", choices=STEP_NAMES,
                    help=f"start at STEP (one of: {', '.join(STEP_NAMES)}); "
                         f"--from {RESUME_AFTER_DIST} resumes after the emulator dist rebuild")
    ap.add_argument("--previews", metavar="DIR", type=Path,
                    help="after step 1, render PNG previews of every part of every case .scad whose "
                         "bytes changed into DIR (the README recipe; needs OpenSCAD; never commit them)")
    ap.add_argument("--site", metavar="DIR", type=Path,
                    help="also write the website carries (gen_builder_manifest.py --site DIR); "
                         "ignored under --check")
    ap.add_argument("--list", action="store_true", help="print the steps and exit")
    args = ap.parse_args(argv)

    if args.list:
        print(list_steps())
        return 0
    if args.previews is not None and args.check:
        ap.error("--previews renders what a WRITE changed; it does not combine with --check")

    repo = REPO
    start = STEP_NAMES.index(args.start) if args.start else 0
    todo = list(enumerate(STEPS, 1))[start:]
    mode = "check" if args.check else "run"

    # Refuse up front, before anything runs: an OpenSCAD step in range, or --previews.
    if not openscad_available():
        needing = [f"step {i} ({s.name})" for i, s in todo
                   if s.needs_openscad and not (args.check and s.check_kind == "none")]
        if args.previews is not None:
            needing.append("--previews")
        if needing:
            print(openscad_missing_message(" and ".join(needing)))
            return 1
    if args.check and args.site is not None:
        print("regen_cad: --site is ignored under --check (gen_builder_manifest.py --site writes the "
              "carries whether or not --check is given; run the write form to carry)")

    print(f"regen_cad: {mode} — steps {todo[0][0]}..{len(STEPS)} of {len(STEPS)}"
          + (f" (from {args.start})" if args.start else ""))
    for i, step in todo:
        if args.check:
            ok, detail = run_check(step, i, repo)
            if not ok:
                print(f"\nregen_cad: FIRST FAILURE at step {i} ({step.name}): {detail}")
                print(f"  fix: python3 scripts/regen_cad.py --from {step.name}   "
                      f"(the write form of this step and everything after it), then --check again")
                return 1
            continue
        # write mode
        if step.name == "render":
            ok, detail = run_render(step, i, repo)
        elif step.name == "setup_regen":
            state, detail = run_setup_regen(step, i, repo)
            if state == "stopped":
                return EXIT_STOPPED
            ok = state != "failed"
        else:
            ok, detail = run_write(step, i, repo, args.site)
        if not ok:
            print(f"\nregen_cad: FAILED at step {i} ({step.name}): {detail}")
            print(f"  resume: python3 scripts/regen_cad.py --from {step.name}")
            return 1
        if step.name == "gen_cad_params" and args.previews is not None:
            print(f"[{i}/{len(STEPS)}] previews  (README \"Preview renders\": every part of every "
                  f"changed case, ROTX {PREVIEW_VIEWS[0][1]} and {PREVIEW_VIEWS[1][1]})")
            n, failures = render_previews(args.previews, repo)
            if failures:
                print("\n".join(failures))
                print(f"\nregen_cad: FAILED rendering previews ({len(failures)} of {n + len(failures)})")
                return 1
            if n:
                print(f"      {n} PNG(s) in {args.previews} — share them with the requester; never "
                      f"commit them")
    print(f"\nregen_cad: {mode} complete — {len(todo)} step(s)"
          + ("" if args.check else "; git status lists what moved (STL bytes move on every render "
                                    "— their bounding boxes are what the gates compare)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
