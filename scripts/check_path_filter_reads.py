#!/usr/bin/env python3
"""Every file a filtered workflow's tests read is inside its path filter.

A workflow with a `pull_request: paths:` filter runs only when a PR touches
one of those paths. A test in it that reads a file outside the filter is a
test that a PR editing that file never runs: the PR goes green, and the red
lands on the next unrelated PR that happens to trigger the workflow. The
canary-local logic tests had 19 such reads on 4e77f0d, 7 of them opened by
the two PRs just before, and nothing noticed until a hand audit (sweep CI2).
Hand-kept lists drift; this gate reads what the tests actually read.

How the reads are collected: the CI step exports three variables and runs
its suites unchanged —

  PATH_FILTER_READS_DIR  a directory outside the checkout ($RUNNER_TEMP/...)
  NODE_OPTIONS           --require <abs>/scripts/path_filter_reads/record.cjs
  PYTHONPATH             <abs>/scripts/path_filter_reads   (its sitecustomize)

and every node and python3 process in those steps — the test files, and the
generators they spawn — appends the repo-relative paths it opened, stat'ed,
listed or checked for existence to a JSONL file there. This script then
matches each path against the workflow's `pull_request` paths, with the
pattern semantics GitHub documents for path filters:

  *    any run of characters except `/`       **   any run, `/` included
  **/  zero or more whole directories         ?    the previous char, optional
  +    the previous char, one or more         [..] one of the listed chars
  !x   (first char) re-excludes what the patterns before it included
  \\x   a literal x

A path is covered when the last pattern that matches it is not a `!` one.
A DIRECTORY (listed, or stat'ed and a directory on disk) is covered when some
pattern can reach inside it: a test that lists a directory usually filters
the names, and the files it then reads are held to the filter one by one.

It fails, naming the test, the file, the test line that read it and the line
to add to BOTH path lists (R6 in .github/CI.md keeps push equal to
pull_request; .github/scripts/ci_policy_check.py enforces that half). It
also fails when a suite the job runs left no record at all — a recorder that
was never armed must not read as "no reads outside the filter".

A read of Python bytecode is charged to its source (a read of
`x/__pycache__/m.cpython-311.pyc` is a read of `x/m.py`). Once a module is
cached, the import system stats the .py and opens only the .pyc — and the
job's own drift steps write those caches before its test step runs, so
allowing `__pycache__/` as "the job's output" would have hidden
`scripts/bom_pricing.py`, a real input.

The only paths it may let through unlisted are the job's own outputs (ALLOW
below, each with its reason; it is empty today). Paths outside the checkout
(tmp dirs, the interpreters) are never recorded in the first place.

Run locally:
  d=$(mktemp -d)
  PATH_FILTER_READS_DIR=$d NODE_OPTIONS="--require $PWD/scripts/path_filter_reads/record.cjs" \\
    PYTHONPATH=$PWD/scripts/path_filter_reads node --test canary-local/tests/wap.test.js
  python3 scripts/check_path_filter_reads.py --workflow .github/workflows/canary-local.yml \\
    --job logic-tests --reads "$d" --suite canary-local/tests/wap.test.js
CI:  canary-local.yml (logic-tests), after the suites, on the same recordings.
Tests: scripts/tests/test_check_path_filter_reads.py
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shlex
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[1]

# The job's own outputs: files the job writes and then reads back, which no
# PR edits (they are not committed), as (pattern, reason). Nothing else
# belongs here — a real input outside the filter is a line in the filter, not
# an allowlist entry. Empty today: the tmp dirs the tests use are outside the
# checkout, and bytecode caches are charged to their sources (source_of).
ALLOW: list[tuple[str, str]] = []

# x/__pycache__/m.cpython-311.pyc, m.cpython-311.opt-1.pyc -> x/m.py (PEP 3147:
# a cache in __pycache__ is only ever used next to its source).
_PYC_RE = re.compile(r"^(?P<dir>(?:.*/)?)__pycache__/(?P<mod>[^/.]+)\.[^/]+\.pyc$")


def source_of(path: str) -> str:
    """The file a read really depends on: a bytecode cache's source, else itself."""
    m = _PYC_RE.match(path)
    return f"{m['dir']}{m['mod']}.py" if m else path


# fs calls that look at a directory's listing rather than a file's bytes.
LISTING_OPS = {"readdirSync", "readdir", "opendirSync", "opendir", "listdir"}


# --- GitHub path-filter patterns --------------------------------------------

def _pattern_regex(pattern: str) -> re.Pattern[str]:
    """One filter pattern as an anchored regex (see the module docstring)."""
    out: list[str] = []
    i, n = 0, len(pattern)
    while i < n:
        c = pattern[i]
        if c == "\\" and i + 1 < n:
            out.append(re.escape(pattern[i + 1]))
            i += 2
        elif pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c in "?+" and out:
            out[-1] = f"(?:{out[-1]}){c}"
            i += 1
        elif c == "[" and pattern.find("]", i + 2) > i:
            j = pattern.find("]", i + 2)
            body = "".join(ch if ch.isalnum() or ch == "-" else re.escape(ch)
                           for ch in pattern[i + 1:j])
            out.append(f"[{body}]")
            i = j + 1
        else:
            out.append(re.escape(c))
            i += 1
    return re.compile("".join(out), re.DOTALL)


def _reaches_into(dir_segs: list[str], pat_segs: list[str]) -> bool:
    """Can a pattern (split on `/`) match some path strictly inside the
    directory `dir_segs`? A segment holding `**` can absorb any depth, so
    once one is reached the answer is yes."""
    for k, seg in enumerate(dir_segs):
        if k >= len(pat_segs):
            return False
        p = pat_segs[k]
        if "**" in p:
            return True
        if not _pattern_regex(p).fullmatch(seg):
            return False
    return len(pat_segs) > len(dir_segs)


class PathFilter:
    """A workflow trigger's `paths` (or `paths-ignore`) list, as GitHub reads it."""

    def __init__(self, patterns: list[str], ignore: bool = False):
        self.patterns = list(patterns)
        self.ignore = ignore
        self._compiled = []
        for p in self.patterns:
            neg = p.startswith("!")
            body = p[1:] if neg else p
            self._compiled.append((neg, body, _pattern_regex(body)))

    def _last_match(self, path: str) -> bool | None:
        verdict = None
        for neg, _body, rx in self._compiled:
            if rx.fullmatch(path):
                verdict = not neg
        return verdict

    def covers_file(self, path: str) -> bool:
        hit = bool(self._last_match(path))
        return not hit if self.ignore else hit

    def covers_dir(self, path: str) -> bool:
        """Can a change inside directory `path` trigger the workflow at all?"""
        probe = path.rstrip("/") + "/\0"  # "\0" never occurs in a real name
        if self.ignore:
            # Ignored only when every path under it is, which only a
            # `<dir>/**`-style pattern says.
            return not self._last_match(probe)
        if self.covers_file(probe):
            return True
        segs = path.strip("/").split("/")
        return any(not neg and _reaches_into(segs, body.split("/"))
                   for neg, body, _rx in self._compiled)


def allowed(path: str) -> str | None:
    """The reason a path needs no filter line, or None."""
    for pattern, why in ALLOW:
        if _pattern_regex(pattern).fullmatch(path):
            return why
    return None


# --- the workflow -----------------------------------------------------------

def triggers_of(wf: dict) -> dict:
    # PyYAML (YAML 1.1) reads the bare key `on` as the boolean True.
    on = wf.get("on", wf.get(True))
    if isinstance(on, str):
        return {on: {}}
    if isinstance(on, list):
        return {t: {} for t in on}
    return on or {}


def pr_filter(wf: dict) -> PathFilter | None:
    pr = triggers_of(wf).get("pull_request")
    if not isinstance(pr, dict):
        return None
    if pr.get("paths"):
        return PathFilter([str(p) for p in pr["paths"]])
    if pr.get("paths-ignore"):
        return PathFilter([str(p) for p in pr["paths-ignore"]], ignore=True)
    return None


def filter_anchor_line(text: str) -> int:
    """The line of the last `pull_request: paths:` entry (1-based), for the
    annotation — the place a reader adds the line; 1 when it can't be found."""
    lines = text.splitlines()
    in_pr = in_paths = False
    pr_indent = paths_indent = -1
    last = 0
    for idx, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if re.match(r"^\s*pull_request\s*:", line):
            in_pr, pr_indent, in_paths = True, indent, False
            continue
        if in_pr and indent <= pr_indent:
            break
        if in_pr and re.match(r"^\s*paths(-ignore)?\s*:", line):
            in_paths, paths_indent = True, indent
            continue
        if in_paths:
            if indent <= paths_indent and not stripped.startswith("-"):
                in_paths = False
                continue
            if stripped.startswith("-"):
                last = idx
    return last or 1


def job_suites(wf: dict, job: str) -> tuple[list[str], list[tuple[str, str]]]:
    """(node --test files, [(unittest discover dir, pattern)]) the job runs."""
    jobs = wf.get("jobs") or {}
    if job not in jobs:
        raise SystemExit(f"::error::no job `{job}` in the workflow")
    node: list[str] = []
    discover: list[tuple[str, str]] = []
    for step in jobs[job].get("steps") or []:
        run = step.get("run") if isinstance(step, dict) else None
        if not isinstance(run, str):
            continue
        for line in run.replace("\\\n", " ").splitlines():
            try:
                words = shlex.split(line, comments=True)
            except ValueError:
                continue
            if len(words) >= 3 and words[0] == "node" and "--test" in words:
                node.extend(w for w in words[words.index("--test") + 1:]
                            if not w.startswith("-"))
            if words[:4] in (["python3", "-m", "unittest", "discover"],
                             ["python", "-m", "unittest", "discover"]):
                opts = words[4:]
                start, pat = ".", "test*.py"
                for k, w in enumerate(opts):
                    if w in ("-s", "--start-directory") and k + 1 < len(opts):
                        start = opts[k + 1]
                    if w in ("-p", "--pattern") and k + 1 < len(opts):
                        pat = opts[k + 1]
                discover.append((os.path.normpath(start).replace(os.sep, "/"), pat))
    return node, discover


def expected_files(repo: Path, node: list[str],
                   discover: list[tuple[str, str]]) -> list[str]:
    files = [os.path.normpath(f).replace(os.sep, "/") for f in node]
    for start, pat in discover:
        d = repo / start
        if d.is_dir():
            files.extend(f"{start}/{name}" for name in sorted(os.listdir(d))
                         if fnmatch.fnmatch(name, pat) and (d / name).is_file())
    return files


# --- the recordings ---------------------------------------------------------

def load_records(reads_dir: Path) -> list[dict]:
    records = []
    for f in sorted(reads_dir.glob("*.jsonl")):
        with open(f, encoding="utf-8") as fh:
            for n, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    # A process killed mid-write leaves a torn last line.
                    print(f"::warning::{f.name}:{n}: unreadable record skipped")
                    continue
                if isinstance(rec, dict) and isinstance(rec.get("path"), str):
                    rec["path"] = source_of(rec["path"])
                    records.append(rec)
    return records


def check(repo: Path, workflow: Path, job: str, reads_dir: Path,
          only: list[str] | None = None) -> tuple[list[str], str]:
    """(problems, summary). Each problem is one ready-to-print message."""
    text = workflow.read_text(encoding="utf-8")
    wf = yaml.safe_load(text)
    if not isinstance(wf, dict):
        return [f"::error::{workflow} is not a workflow mapping"], ""
    flt = pr_filter(wf)
    wf_rel = os.path.relpath(workflow, repo).replace(os.sep, "/")
    anchor = filter_anchor_line(text)
    node, discover = job_suites(wf, job)
    expected = expected_files(repo, node, discover)
    if only:
        expected = [f for f in expected if f in only]
    records = load_records(reads_dir)
    problems: list[str] = []

    heard = {r.get("path") for r in records} | {r.get("suite") for r in records}
    for f in expected:
        if f not in heard:
            problems.append(
                f"::error file={wf_rel}::{f} ran in `{job}` but left no read "
                f"record — the recorder was not armed for it (the step needs "
                f"PATH_FILTER_READS_DIR, NODE_OPTIONS and PYTHONPATH, see "
                f"scripts/check_path_filter_reads.py), so its reads went unchecked"
            )
    if not expected:
        problems.append(f"::error file={wf_rel}::`{job}` runs no suite this "
                        f"gate recognizes — nothing to check is not a pass")

    if flt is None:
        return problems, f"{wf_rel} has no pull_request path filter; every read is covered"

    misses: dict[str, list[dict]] = {}
    allowed_n = 0
    paths = set()
    for rec in records:
        path = rec["path"]
        paths.add(path)
        if allowed(path):
            allowed_n += 1
            continue
        is_dir = rec.get("op") in LISTING_OPS or (repo / path).is_dir()
        ok = flt.covers_dir(path) if is_dir else flt.covers_file(path)
        if not ok:
            misses.setdefault(path, []).append(rec)

    for path in sorted(misses):
        recs = misses[path]
        is_dir = any(r.get("op") in LISTING_OPS for r in recs) or (repo / path).is_dir()
        line = f'- "{path}/**"' if is_dir else f'- "{path}"'
        by_suite: dict[str, dict] = {}
        for r in recs:
            by_suite.setdefault(r.get("suite") or "?", r)
        who = "; ".join(
            f"{s} ({r.get('op')}{' at ' + r['at'] if r.get('at') else ''}"
            f"{', via ' + r['proc'] if r.get('proc') and r.get('proc') != s else ''})"
            for s, r in sorted(by_suite.items()))
        problems.append(
            f"::error file={wf_rel},line={anchor}::{path} is read by {who}, "
            f"but the pull_request path filter does not cover it, so a PR "
            f"that edits it never runs this job. Add   {line}   to BOTH "
            f"on.push.paths and on.pull_request.paths (R6), with a comment "
            f"naming the reader"
        )
    outputs = f" ({allowed_n} reads of the job's own outputs allowed)" if allowed_n else ""
    summary = (f"{len(expected)} suites recorded, {len(paths)} distinct repo "
               f"paths read, all inside {wf_rel}'s pull_request filter{outputs}")
    return problems, summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--workflow", required=True, type=Path)
    ap.add_argument("--job", required=True)
    ap.add_argument("--reads", required=True, type=Path,
                    help="the PATH_FILTER_READS_DIR the suites recorded into")
    ap.add_argument("--suite", action="append", default=None,
                    help="only expect these suites (a local run of a few)")
    args = ap.parse_args(argv)
    workflow = args.workflow if args.workflow.is_absolute() else Path.cwd() / args.workflow
    if not args.reads.is_dir():
        print(f"::error::{args.reads} is not a directory — the recorder never ran")
        return 1
    problems, summary = check(REPO, workflow, args.job, args.reads, args.suite)
    for p in problems:
        print(p)
    if problems:
        print(f"\n{len(problems)} problem(s). Why this gate exists and how the "
              f"reads are recorded: scripts/check_path_filter_reads.py")
        return 1
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
