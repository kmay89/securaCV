#!/usr/bin/env python3
"""Print the body a release workflow would publish for a tag, by running that
workflow's own step.

Why it runs the step instead of rebuilding the text: a published release body
does not follow its sources. When a CHANGELOG section or a RELEASE_NOTES.md
section is corrected after the release, the page keeps the old words (the
2.4.16 train shipped five such bodies: a firmware body that ended mid-sentence,
two Flasher bodies that called it "the SecuraCV Lab", two Lab bodies that
promised "no quarantine warning"). Rewriting them takes the same text the
release workflow would write today, and a second copy of that text here would
drift from the workflow the first time either changed. So this reads the step
out of the workflow file and runs it, with `gh` replaced by a stand-in that
captures the body instead of editing anything.

  python3 .github/scripts/release_body.py <tag> [--signed] [--repo owner/name] > body.md

  fw-v<ver>       firmware-release.yml  "Extract release notes from CHANGELOG"
                  (the CHANGELOG section, capped below GitHub's body limit)
  flasher-v<ver>  desktop-flasher-release.yml  "Release body says what this version changes"
  app-v<ver>      desktop-release.yml  "Draft body says what this version changes"

--signed answers the app steps' SIGNED question (vars.ENABLE_MACOS_SIGNING in
the workflows): it picks the macOS install paragraph. The firmware step does
not read it.

Refuses a step that reads a `${{ }}` expression inside its script (only its
env block may, and this supplies that env), a step that calls `gh` for
anything but `release edit --notes-file`, and a tag of any other kind.

Run the tests:  python3 -m unittest discover -s .github/scripts -p 'test_*.py'
"""
from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]

# prefix -> (workflow, step name, version env var, how the step hands its body over)
STEPS = {
    "fw-v": ("firmware-release.yml", "Extract release notes from CHANGELOG", "VERSION", "file"),
    "flasher-v": ("desktop-flasher-release.yml", "Release body says what this version changes", "VER", "gh"),
    "app-v": ("desktop-release.yml", "Draft body says what this version changes", "VER", "gh"),
}

# The firmware step writes its body to this path; it is rewritten to a temp file.
FW_NOTES_PATH = "/tmp/release_notes.md"

# Answers `gh release edit <tag> ... --notes-file <f>` by copying <f> to
# $RELEASE_BODY_OUT; any other gh call is a step this tool was not built for.
GH_STUB = """#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = "release" ] && [ "${2:-}" = "edit" ]; then
  while [ $# -gt 0 ]; do
    if [ "$1" = "--notes-file" ]; then cp "$2" "$RELEASE_BODY_OUT"; exit 0; fi
    shift
  done
fi
echo "release_body.py: the step called 'gh $*', which this tool does not stand in for" >&2
exit 3
"""


class StepError(RuntimeError):
    pass


def kind_of(tag: str) -> tuple[str, str]:
    for prefix in STEPS:
        if tag.startswith(prefix) and len(tag) > len(prefix):
            return prefix, tag[len(prefix):]
    raise StepError(f"{tag!r} is not a release tag this tool knows (fw-v*, flasher-v*, app-v*)")


def step_script(workflow: str, step_name: str, root: Path = REPO_ROOT) -> str:
    doc = yaml.safe_load((root / ".github" / "workflows" / workflow).read_text(encoding="utf-8"))
    found = [
        s.get("run")
        for job in (doc.get("jobs") or {}).values()
        for s in (job.get("steps") or [])
        if s.get("name") == step_name
    ]
    if len(found) != 1 or not found[0]:
        raise StepError(f"{workflow}: expected one step named {step_name!r} with a run block, found {len(found)}")
    script = found[0]
    if "${{" in script:
        raise StepError(f"{workflow}: step {step_name!r} reads a ${{{{ }}}} expression in its script; "
                        "it must take everything through env for this tool to run it")
    return script


def compose(tag: str, *, signed: bool, repo: str, root: Path = REPO_ROOT) -> str:
    prefix, version = kind_of(tag)
    workflow, step_name, ver_var, handoff = STEPS[prefix]
    script = step_script(workflow, step_name, root)
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        out = tmpd / "body.md"
        bindir = tmpd / "bin"
        bindir.mkdir()
        gh = bindir / "gh"
        gh.write_text(GH_STUB, encoding="utf-8")
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        if handoff == "file":
            if FW_NOTES_PATH not in script:
                raise StepError(f"{workflow}: step {step_name!r} no longer writes {FW_NOTES_PATH}")
            script = script.replace(FW_NOTES_PATH, str(out))
        env = dict(os.environ)
        env.update({
            "PATH": f"{bindir}{os.pathsep}{env.get('PATH', '')}",
            "TAG": tag,
            ver_var: version,
            "REPO": repo,
            "SIGNED": "true" if signed else "",
            "RELEASE_BODY_OUT": str(out),
            "GH_TOKEN": "unused-by-the-stand-in",
        })
        proc = subprocess.run(["bash", "-euo", "pipefail", "-c", script], cwd=root, env=env,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise StepError(f"{workflow}: step {step_name!r} failed for {tag} "
                            f"(exit {proc.returncode}):\n{proc.stderr.strip()}")
        if not out.exists() or out.stat().st_size == 0:
            raise StepError(f"{workflow}: step {step_name!r} produced no body for {tag}")
        return out.read_text(encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tag")
    ap.add_argument("--signed", action="store_true", help="the macOS builds are signed and notarized")
    ap.add_argument("--repo", default="kmay89/securaCV", help="owner/name the firmware body links to")
    args = ap.parse_args(argv)
    try:
        sys.stdout.write(compose(args.tag, signed=args.signed, repo=args.repo))
    except StepError as e:
        print(f"::error::{e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
