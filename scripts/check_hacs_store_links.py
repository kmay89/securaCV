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
has them, and runs `lint_readme.py --source <this checkout>`. The checker is
the mirror's on purpose: one slugger, one URL parser and one rule set for
both repositories, so the two checks cannot disagree about what lands.

FAIL CLOSED. Nothing here passes without the checker saying, in its own
words, that it checked the links: an exit 0 counts only with its summary
line `links into kmay89/securaCV OK — <n> resolve ...` (n at least 1) on
stdout. A checker that exits 0 without it (one older than
securacv-homeassistant#28, which ignores --source and lints the prose
instead; an empty or truncated download; a reworded summary) is exit 2, not
a pass. A file that cannot be fetched, or comes back empty, is exit 2.

RENAMING OR MOVING SOMETHING THE STORE PAGE LINKS, on purpose. Keep the old
name alive in the same PR: an explicit `<a id="old-slug"></a>` on the line
above the renamed heading (the checker accepts an HTML id or name anchor,
as GitHub does), or the old file left in place pointing at the new one.
This job then passes against the mirror's main as it stands. After the PR
merges, move the store page's link in a mirror PR (its link step checks
against this repository's main, which now has both names), and drop the
alias in a later PR once the mirror's main links the new name. Without the
alias there is no order that keeps everything green: merging the mirror PR
first breaks the live store page and fails this job on every other PR and
main push until the rename merges; merging the rename first does the same
until the mirror PR merges.

Exit codes: 0 = the checker vouched for every link; 1 = a link does not
land, or the checker failed its own probes (any non-zero exit of the
checker other than 2); 2 = the mirror's files could not be fetched or read,
or the checker did not say it checked the links.

CI:   .github/workflows/lint.yml (job hacs-store-page-links)
Test: scripts/tests/test_check_hacs_store_links.py
"""
from __future__ import annotations

import argparse
import re
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
ATTEMPTS = 3
RETRY_PAUSE = 2  # seconds, times the attempt number
# The checker's --source summary line (lint_upstream() in the mirror's
# lint_readme.py). Its words are the contract: an exit 0 without this line
# is not a pass.
VOUCHED = re.compile(r"^links into kmay89/securaCV OK\W+(\d+) resolve\b",
                     re.MULTILINE)
HINT = ("check_hacs_store_links.py: a link on the HACS store page no longer "
        "lands in this checkout. If the rename or move is on purpose, keep "
        "the old name alive in this PR (an <a id=\"old-slug\"></a> on the "
        "line above the renamed heading, or the old file left in place), "
        "then move the store page's link in a mirror PR after this one "
        "merges; lint.yml's hacs-store-page-links comment has the order.")


def fetch(ref: str, dest: Path) -> str | None:
    """Download FILES at `ref` into `dest`; None on success, else why not."""
    for path in FILES:
        url = RAW.format(repo=MIRROR, ref=ref, path=path)
        data = b""
        for attempt in range(1, ATTEMPTS + 1):
            try:
                with urllib.request.urlopen(url, timeout=30) as r:
                    data = r.read()
                if data:
                    break
                why = "an empty body"
            except (urllib.error.URLError, OSError) as e:
                why = str(e)
            if attempt == ATTEMPTS:
                return f"{url}: {why} (after {ATTEMPTS} tries)"
            time.sleep(RETRY_PAUSE * attempt)
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
        if f.stat().st_size == 0:
            return f"{f}: empty"
        out = dest / path
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, out)
    return None


def run_checker(mirror: Path, source: Path, ref: str) -> int:
    checker = mirror / CHECKER
    # -I: the fetched script is run isolated (no user site, no PYTHON* env,
    # no current-directory imports), as untrusted downloads are here. Its
    # stdout is read for the summary line and then passed on; its stderr
    # (the problem list) goes straight to the log.
    r = subprocess.run(
        [sys.executable, "-I", str(checker), "--source", str(source)],
        stdout=subprocess.PIPE, text=True, check=False)
    sys.stdout.write(r.stdout)
    sys.stdout.flush()
    if r.returncode != 0:
        if r.returncode != 2:
            print(HINT, file=sys.stderr)
        return r.returncode if r.returncode == 2 else 1
    m = VOUCHED.search(r.stdout)
    if m is None or int(m.group(1)) < 1:
        print(f"check_hacs_store_links.py: the mirror's {CHECKER} on {ref} "
              "exited 0 without saying it checked the store page's links "
              "into this repository (no 'links into kmay89/securaCV OK — <n> "
              "resolve' line, n at least 1): a checker older than "
              "securacv-homeassistant#28, an empty or truncated download, or "
              "a reworded summary. Nothing was vouched for, so this is not a "
              "pass.", file=sys.stderr)
        return 2
    return 0


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
