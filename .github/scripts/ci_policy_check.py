#!/usr/bin/env python3
"""CI policy checker — enforces the general rules in .github/CI.md on every
workflow, current and future. A new workflow that skips a rule fails the
Workflow Lint check on the PR that introduces it, with a message naming the
rule and the fix.

Run locally:  python3 .github/scripts/ci_policy_check.py

The rules (exemptions live in .github/ci-policy.yml, NEVER in this file —
an exemption must be visible and reviewable, with a comment saying why):

  R1  every workflow declares `permissions` — at the top level, or on
      every job (least-privilege GITHUB_TOKEN, always explicit)
  R2  every job sets `timeout-minutes` (a hung job otherwise burns the
      360-minute default)
  R3  a workflow triggered by push or pull_request declares a
      `concurrency` group (supersede stale PR runs; queue — never
      cancel — release publishes); a workflow that fires on tag pushes
      or release events must not set a bare `cancel-in-progress: true`
      (a run canceled mid-publish leaves half-uploaded assets)
  R4  every action ref is pinned to a tag or SHA — never a mutable
      branch ref (@main/@master) or a floating docker :latest
  R5  pull_request workflows are path-filtered so unrelated PRs don't
      pay for them (or are listed in `unfiltered_ok` with a reason)
  R6  when push and pull_request both declare path filters, the two
      lists are identical — copy-paste drift between them silently
      changes what main verifies vs what PRs verify
  R7  a paths filter includes the workflow's own file, so editing a
      workflow always exercises it
  R8  third-party actions (any owner outside actions/ and github/) are
      pinned to a full 40-hex commit SHA, with a trailing `# <version>`
      comment naming the tag it was resolved from — a tag is a mutable
      ref the owner can move under a release run; Dependabot updates
      SHA pins and their comments together
"""

from __future__ import annotations

import fnmatch
import glob
import os
import re
import sys

import yaml

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORKFLOWS_DIR = os.path.join(REPO_ROOT, ".github", "workflows")
POLICY_PATH = os.path.join(REPO_ROOT, ".github", "ci-policy.yml")

MUTABLE_REFS = {"main", "master", "latest", "HEAD"}

# R8: owners whose actions may ride a version tag instead of a SHA.
# GitHub runs both orgs itself; everyone else's tags are mutable refs in
# somebody else's hands, so they get 40-hex SHA pins.
FIRST_PARTY_OWNERS = {"actions", "github"}

SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def load_policy() -> dict:
    if not os.path.exists(POLICY_PATH):
        return {}
    with open(POLICY_PATH, encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def triggers_of(wf: dict) -> dict:
    # YAML 1.1 parses the bare key `on` as boolean True.
    on = wf.get("on", wf.get(True, {}))
    if isinstance(on, str):
        return {on: None}
    if isinstance(on, list):
        return {t: None for t in on}
    return on or {}


def norm_list(v) -> list[str]:
    if v is None:
        return []
    if isinstance(v, str):
        return [v]
    return list(v)


def check_workflow(path: str, policy: dict) -> list[str]:
    name = os.path.basename(path)
    problems: list[str] = []

    with open(path, encoding="utf-8") as f:
        wf = yaml.safe_load(f)
    if not isinstance(wf, dict):
        return [f"{name}: does not parse as a workflow mapping"]

    triggers = triggers_of(wf)
    jobs = wf.get("jobs") or {}
    timeout_max = int(policy.get("timeout_max", 120))
    unfiltered_ok = set(policy.get("unfiltered_ok") or [])
    parity_ok = set(policy.get("paths_parity_ok") or [])

    # R1 — permissions: top-level, or on every job
    if "permissions" not in wf:
        missing = [j for j, spec in jobs.items()
                   if "permissions" not in (spec or {})]
        if missing:
            problems.append(
                f"{name}: R1 — no top-level `permissions`, and job(s) "
                f"{', '.join(missing)} don't declare their own. Add an "
                f"explicit least-privilege block (usually `contents: read`)."
            )

    # R2 — timeout-minutes on every job (reusable-workflow calls excluded:
    # the called workflow owns its own timeouts)
    for job, spec in jobs.items():
        spec = spec or {}
        if "uses" in spec:
            continue
        t = spec.get("timeout-minutes")
        if t is None:
            problems.append(
                f"{name}: R2 — job `{job}` has no `timeout-minutes`. A hung "
                f"run burns the 360-minute default; pick ~2-3x the healthy time."
            )
        else:
            # A matrix job may set its timeout per entry — `timeout-minutes:
            # ${{ matrix.minutes }}` with the value in `strategy.matrix.include`.
            # That is the right shape when one job runs workloads of very
            # different weight: a budget generous enough for the slowest entry
            # cannot catch the fastest one hanging. Resolve the expression and
            # check EVERY value, rather than exempting the job — an exemption
            # would switch R2 off exactly where it does the most good.
            for label, value in _resolve_timeouts(t, spec):
                if not isinstance(value, int) or not (1 <= value <= timeout_max):
                    problems.append(
                        f"{name}: R2 — job `{job}`{label} timeout-minutes "
                        f"{value!r} outside 1..{timeout_max} (raise timeout_max "
                        f"in ci-policy.yml if truly needed)."
                    )

    # R3 — concurrency for push/pull_request workflows
    if ("push" in triggers or "pull_request" in triggers) and "concurrency" not in wf:
        problems.append(
            f"{name}: R3 — runs on push/pull_request but declares no "
            f"`concurrency` group. PR workflows should cancel superseded runs "
            f"(cancel-in-progress on pull_request); publish/release workflows "
            f"should queue (cancel-in-progress: false)."
        )

    # R3 (publish half) — a workflow that fires on tag pushes or release
    # events publishes artifacts; a bare `cancel-in-progress: true` can
    # kill it mid-upload and leave half-published assets. `false` and
    # conditional ${{ ... }} expressions pass — the robot catches the
    # blunt footgun, reviewers judge the condition.
    push_spec = triggers.get("push")
    fires_on_publish = ("release" in triggers) or (
        isinstance(push_spec, dict) and push_spec.get("tags") is not None
    )
    conc = wf.get("concurrency")
    if (fires_on_publish and isinstance(conc, dict)
            and name not in set(policy.get("publish_cancel_ok") or [])
            and conc.get("cancel-in-progress") is True):
        problems.append(
            f"{name}: R3 — fires on tags/release but sets a bare "
            f"`cancel-in-progress: true`; a canceled run can die "
            f"mid-publish with half-uploaded release assets. Use `false`, "
            f"or a condition that excludes the publish path (see "
            f"desktop-release.yml), or exempt in publish_cancel_ok with a "
            f"comment."
        )

    # R3 (branch half) — a workflow that runs on pushes to a branch is the
    # thing that tells you whether that branch is healthy. A bare
    # `cancel-in-progress: true` applies to main as well as PR branches, so a
    # burst of merges kills each build before it reports and main's real state
    # goes unknown while merely looking busy.
    #
    # That is not theoretical: firmware.yml carried a bare `true`, and on
    # 2026-07-24 three merges landed within about a minute against a
    # ~30-minute build. Four of five main builds that day ended `canceled`,
    # including both halves of a link-error fix — the broken commit reached
    # main and the correction's build was canceled too.
    #
    # CI.md's R3 prose already prescribes the conditional form; only the
    # publish half was ever enforced, so this case slipped through for as long
    # as it existed. `false` and `${{ ... }}` conditions pass — the robot
    # catches the blunt footgun, reviewers judge the condition.
    fires_on_branch = isinstance(push_spec, dict) and push_spec.get("branches")
    if (fires_on_branch and isinstance(conc, dict)
            and name not in set(policy.get("branch_cancel_ok") or [])
            and conc.get("cancel-in-progress") is True):
        problems.append(
            f"{name}: R3 — runs on branch pushes but sets a bare "
            f"`cancel-in-progress: true`, so a merge landing behind another "
            f"cancels the first one's build and main's state goes unknown. "
            f"Use `${{{{ github.event_name == 'pull_request' }}}}` to "
            f"supersede PR runs without ever canceling main, or exempt in "
            f"branch_cancel_ok with a reason."
        )

    # R4/R8 — pinned action refs, in workflows AND composite actions (collected
    # by caller passing composite files through this same function is not
    # needed; see check_action_pins()).
    problems.extend(check_action_pins(name, jobs, policy))

    # R5 — pull_request path filtering
    if "pull_request" in triggers and name not in unfiltered_ok:
        pr = triggers.get("pull_request") or {}
        if not isinstance(pr, dict) or not ({"paths", "paths-ignore"} & set(pr)):
            problems.append(
                f"{name}: R5 — pull_request trigger has no paths/paths-ignore "
                f"filter, so EVERY PR pays for this workflow. Scope it, or add "
                f"it to unfiltered_ok in .github/ci-policy.yml with a comment "
                f"saying why repo-wide runs are the point."
            )

    # R6 — push/pull_request path parity
    if name not in parity_ok and "push" in triggers and "pull_request" in triggers:
        push = triggers.get("push") or {}
        pr = triggers.get("pull_request") or {}
        if isinstance(push, dict) and isinstance(pr, dict):
            for key in ("paths", "paths-ignore"):
                a, b = norm_list(push.get(key)), norm_list(pr.get(key))
                if sorted(a) != sorted(b):
                    problems.append(
                        f"{name}: R6 — push and pull_request `{key}` lists "
                        f"differ; main and PRs would verify different things. "
                        f"Make them identical (or exempt in paths_parity_ok "
                        f"with a comment)."
                    )

    # R7 — a paths filter must include the workflow's own file
    own = f".github/workflows/{name}"
    for trig in ("push", "pull_request"):
        spec = triggers.get(trig)
        if not isinstance(spec, dict):
            continue
        paths = norm_list(spec.get("paths"))
        if paths and not any(fnmatch.fnmatch(own, pat) for pat in paths):
            problems.append(
                f"{name}: R7 — the {trig} paths filter doesn't cover "
                f"`{own}`, so edits to this workflow won't run it. Add its "
                f"own path to the list."
            )

    return problems


def check_action_pins(label: str, jobs: dict, policy: dict) -> list[str]:
    problems = []
    tag_ok = set(policy.get("third_party_tag_ok") or [])
    for job, spec in jobs.items():
        steps = (spec or {}).get("steps") or []
        for step in steps:
            uses = (step or {}).get("uses")
            if not uses or uses.startswith("./"):
                continue
            if uses.startswith("docker://"):
                image = uses[len("docker://"):]
                tag = image.rsplit(":", 1)[1] if ":" in image else ""
                if not tag or tag in MUTABLE_REFS:
                    problems.append(
                        f"{label}: R4 — `{uses}` in job `{job}` is not pinned "
                        f"to an immutable tag."
                    )
                continue
            ref = uses.rsplit("@", 1)[1] if "@" in uses else ""
            if not ref or ref in MUTABLE_REFS:
                problems.append(
                    f"{label}: R4 — `{uses}` in job `{job}` rides a mutable "
                    f"ref; pin to a version tag or commit SHA."
                )
                continue
            # R8 — third-party actions get a full commit SHA, not a tag.
            owner = uses.split("/", 1)[0]
            if (owner.lower() not in {o.lower() for o in FIRST_PARTY_OWNERS}
                    and os.path.basename(label) not in tag_ok
                    and not SHA_RE.match(ref)):
                problems.append(
                    f"{label}: R8 — `{uses}` in job `{job}` pins a "
                    f"third-party action to a tag, which its owner can move "
                    f"under a release run. Resolve it (`git ls-remote "
                    f"https://github.com/{owner}/... 'refs/tags/{ref}^{{}}'`) "
                    f"and pin the 40-hex SHA with a trailing `# {ref}` "
                    f"comment; Dependabot keeps SHA pins fresh. Or exempt "
                    f"the workflow in third_party_tag_ok with a reason."
                )
    return problems


def check_composite_actions(policy: dict) -> list[str]:
    """R4/R8 also apply to steps inside local composite actions."""
    problems = []
    pattern = os.path.join(REPO_ROOT, ".github", "actions", "**", "action.y*ml")
    for path in glob.glob(pattern, recursive=True):
        with open(path, encoding="utf-8") as f:
            action = yaml.safe_load(f) or {}
        steps = (action.get("runs") or {}).get("steps") or []
        rel = os.path.relpath(path, REPO_ROOT)
        problems.extend(
            check_action_pins(rel, {"(composite)": {"steps": steps}}, policy))
    return problems


MATRIX_EXPR = re.compile(r"^\$\{\{\s*matrix\.([A-Za-z_][A-Za-z0-9_-]*)\s*\}\}$")


def _resolve_timeouts(t, spec):
    """[(label, value)] for a job's timeout-minutes, one per matrix entry.

    A literal resolves to itself. `${{ matrix.<key> }}` resolves to that key in
    every strategy.matrix.include entry, so all of them get range-checked. An
    expression we cannot resolve — a key that no entry defines, or anything
    other than a bare matrix reference — is returned unresolved so R2 rejects
    it, because a timeout nobody can evaluate is not a timeout.
    """
    if not isinstance(t, str):
        return [("", t)]
    m = MATRIX_EXPR.match(t.strip())
    if not m:
        return [("", t)]
    key = m.group(1)
    include = ((spec.get("strategy") or {}).get("matrix") or {}).get("include") or []
    entries = [e for e in include if isinstance(e, dict) and key in e]
    if not entries:
        return [(f" (matrix.{key} is defined by no include entry)", t)]
    return [(f" ({e.get('group', e.get('name', '?'))})", e[key]) for e in entries]


def main() -> int:
    policy = load_policy()
    files = sorted(
        glob.glob(os.path.join(WORKFLOWS_DIR, "*.yml"))
        + glob.glob(os.path.join(WORKFLOWS_DIR, "*.yaml"))
    )
    if not files:
        print("::error::no workflows found — is the checkout intact?")
        return 1

    all_problems: list[str] = []
    for path in files:
        all_problems.extend(check_workflow(path, policy))
    all_problems.extend(check_composite_actions(policy))

    if all_problems:
        for p in all_problems:
            print(f"::error::{p}")
        print(
            f"\n{len(all_problems)} CI policy violation(s). The rules and "
            f"their reasons: .github/CI.md — exemptions go in "
            f".github/ci-policy.yml with a comment, never silently."
        )
        return 1

    print(f"CI policy OK — {len(files)} workflows conform to .github/CI.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
