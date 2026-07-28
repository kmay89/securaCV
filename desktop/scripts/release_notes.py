#!/usr/bin/env python3
"""Each desktop app says WHAT a version changes, in words a user can read.

The version files name a release; they don't explain it. The explanation
lives in one place per app — `RELEASE_NOTES.md`, newest first, one
`## <version> — <YYYY-MM-DD>` section per released version — and flows,
verbatim, everywhere a human meets the release:

    the GitHub release body      (what the download page says it changes)
    the updater manifest `notes` (what the in-app "update ready" UI shows)

Two modes:

    release_notes.py check
        The newest section of each app's RELEASE_NOTES.md must match the
        version in its tauri.conf.json, with a non-empty body, and no
        version may appear twice. Run by lint on every PR and by both app
        release workflows before they build — bumping the version and
        writing the notes are one act, exactly like the three version
        files (check_app_versions.py).

    release_notes.py extract <flasher|lab> <version>
        Print that version's section body (no heading) to stdout, for the
        workflows to publish. A missing section is a hard error: a release
        that can't say what it changes isn't ready to ship.

Run locally:  python3 desktop/scripts/release_notes.py check
Self-test:    python3 desktop/scripts/release_notes.py --self-test
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# app id -> (its notes file, the tauri.conf.json its newest entry must match)
APPS: dict[str, tuple[str, str]] = {
    "flasher": ("desktop/RELEASE_NOTES.md", "desktop/src-tauri/tauri.conf.json"),
    "lab": ("desktop-lab/RELEASE_NOTES.md", "desktop-lab/src-tauri/tauri.conf.json"),
}

# `## 0.3.5 — 2026-07-28` (em dash; semver may carry a -rc.1 style suffix).
HEADING = re.compile(
    r"^##\s+(\d+\.\d+\.\d+(?:-[0-9A-Za-z.\-]+)?)\s+—\s+(\d{4}-\d{2}-\d{2})\s*$",
    re.MULTILINE,
)


def parse_sections(text: str) -> list[tuple[str, str, str]]:
    """[(version, date, body)] in file order (newest first by contract)."""
    matches = list(HEADING.finditer(text))
    sections = []
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        sections.append((m.group(1), m.group(2), text[m.end():end].strip()))
    return sections


def check_app(app: str, notes_rel: str, conf_rel: str) -> list[str]:
    notes_path = REPO / notes_rel
    conf_path = REPO / conf_rel
    try:
        current = json.loads(conf_path.read_text(encoding="utf-8"))["version"]
    except (OSError, json.JSONDecodeError, KeyError):
        return [f"{app}: cannot read a version out of {conf_rel}."]
    try:
        sections = parse_sections(notes_path.read_text(encoding="utf-8"))
    except OSError:
        return [
            f"{app}: {notes_rel} is missing — every released version must say "
            "what it changes. Add a `## <version> — <YYYY-MM-DD>` section."
        ]
    if not sections:
        return [
            f"{app}: {notes_rel} has no `## <version> — <YYYY-MM-DD>` sections; "
            f"the release workflows publish the newest one as the release body "
            f"and updater notes."
        ]
    problems: list[str] = []
    versions = [v for v, _, _ in sections]
    dupes = sorted({v for v in versions if versions.count(v) > 1})
    if dupes:
        problems.append(
            f"{app}: {notes_rel} repeats version section(s) {', '.join(dupes)} — "
            "one section per version, newest first."
        )
    newest_ver, _, newest_body = sections[0]
    if newest_ver != current:
        problems.append(
            f"{app}: {conf_rel} says {current}, but the newest section in "
            f"{notes_rel} is {newest_ver}. Bumping the version and writing its "
            "notes are one act — add a section for "
            f"{current} at the top (what the user gets, not the diff)."
        )
    elif not newest_body:
        problems.append(
            f"{app}: the {newest_ver} section in {notes_rel} is empty — a "
            "release that can't say what it changes isn't ready to ship."
        )
    if not problems:
        print(f"  {app}: notes for {current} present ✓")
    return problems


def cmd_check() -> int:
    print("Release notes (newest RELEASE_NOTES.md section = tauri.conf.json version):")
    problems: list[str] = []
    for app, (notes_rel, conf_rel) in APPS.items():
        problems.extend(check_app(app, notes_rel, conf_rel))
    for p in problems:
        print(f"::error::{p}")
    return 1 if problems else 0


def cmd_extract(app: str, version: str) -> int:
    if app not in APPS:
        raise SystemExit(f"unknown app {app!r} — one of: {', '.join(APPS)}")
    notes_rel, _ = APPS[app]
    text = (REPO / notes_rel).read_text(encoding="utf-8")
    for ver, _, body in parse_sections(text):
        if ver == version:
            print(body)
            return 0
    raise SystemExit(
        f"{notes_rel} has no section for {version} — write one "
        "(`## " + version + " — <YYYY-MM-DD>`) before releasing it."
    )


def _self_test() -> int:
    sample = (
        "# X — release notes\n\npreamble\n\n"
        "## 1.2.3 — 2026-07-28\n\n- **New thing.** It helps.\n\n"
        "## 1.2.2 — 2026-07-20\n\n- Old thing.\n"
    )
    secs = parse_sections(sample)
    assert [s[0] for s in secs] == ["1.2.3", "1.2.2"], secs
    assert secs[0][1] == "2026-07-28"
    assert secs[0][2] == "- **New thing.** It helps."
    assert secs[1][2] == "- Old thing."
    # a prerelease-suffixed heading parses too
    pre = parse_sections("## 2.0.0-rc.1 — 2026-01-01\n\nbody\n")
    assert pre and pre[0][0] == "2.0.0-rc.1", pre
    # a malformed heading (hyphen, not em dash) is NOT a section
    assert parse_sections("## 1.0.0 - 2026-01-01\n\nbody\n") == []
    print("release_notes self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    if not argv:
        raise SystemExit(__doc__)
    if argv[0] == "--self-test":
        return _self_test()
    if argv[0] == "check" and len(argv) == 1:
        return cmd_check()
    if argv[0] == "extract" and len(argv) == 3:
        return cmd_extract(argv[1], argv[2])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
