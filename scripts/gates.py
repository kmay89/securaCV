#!/usr/bin/env python3
"""scripts/gates.py — which CI gates does this diff trip, and run the fast ones here.

    python3 scripts/gates.py              # what CI will run for the diff vs origin/main
    python3 scripts/gates.py --run        # and run every fast scripted gate, here, now
    python3 scripts/gates.py --all        # list the manual (toolchain / runner-only) steps too
    python3 scripts/gates.py --paths a b  # for a hypothetical diff
    python3 scripts/gates.py --base REF   # against another base (default origin/main, then main)

WHY. Fifty-nine workflows, twenty-nine of them path-filtered, forty-odd
generator --checks and a lint job with fifty steps: which of them a given
edit trips was a table in the brief, kept by hand, and wrong by the time it
was read. This script derives the answer from the workflows themselves —
their `pull_request` path filters, read with the same matcher the
path-filter-reads gate uses (scripts/check_path_filter_reads.py), and their
steps' `run:` blocks — so there is nothing to keep.

WHAT IS FAST. A step is runnable here when it has a `run:` (not `uses:`),
no `if:` (a condition this script cannot evaluate), and every line of it,
install lines (`pip install …`) set aside, starts with python3, node, bash or
sh and names no toolchain, runner or secret (cargo, pio, arduino-cli,
xcodebuild, docker, emcc, npm, apt-get, $GITHUB_*, ${{ secrets }}). Every
other step is listed as manual, with its command, so nothing is hidden —
the list says what CI will do, --run says what you can prove before pushing.

REFUSES (exit 2): a workflow this script cannot parse — a gate list that
silently drops a workflow is a wrong answer — and a diff with no base to
compare against, unless --paths names the paths.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_path_filter_reads import pr_filter, triggers_of  # noqa: E402

REPO = Path(__file__).resolve().parents[1]

FAST_LINE = re.compile(r"^(python3|node|bash|sh)\s+\S")
INSTALL_LINE = re.compile(r"^(python3 -m pip install|pip3? install)\b")
NOT_HERE = re.compile(
    r"\b(cargo|pio|platformio|arduino-cli|xcodebuild|xcrun|docker|emcc|em\+\+|npm|npx|"
    r"apt-get|rustup|curl|wget|gh)\b|\$\{?(GITHUB|RUNNER)_|\$\{\{"
)


class Step:
    __slots__ = ("workflow", "job", "name", "lines", "cwd", "env", "kind", "why")

    def __init__(self, workflow: str, job: str, step: dict):
        self.workflow, self.job = workflow, job
        self.name = str(step.get("name") or step.get("run", "").strip().splitlines()[0])[:70]
        self.lines = [ln.rstrip() for ln in str(step.get("run", "")).strip().splitlines()
                      if ln.strip() and not ln.strip().startswith("#")]
        self.cwd = str(step.get("working-directory") or "").strip("/")
        self.env = {k: str(v) for k, v in (step.get("env") or {}).items()
                    if "${{" not in str(v)}
        self.kind, self.why = self._classify(step)

    def _classify(self, step: dict) -> tuple[str, str]:
        if step.get("if"):
            return "manual", "has an if: condition"
        body = [ln.strip() for ln in self.lines if not INSTALL_LINE.match(ln.strip())]
        if not body:
            return "manual", "nothing to run after its install lines"
        for ln in body:
            if not FAST_LINE.match(ln):
                return "manual", f"not a python3/node/bash/sh line: {ln[:50]}"
            m = NOT_HERE.search(ln)
            if m:
                return "manual", f"needs {m.group(0).strip()}"
        return "fast", ""

    @property
    def command(self) -> str:
        return "\n".join(ln for ln in self.lines if not INSTALL_LINE.match(ln.strip()))

    @property
    def installs(self) -> list[str]:
        """The install lines set aside: what a failure here may be missing."""
        return [ln.strip() for ln in self.lines if INSTALL_LINE.match(ln.strip())]

    @property
    def key(self) -> tuple[str, str]:
        return (self.cwd, self.command)


class Workflow:
    def __init__(self, path: Path):
        self.path = path
        try:
            self.doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        except yaml.YAMLError as exc:
            refuse(f"cannot parse {path.name}: {exc}")
        self.name = str(self.doc.get("name") or path.stem)
        self.pr = "pull_request" in triggers_of(self.doc)
        self.filter = pr_filter(self.doc)
        self.steps = [Step(path.name, str(job), s)
                      for job, spec in (self.doc.get("jobs") or {}).items()
                      if isinstance(spec, dict)
                      for s in (spec.get("steps") or []) if isinstance(s, dict) and "run" in s]
        self.jobs = len(self.doc.get("jobs") or {})

    def triggered_by(self, paths: list[str]) -> str | None:
        """Why this workflow runs for these paths, or None."""
        if not self.pr:
            return None
        if self.filter is None:
            return "every PR"
        hits = [p for p in paths if self.filter.covers_file(p)]
        if not hits:
            return None
        return ("not ignored: " if self.filter.ignore else "matches: ") + hits[0] + (
            f" (+{len(hits) - 1})" if len(hits) > 1 else "")


def refuse(msg: str) -> None:
    print(f"gates.py: {msg}", file=sys.stderr)
    raise SystemExit(2)


def git(root: Path, *args: str) -> str | None:
    r = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=False)
    return r.stdout if r.returncode == 0 else None


def changed_paths(root: Path, base: str | None) -> tuple[list[str], str]:
    """Paths changed vs the merge-base with `base`, plus the working tree."""
    bases = [base] if base else ["origin/main", "main"]
    mb = next((git(root, "merge-base", b, "HEAD") for b in bases
               if git(root, "merge-base", b, "HEAD")), None)
    if mb is None:
        refuse(f"no base to diff against (tried {', '.join(bases)}); pass --base or --paths")
    out: set[str] = set()
    for args in (("diff", "--name-only", mb.strip()), ("diff", "--name-only"),
                 ("diff", "--name-only", "--cached"), ("ls-files", "--others", "--exclude-standard")):
        out.update(ln for ln in (git(root, *args) or "").splitlines() if ln)
    return sorted(out), bases[0] if base else next(b for b in bases if git(root, "merge-base", b, "HEAD"))


def run_step(root: Path, step: Step) -> tuple[bool, str]:
    cwd = root / step.cwd if step.cwd else root
    env = {**os.environ, **step.env}
    r = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", step.command], cwd=cwd, env=env,
                       capture_output=True, text=True, check=False)
    return r.returncode == 0, (r.stdout + r.stderr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", action="store_true", help="run every fast gate the diff trips")
    ap.add_argument("--all", action="store_true", help="list the manual steps too")
    ap.add_argument("--paths", nargs="+", metavar="PATH", help="use these paths instead of the git diff")
    ap.add_argument("--base", help="diff against this ref (default: origin/main, then main)")
    ap.add_argument("--root", type=Path, default=REPO, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    root = args.root.resolve()

    if args.paths:
        paths, against = sorted(set(args.paths)), "--paths"
    else:
        paths, against = changed_paths(root, args.base)
    workflows = [Workflow(p) for p in sorted((root / ".github" / "workflows").glob("*.yml"))]
    pr_workflows = [w for w in workflows if w.pr]

    print(f"gates.py — {len(paths)} changed path(s) vs {against}")
    for p in paths[:12]:
        print(f"  {p}")
    if len(paths) > 12:
        print(f"  … and {len(paths) - 12} more")

    triggered = [(w, w.triggered_by(paths)) for w in pr_workflows]
    triggered = [(w, why) for w, why in triggered if why]
    print(f"\nworkflows that will run: {len(triggered)} of {len(pr_workflows)} with a pull_request trigger")
    seen: set[tuple[str, str]] = set()
    fast: list[Step] = []
    manual: list[Step] = []
    for w, why in triggered:
        f = [s for s in w.steps if s.kind == "fast"]
        m = [s for s in w.steps if s.kind == "manual"]
        print(f"  {w.path.name:<34} {w.jobs:>2} job(s) · {len(f):>2} fast · {len(m):>2} manual   {why}")
        for s in w.steps:
            if s.key in seen:
                continue
            seen.add(s.key)
            (fast if s.kind == "fast" else manual).append(s)

    print(f"\nfast gates, runnable here: {len(fast)}" + ("" if args.run else "   (--run runs them)"))
    failed: list[Step] = []
    for s in fast:
        head = f"  {s.workflow} › {s.job} › {s.name}"
        if not args.run:
            print(head)
            print(f"      {s.command.splitlines()[0]}" + ("  …" if len(s.command.splitlines()) > 1 else ""))
            continue
        ok, out = run_step(root, s)
        print(f"  {'ok  ' if ok else 'FAIL'} {s.workflow} › {s.job} › {s.name}")
        if not ok:
            failed.append(s)
            tail = out.strip().splitlines()[-40:]
            print("       " + s.command.replace("\n", "\n       "))
            for ln in tail:
                print(f"       | {ln}")
            for ln in s.installs:
                print(f"       installs skipped here, run it once if that is what failed: {ln}")

    print(f"\nmanual steps (toolchain, runner or condition): {len(manual)}"
          + ("" if args.all else "   (--all lists them)"))
    if args.all:
        for s in manual:
            print(f"  {s.workflow} › {s.job} › {s.name}   [{s.why}]")
            print(f"      {s.command.splitlines()[0] if s.command else '(install only)'}")

    if args.run:
        print(f"\n{len(fast) - len(failed)} of {len(fast)} fast gates passed"
              + (f"; {len(failed)} FAILED: " + ", ".join(s.name for s in failed) if failed else ""))
        return 1 if failed else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
