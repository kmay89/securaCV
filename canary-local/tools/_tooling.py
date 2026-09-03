#!/usr/bin/env python3
"""Shared plumbing for the canary-local/tools generators — ONE behavior each.

Every generator in this directory used to carry its own copy of `die()` (ten
of them, three different behaviors: with and without an `ERROR:` token, with
and without a second "drift-gated" hint line, `sys.exit(str)` vs
`SystemExit(1)`) and two of `_warn()`. This module is the single definition.

  die(msg, code=1)
      `<prog>: ERROR: <msg>` on stderr, then — only when GITHUB_ACTIONS is
      set — a `::error::<prog>: <msg>` workflow command on stdout, so the
      failure surfaces as a red annotation on the PR (the same stream the
      existing `::warning::` annotations in this directory use). Exits with
      `code`, default 1. A caller that needs a distinct exit status passes it
      explicitly, so the status is always visible at the call site; none does
      today. `<prog>` is the basename of the module that called die() —
      `gen_vision.py`, not whatever wrapper imported it — so the line a person
      reads in the log names the generator that actually failed.

  warn(msg)
      `<prog>: WARNING: <msg>` on stderr, plus a `::warning::` annotation
      under GITHUB_ACTIONS. Never exits.

  repo_root(levels=2, start=None)
      The repository root as a Path. Anchored on THIS file by default: it
      lives at canary-local/tools/_tooling.py, two levels below the root, so
      `repo_root()` is correct for every caller wherever that caller lives.
      `start` / `levels` let a script anchor on itself instead (a test in
      tools/tests/ is three deep: `repo_root(3, __file__)`). The result is
      checked either way — it must contain this very file — so a miscount
      fails here with a clear message, not three functions later with a
      "missing source" error that names the wrong path.

Importable as `from _tooling import die, warn, repo_root` because the scripts
run as `python3 canary-local/tools/<tool>.py`, which puts this directory at
sys.path[0]; the unit tests in tools/tests/ insert the same directory
themselves. NOT used by hub_seed_apply.py or hub_host_provision.sh: those are
embedded verbatim (include_str! in desktop/hub-io, sha256-pinned by
gen_hub_provision_bundle.py) and must stay self-contained.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import NoReturn

_HERE = Path(__file__).resolve()


def _prog(depth: int) -> str:
    """Basename of the module `depth` frames up the stack (the caller of the
    caller, for die/warn). Falls back to argv[0], then to a constant, so a
    frozen or interactive caller still gets a readable prefix."""
    try:
        frame = sys._getframe(depth)
        path = frame.f_globals.get("__file__")
    except (ValueError, AttributeError):
        path = None
    if not path:
        path = sys.argv[0] if sys.argv and sys.argv[0] else "tool"
    return Path(path).name


def _annotate(kind: str, prog: str, msg: str) -> None:
    if os.environ.get("GITHUB_ACTIONS"):
        # Workflow commands must be a single line; fold a multi-line message so
        # the runner does not truncate the annotation at the first newline.
        one_line = " ".join(msg.split())
        print(f"::{kind}::{prog}: {one_line}", flush=True)


def die(msg: str, code: int = 1) -> NoReturn:
    """Fail closed and loud: better a red build than a catalog that quietly lies."""
    prog = _prog(2)
    print(f"{prog}: ERROR: {msg}", file=sys.stderr)
    _annotate("error", prog, msg)
    raise SystemExit(code)


def warn(msg: str) -> None:
    prog = _prog(2)
    print(f"{prog}: WARNING: {msg}", file=sys.stderr)
    _annotate("warning", prog, msg)


def repo_root(levels: int = 2, start: str | os.PathLike[str] | None = None) -> Path:
    base = _HERE if start is None else Path(start).resolve()
    try:
        root = base.parents[levels]
    except IndexError:
        raise RuntimeError(
            f"repo_root: {base} has no ancestor {levels} levels up"
        ) from None
    marker = root / "canary-local" / "tools" / "_tooling.py"
    if not marker.is_file():
        raise RuntimeError(
            f"repo_root: {root} is not the securaCV checkout "
            f"(expected {marker.relative_to(root)} under it) — wrong `levels` for {base}?"
        )
    return root
