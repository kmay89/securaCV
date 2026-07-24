#!/usr/bin/env python3
"""The decision engine behind the "Update everything" button.

`release-everything.yml` is one click that ships whatever genuinely needs
shipping — every app and every firmware — and, just as importantly, does
*nothing* for whatever doesn't. Deciding that correctly is the whole problem,
so it lives here as a pure function over explicit inputs instead of inside YAML
where it can't be tested.

The rule, per target, generalized from `firmware-release-if-changed.yml`:

  * nothing published yet            → RELEASE (cut the first one)
  * source version NEWER than latest → RELEASE
  * source == latest, watched paths changed since that tag
                                     → NEEDS_BUMP (a human must bump the
                                       version; we must never re-ship a
                                       different tree under a published version)
  * source == latest, nothing changed → UP_TO_DATE (skip — this is the case
                                       that stops the button wasting compute)
  * source OLDER than latest         → BEHIND (something is wrong; say so)

and for the un-versioned Pages target:

  * watched paths changed since the last successful deploy → RELEASE
  * otherwise                                              → UP_TO_DATE

Why NEEDS_BUMP and BEHIND are *not* failures: the button's contract is that
pressing it is always safe. A target that needs a version bump is information,
not a broken build — it is reported loudly in the summary while every other
target still ships. The run fails only when something that was *supposed* to
happen didn't (a dispatch call erroring), which is the honest definition of a
red run.

Run the tests:  python3 -m unittest discover -s .github/scripts -p 'test_*.py'
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field, asdict
from typing import Any, Iterable

import yaml

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
CATALOG_PATH = os.path.join(REPO_ROOT, ".github", "release-targets.yml")

# Decisions
RELEASE = "release"
UP_TO_DATE = "up_to_date"
NEEDS_BUMP = "needs_bump"
BEHIND = "behind"
GATED = "gated"
SKIPPED = "skipped"

# Decisions that mean "start this workflow".
ACTIONABLE = {RELEASE}


# ─────────────────────────────── versions ──────────────────────────────────


def version_key(version: str) -> tuple:
    """Sort key for a version string.

    Handles `2.3.0` and `2.3.0-dev.1`. Per semver, a prerelease sorts BEFORE
    its release (`2.3.0-dev.1` < `2.3.0`), which matters: a dev build must
    never look "newer" than the stable release of the same triple and trigger
    a pointless re-cut.
    """
    match = re.match(r"^(\d+)\.(\d+)\.(\d+)(?:[-+](.*))?$", version.strip())
    if not match:
        raise ValueError(f"not a version: {version!r}")
    major, minor, patch, pre = match.groups()
    # (triple…, 0, prerelease) sorts before (triple…, 1, "") — the release.
    return (int(major), int(minor), int(patch), 0 if pre else 1, pre or "")


def compare(a: str, b: str) -> int:
    ka, kb = version_key(a), version_key(b)
    return (ka > kb) - (ka < kb)


def newest_published(tags: Iterable[str], prefix: str) -> str | None:
    """Newest STABLE version published under `prefix`.

    Prereleases are deliberately ignored as a baseline: a `fw-v2.3.0-dev.1`
    must not make the button think stable 2.3.0 is already out.
    """
    versions = []
    for tag in tags:
        if not tag.startswith(prefix):
            continue
        rest = tag[len(prefix):]
        if not re.fullmatch(r"\d+\.\d+\.\d+", rest):
            continue
        versions.append(rest)
    if not versions:
        return None
    return max(versions, key=version_key)


# ─────────────────────────────── the plan ──────────────────────────────────


@dataclass
class Decision:
    name: str
    label: str
    decision: str
    reason: str
    source_version: str | None = None
    latest_version: str | None = None
    workflow: str | None = None
    inputs: dict[str, str] = field(default_factory=dict)

    @property
    def actionable(self) -> bool:
        return self.decision in ACTIONABLE


def _render_inputs(raw: dict[str, Any], version: str | None) -> dict[str, str]:
    """Substitute `${version}` and stringify — workflow_dispatch inputs are
    always strings over the API, and a bare `True` would be rejected."""
    out = {}
    for key, value in (raw or {}).items():
        text = str(value)
        if version is not None:
            text = text.replace("${version}", version)
        out[key] = text
    return out


def decide(
    target: dict[str, Any],
    *,
    source_version: str | None,
    latest_version: str | None,
    changed: bool,
    publish: bool = True,
    force: bool = False,
    gate_enabled: bool = True,
    selected: bool = True,
) -> Decision:
    """Decide one target. Pure — every input is explicit, so it is testable."""
    name = target["name"]
    label = target.get("label", name)
    workflow = target.get("workflow")
    inputs_spec = target.get("inputs" if publish else "dev_inputs") or target.get("inputs") or {}

    def make(decision: str, reason: str, version: str | None = None) -> Decision:
        return Decision(
            name=name,
            label=label,
            decision=decision,
            reason=reason,
            source_version=source_version,
            latest_version=latest_version,
            workflow=workflow,
            inputs=_render_inputs(inputs_spec, version) if decision in ACTIONABLE else {},
        )

    if not selected:
        return make(SKIPPED, "Not selected for this run.")

    if target.get("gate_var") and not gate_enabled:
        return make(
            GATED,
            f"{target['gate_var']} is not 'true', so this target's release workflow "
            f"would be a no-op. Nothing dispatched — see tvos/README.md for the "
            f"one-time Apple setup.",
        )

    # The Pages target has no version to compare; "did the published files
    # change?" is the whole question.
    if target.get("kind") == "pages":
        if force:
            return make(RELEASE, "Force requested — redeploying the site.")
        if changed:
            return make(RELEASE, "Published pages changed since the last deploy — redeploying.")
        return make(UP_TO_DATE, "The site is unchanged since the last successful deploy. ✅")

    if source_version is None:
        return make(
            NEEDS_BUMP,
            "Could not read this target's source version — check the path in "
            ".github/release-targets.yml.",
        )

    if force:
        return make(RELEASE, f"Force requested — re-cutting {source_version}.", source_version)

    if latest_version is None:
        return make(
            RELEASE,
            f"No release published yet — cutting the first, {source_version}.",
            source_version,
        )

    order = compare(source_version, latest_version)
    if order > 0:
        return make(
            RELEASE,
            f"Source {source_version} is newer than the latest release "
            f"{latest_version} — cutting it.",
            source_version,
        )
    if order == 0:
        if changed:
            return make(
                NEEDS_BUMP,
                f"Changed since {latest_version} was released, but the version is "
                f"still {source_version}. Bump it, then press the button again — "
                f"a release must never ship under a version already published.",
            )
        return make(
            UP_TO_DATE,
            f"Already released at {latest_version} and unchanged since. Nothing to do. ✅",
        )

    return make(
        BEHIND,
        f"Source {source_version} is OLDER than the latest release "
        f"{latest_version}. That shouldn't happen — check the version file.",
    )


# ───────────────────────── reading the real repo ───────────────────────────


def load_catalog(path: str = CATALOG_PATH) -> list[dict[str, Any]]:
    with open(path, encoding="utf-8") as handle:
        doc = yaml.safe_load(handle) or {}
    targets = doc.get("targets") or []
    seen = set()
    for target in targets:
        if "name" not in target:
            raise ValueError(f"a target in {path} has no name")
        if target["name"] in seen:
            raise ValueError(f"duplicate target name in {path}: {target['name']}")
        seen.add(target["name"])
    return targets


def read_source_version(target: dict[str, Any], repo_root: str = REPO_ROOT) -> str | None:
    """Read a target's version from the file the catalog points at.

    Returns None (rather than raising) when it can't be read, so one
    mis-pointed catalog row degrades to a NEEDS_BUMP note instead of taking the
    whole button down with it.
    """
    spec = target.get("version")
    if not spec:
        return None
    path = os.path.join(repo_root, spec["file"])
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        return None

    if "json_path" in spec:
        try:
            value: Any = json.loads(text)
        except json.JSONDecodeError:
            return None
        for part in str(spec["json_path"]).split("."):
            if not isinstance(value, dict) or part not in value:
                return None
            value = value[part]
        return str(value).strip() if isinstance(value, (str, int, float)) else None

    if "regex" in spec:
        match = re.search(spec["regex"], text, re.MULTILINE)
        return match.group(1).strip() if match else None

    return None


def paths_changed(since_ref: str, paths: list[str], repo_root: str = REPO_ROOT) -> bool:
    """Did any watched path change between `since_ref` and HEAD?

    Conservative on failure: if the diff can't be computed (a tag that was
    never fetched, say), assume it DID change. Being told "you may need to bump"
    when nothing changed is a harmless nudge; silently skipping a release that
    was needed is the failure mode worth avoiding.
    """
    if not paths:
        return False
    try:
        result = subprocess.run(
            ["git", "diff", "--quiet", f"{since_ref}..HEAD", "--", *paths],
            cwd=repo_root,
            capture_output=True,
        )
    except OSError:
        return True
    if result.returncode == 0:
        return False
    if result.returncode == 1:
        return True
    return True  # git couldn't answer (unknown ref) — assume changed


def build_plan(
    targets: list[dict[str, Any]],
    published_tags: list[str],
    *,
    publish: bool = True,
    force: set[str] | None = None,
    only: set[str] | None = None,
    gates: dict[str, bool] | None = None,
    changed_resolver=None,
    repo_root: str = REPO_ROOT,
) -> list[Decision]:
    force = force or set()
    gates = gates or {}
    decisions = []

    for target in targets:
        name = target["name"]
        selected = only is None or name in only
        source_version = read_source_version(target, repo_root) if target.get("version") else None
        latest_version = (
            newest_published(published_tags, target["tag_prefix"])
            if target.get("tag_prefix")
            else None
        )

        changed = False
        if selected and changed_resolver is not None:
            changed = changed_resolver(target, latest_version)

        gate_var = target.get("gate_var")
        gate_enabled = gates.get(gate_var, False) if gate_var else True

        decisions.append(
            decide(
                target,
                source_version=source_version,
                latest_version=latest_version,
                changed=changed,
                publish=publish,
                force=name in force or "all" in force,
                gate_enabled=gate_enabled,
                selected=selected,
            )
        )
    return decisions


# ────────────────────────────────── CLI ────────────────────────────────────


def main(argv: list[str]) -> int:
    """Emit the plan as JSON.

    Reads the published tag list from stdin (a JSON array) so the GitHub API
    call stays in the workflow and this stays a pure, offline-testable function.
    """
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publish", action="store_true", help="plan real releases (default: dev build-only)")
    parser.add_argument("--force", default="", help="comma-separated target names, or 'all'")
    parser.add_argument("--only", default="", help="comma-separated target names (default: all)")
    parser.add_argument("--gates", default="{}", help="JSON object of gate var -> bool")
    parser.add_argument("--pages-since", default="", help="commit the site was last deployed from")
    args = parser.parse_args(argv)

    published_tags = json.load(sys.stdin) if not sys.stdin.isatty() else []
    targets = load_catalog()

    def changed_resolver(target: dict[str, Any], latest_version: str | None) -> bool:
        watch = target.get("watch") or []
        if target.get("kind") == "pages":
            # No tag to diff against — compare with the commit the site was
            # last deployed from. Unknown (first-ever run) means "deploy".
            if not args.pages_since:
                return True
            return paths_changed(args.pages_since, watch)
        if latest_version is None:
            return False  # nothing published: "changed" is not the question
        return paths_changed(f"{target['tag_prefix']}{latest_version}", watch)

    decisions = build_plan(
        targets,
        published_tags,
        publish=args.publish,
        force={f for f in args.force.split(",") if f},
        only={o for o in args.only.split(",") if o} or None,
        gates=json.loads(args.gates or "{}"),
        changed_resolver=changed_resolver,
    )
    json.dump([asdict(d) for d in decisions], sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
