#!/usr/bin/env python3
"""scripts/lint_apple_parity.py — the Apple surfaces show a device the SAME way.

WHY THIS EXISTS. The Witness Wall (tvOS) is touched for a few hours every
several weeks. Everything about it that is not enforced drifts, and it drifted
in exactly the way an infrequently-visited target does — not by breaking, but
by quietly re-deciding things the iPhone had already decided:

  * it printed the raw wire string, so a television showed
    "canary-nightstand7" where the phone showed "Canary Nightstand 7";
  * it drew a colored dot where the phone drew the device itself;
  * it read `chain: "unknown"` as a verification FAILURE, painting every
    display in the fleet orange with "Record didn't verify" — a chain those
    devices never claimed to have.

Each was a second copy of a decision, made worse. The fix was to compile the
iPhone's files instead of re-deciding, and the fix STAYING fixed is this lint.

HOW IT WORKS — the set is DERIVED, never hand-listed here. A shared file opts
in by carrying the marker line:

    // SecuraCV-Parity: every Apple surface that shows a device compiles this.

This lint finds every marked file under ios/Shared and asserts that each Apple
target which renders devices compiles it. Adding a new shared file for the
phone therefore forces a decision instead of a silent divergence: add the
marker to share it, or leave the marker off to keep it iPhone-only. A list
maintained HERE would be one more thing to forget; a marker beside the code is
not.

The iPhone target is not checked file-by-file because `ios/project.yml`
includes the whole `Shared` directory — it cannot miss one by construction.
Targets that list files individually are the ones that can, and do.

Run:    python3 scripts/lint_apple_parity.py
Exit:   0 in sync; 1 with the exact missing lines to paste.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SHARED = REPO / "ios" / "Shared"
MARKER = "SecuraCV-Parity"

# Targets that render devices and list their sources file-by-file. Each entry
# is (project.yml, how that file spells a path into ios/Shared).
#
# `ios/project.yml` is deliberately absent: it takes `- path: Shared` wholesale,
# so it cannot omit a file. Add a row here when a new Apple target starts
# showing devices — a target that shows them and is not listed is the gap this
# whole file is about.
CONSUMERS = [
    (REPO / "tvos" / "WitnessWall" / "project.yml", "../../ios/Shared/{name}"),
]

# A target that COMPILES ios/Shared must also REBUILD when it changes, or the
# sharing quietly becomes a way to break that target without hearing about it.
# tvos.yml is path-filtered to "tvos/**", so before this check a change to a
# shared file ran no tvOS job at all — and the two files the Wall had shared
# since the beginning were never in the filter. Each entry is
# (workflow, the path glob it must watch).
WORKFLOW_TRIGGERS = [
    (REPO / ".github" / "workflows" / "tvos.yml", "ios/Shared/**"),
]


def marked_files() -> list[Path]:
    """Every shared file that has declared itself part of the parity set."""
    out = []
    for path in sorted(SHARED.glob("*.swift")):
        if MARKER in path.read_text(encoding="utf-8"):
            out.append(path)
    return out


def main() -> int:
    marked = marked_files()
    if not marked:
        print(f"::error::no file under ios/Shared carries the {MARKER} marker — "
              "either the marker was renamed or the parity set was deleted. "
              "This lint cannot pass vacuously.")
        return 1

    failed = False
    for project, spelling in CONSUMERS:
        if not project.exists():
            print(f"::error::{project.relative_to(REPO)} does not exist, but this "
                  "lint expects it to consume the parity set. Remove the row from "
                  "CONSUMERS if the target is gone.")
            failed = True
            continue

        text = project.read_text(encoding="utf-8")
        missing = []
        for shared in marked:
            wanted = spelling.format(name=shared.name)
            # Match the path as a source entry, tolerating YAML spacing.
            if not re.search(rf"^\s*-\s*path:\s*{re.escape(wanted)}\s*$", text,
                             re.MULTILINE):
                missing.append(wanted)

        rel = project.relative_to(REPO)
        if missing:
            failed = True
            print(f"::error::{rel} does not compile {len(missing)} file(s) of the "
                  "shared device vocabulary, so it will describe devices in its "
                  "own words and drift from the iPhone app.")
            print("         Add these under that target's `sources:` —")
            for m in missing:
                print(f"           - path: {m}")
            print("         Or, if the file genuinely should not be shared, remove "
                  f"its `{MARKER}` marker and say why in the file.")
        else:
            print(f"{rel}: compiles all {len(marked)} parity files ✅")

    for workflow, glob in WORKFLOW_TRIGGERS:
        rel = workflow.relative_to(REPO)
        if not workflow.exists():
            print(f"::error::{rel} is missing but is expected to watch {glob}.")
            failed = True
            continue
        text = workflow.read_text(encoding="utf-8")
        # Both the push and pull_request filters need it; count rather than
        # search, since watching only one half still leaves a blind side.
        hits = len(re.findall(rf'^\s*-\s*"{re.escape(glob)}"\s*$', text, re.MULTILINE))
        if hits < 2:
            failed = True
            print(f"::error::{rel} compiles files from {glob} but only {hits} of its "
                  "2 path filters (push and pull_request) watch that directory. A "
                  "change to a shared file would then run no job for this target, "
                  "and the break would surface weeks later.")
            print(f'         Add `- "{glob}"` under BOTH `on.push.paths` and '
                  "`on.pull_request.paths`.")
        else:
            print(f"{rel}: rebuilds when {glob} changes ✅")

    if failed:
        return 1
    names = ", ".join(p.name for p in marked)
    print(f"Apple parity OK — the device vocabulary ({names}) is compiled by "
          "every surface that shows a device.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
