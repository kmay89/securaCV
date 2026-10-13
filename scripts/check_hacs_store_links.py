#!/usr/bin/env python3
"""The HACS store page's links into this repository land in this checkout.

    python3 scripts/check_hacs_store_links.py            # fetch the mirror's main
    python3 scripts/check_hacs_store_links.py --from DIR # a local mirror checkout

WHY. The HACS store page is the mirror's README.md
(kmay89/securacv-homeassistant, mirror-owned), and it sends readers here by
absolute URL: into docs/ (most of them to a heading), to
scripts/install.sh (piped to bash) and to the mirror workflow. The mirror's
own link check (its HA31 step, `lint_readme.py --source`) reads this
repository's main once a week, so a PR here that renames a linked heading or
moves a linked file merges green and the store page is broken before
anything goes red (sweep HA35). This runs the mirror's own checker against
the PR's checkout, so the PR that breaks a link is the one that fails. The
mirror's weekly step stays as the backstop for links this check cannot see
coming (a store-page edit that links a path that does not exist yet).

WHAT. It fetches four files from the mirror's main, read-only and without
credentials (the repository is public, as HACS requires): README.md,
AGENTS.md and CLAUDE.md (the mirror-owned prose the checker reads) and
.github/scripts/lint_readme.py (the checker), lays them out as the mirror
has them, and runs `lint_readme.py --source <this checkout>`. Its exit code
is this script's. The checker is the mirror's on purpose: one slugger, one
URL parser and one rule set for both repositories, so the two checks cannot
disagree about what lands.

The checker gained `--source` in securacv-homeassistant#28. Until that merges,
the mirror's main carries a lint_readme.py without it, which would ignore the
flag and lint the fetched prose's RELATIVE links against an empty directory.
So a checker with no --source mode is not run: this prints a GitHub warning
naming the gap and exits 0. That is the only path that passes without
checking, and it says so in the job log and as an annotation.

Exit codes: 0 = every link lands (or the mirror's checker predates --source,
warned); 1 = a link does not land, or the checker failed its own probes;
2 = the mirror's files could not be fetched or read.

CI:   .github/workflows/lint.yml (job hacs-store-page-links)
Test: scripts/tests/test_check_hacs_store_links.py
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MIRROR = "kmay89/securacv-homeassistant"
RAW = "https://raw.githubusercontent.com/{repo}/{ref}/{path}"
PROSE = ("README.md", "AGENTS.md", "CLAUDE.md")
CHECKER = ".github/scripts/lint_readme.py"
FILES = PROSE + (CHECKER,)
# The literal the checker's argparse registers; a checker without it is the
# pre-#28 one, which ignores --source.
SOURCE_FLAG = '"--source"'
PREDATES = ("::warning title=HACS store page links not checked::the mirror's "
            f"{CHECKER} on {{ref}} has no --source mode yet "
            "(securacv-homeassistant#28 adds it), so the store page's links "
            "into this repository were not checked")


def fetch(ref: str, dest: Path, attempts: int = 3) -> str | None:
    """Download FILES at `ref` into `dest`; None on success, else why not."""
    for path in FILES:
        url = RAW.format(repo=MIRROR, ref=ref, path=path)
        for attempt in range(1, attempts + 1):
            try:
                with urllib.request.urlopen(url, timeout=30) as r:
                    data = r.read()
                break
            except (urllib.error.URLError, OSError) as e:
                if attempt == attempts:
                    return f"{url}: {e}"
                time.sleep(2 * attempt)
        out = dest / path
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
    return None


def copy_from(src: Path, dest: Path) -> str | None:
    """Copy FILES from a local mirror checkout; None on success."""
    for path in FILES:
        f = src / path
        if not f.is_file():
            return f"{f}: no such file"
        out = dest / path
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, out)
    return None


def run_checker(mirror: Path, source: Path, ref: str) -> int:
    checker = mirror / CHECKER
    if SOURCE_FLAG not in checker.read_text(encoding="utf-8"):
        print(PREDATES.format(ref=ref))
        return 0
    # -I: the fetched script is run isolated (no user site, no PYTHON* env,
    # no current-directory imports), as untrusted downloads are here.
    return subprocess.run(
        [sys.executable, "-I", str(checker), "--source", str(source)],
        check=False).returncode


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ref", default="main",
                    help="the mirror's branch to fetch (default: main)")
    ap.add_argument("--from", dest="local", type=Path, metavar="DIR",
                    help="a local mirror checkout instead of fetching")
    ap.add_argument("--source", type=Path, default=REPO,
                    help="the checkout the links must land in (default: "
                         "this repository)")
    args = ap.parse_args(argv)
    with tempfile.TemporaryDirectory(prefix="hacs-mirror-") as tmp:
        mirror = Path(tmp)
        why = (copy_from(args.local, mirror) if args.local is not None
               else fetch(args.ref, mirror))
        if why:
            print(f"check_hacs_store_links.py: could not read the mirror's "
                  f"files: {why}", file=sys.stderr)
            return 2
        ref = str(args.local) if args.local is not None else args.ref
        return run_checker(mirror, args.source.resolve(), ref)


if __name__ == "__main__":
    raise SystemExit(main())
