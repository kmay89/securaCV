#!/usr/bin/env python3
"""canary-local/tools/hub_seed_apply.py — run the hub's first-boot provisioning plan.

Reads canary-local/devices/hub_seed.json (produced by gen_hub_seed.py) and makes
it real on a Home Assistant OS box: registers the third-party add-on
repositories, installs the broker + Frigate + the securaCV kernel in order, drops
the curated Frigate config, and narrates *why* each step exists as it runs.

Why this exists — the punt it replaces
--------------------------------------
scripts/install.sh drives the `ha` CLI, which has NO command to register an
add-on store repository. So today's "one-liner" install quietly degrades to
"now go click through the Add-on Store UI yourself" for the two third-party
add-ons (Frigate, securaCV). The Supervisor REST API *can* register a repository
(POST /store/repositories); this executor uses it, so the install is genuinely
unattended.

Design — pure plan, thin I/O
----------------------------
The same split the flasher uses (hub-core decides, hub-io touches the world):

  * ``plan_actions(seed, observed)`` is PURE. Given the plan and a snapshot of
    what is already on the hub, it returns the ordered, idempotent list of
    concrete actions plus the narration for each. No network, no filesystem.
    This is what the tests drive, and why they need no Home Assistant.
  * ``SupervisorClient`` is the ONLY thing that talks to the hub. Swap it out
    (``--dry-run`` builds no client at all) and nothing else changes.

The narration is the point, not decoration
-------------------------------------------
Every ``why`` / ``what`` / ``for_what`` printed here comes verbatim from the
generated plan, so the installer, the docs (docs/full_stack_setup.md), and the
flasher UI cannot tell three different stories about what is happening to your
house. ``--format json`` emits the same narration for a UI to render.

Runtime
-------
Python 3 standard library only (urllib) — nothing to pip-install on the device.
Its real home is a first-boot provisioner the seed assembler bakes in, which
supplies ``SUPERVISOR_TOKEN`` and reachability to ``http://supervisor``.
``--dry-run`` needs neither and prints exactly what a fresh hub would get — that
is what CI exercises.

Usage
-----
  # See the whole narrated plan, make no changes, need no hub:
  python3 canary-local/tools/hub_seed_apply.py --dry-run

  # Machine-readable narration (for the flasher UI):
  python3 canary-local/tools/hub_seed_apply.py --dry-run --format json

  # Do it for real, from inside a context that has SUPERVISOR_TOKEN:
  python3 canary-local/tools/hub_seed_apply.py
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import shutil
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_PLAN = REPO / "canary-local/devices/hub_seed.json"
DEFAULT_BASE_URL = os.environ.get("SUPERVISOR_URL", "http://supervisor")


# ---------------------------------------------------------------------------
# Pure core: decide what to do, and why. No I/O lives below this line until the
# SupervisorClient section — everything here is a deterministic function of the
# plan plus an observed-state snapshot, which is what makes it testable.
# ---------------------------------------------------------------------------


def norm_url(url: str) -> str:
    """Normalize a repo URL for comparison: lowercased, no trailing slash.

    Must match how Supervisor stores repository sources and how gen_hub_seed.py
    hashes them — a trailing slash there produces a different add-on slug, so we
    are consistent about dropping it here too.
    """
    return url.strip().rstrip("/").lower()


@dataclass
class Action:
    """One concrete thing to do (or skip). The atom the executor performs."""

    kind: str  # register_repo | install_addon | start_addon | write_config
    label: str  # human-facing target (a URL, a slug, a path)
    already: bool = False  # true => this is already satisfied; skip it
    reason: str = ""  # why it is being skipped, or an extra note
    url: str = ""  # register_repo
    slug: str = ""  # install_addon / start_addon (the Supervisor slug)
    friendly: str = ""  # the friendly slug, for display
    src: str = ""  # write_config: repo-relative source
    dest: str = ""  # write_config: absolute on-device destination


@dataclass
class StepPlan:
    """A plan step expanded into its actions, carrying the plan's narration."""

    id: str
    title: str
    what: str
    why: str
    for_what: str
    actions: list[Action] = field(default_factory=list)
    user_must_finish: str = ""


# A fresh hub: nothing registered, nothing installed, no config written. This is
# the assumption --dry-run makes so it always prints the FULL plan.
FRESH_HUB: dict = {"repositories": set(), "addons": {}, "existing_files": set()}


def plan_actions(seed: dict, observed: dict) -> list[StepPlan]:
    """Expand the plan's steps into idempotent, ordered actions given hub state.

    ``observed`` is a snapshot::

        {
          "repositories":  {normalized repo url, ...},   # already registered
          "addons":        {supervisor_slug: state, ...},# already installed
          "existing_files":{dest path, ...},             # config already present
        }

    Anything already satisfied comes back as an Action with ``already=True`` and
    a human ``reason`` — the executor prints it as a skip rather than redoing it,
    which is what makes re-running safe.
    """
    repos_present = observed.get("repositories", set())
    addons = observed.get("addons", {})
    files_present = observed.get("existing_files", set())

    out: list[StepPlan] = []
    for step in seed.get("steps", []):
        sp = StepPlan(
            id=step.get("id", ""),
            title=step.get("title", ""),
            what=step.get("what", ""),
            why=step.get("why", ""),
            for_what=step.get("for_what", ""),
            user_must_finish=step.get("user_must_finish", ""),
        )

        if "repositories" in step:
            for url in step["repositories"]:
                present = norm_url(url) in repos_present
                sp.actions.append(
                    Action(
                        kind="register_repo",
                        label=url,
                        url=url,
                        already=present,
                        reason="already registered" if present else "",
                    )
                )

        if "addon" in step:
            sup = step.get("supervisor_slug") or step["addon"]
            friendly = step["addon"]
            installed = sup in addons
            sp.actions.append(
                Action(
                    kind="install_addon",
                    label=friendly,
                    slug=sup,
                    friendly=friendly,
                    already=installed,
                    reason="already installed" if installed else "",
                )
            )
            if step.get("start"):
                running = addons.get(sup) == "started"
                sp.actions.append(
                    Action(
                        kind="start_addon",
                        label=friendly,
                        slug=sup,
                        friendly=friendly,
                        already=running,
                        reason="already running" if running else "",
                    )
                )

        if "dest" in step:
            dest = step["dest"]
            exists = dest in files_present
            never = step.get("never_overwrite", False)
            skip = exists and never
            sp.actions.append(
                Action(
                    kind="write_config",
                    label=dest,
                    src=step.get("source", ""),
                    dest=dest,
                    already=skip,
                    reason=(
                        "config already present — never overwriting your edits" if skip else ""
                    ),
                )
            )
            if step.get("then_start"):
                sup = step.get("then_start_supervisor_slug") or step["then_start"]
                friendly = step["then_start"]
                running = addons.get(sup) == "started"
                sp.actions.append(
                    Action(
                        kind="start_addon",
                        label=friendly,
                        slug=sup,
                        friendly=friendly,
                        already=running,
                        reason="already running" if running else "",
                    )
                )

        out.append(sp)
    return out


def describe(action: Action, base_url: str) -> str:
    """One line saying what an action does — including the exact API call, so a
    dry-run doubles as documentation and CI can assert on the real endpoints."""
    if action.kind == "register_repo":
        return f"register repository {action.url}  (POST {base_url}/store/repositories)"
    if action.kind == "install_addon":
        return f"install add-on {action.slug}  (POST {base_url}/store/addons/{action.slug}/install)"
    if action.kind == "start_addon":
        return f"start add-on {action.slug}  (POST {base_url}/addons/{action.slug}/start)"
    if action.kind == "write_config":
        return f"copy {action.src} -> {action.dest}"
    return action.kind


def counts(steps: list[StepPlan]) -> tuple[int, int]:
    """(actions to perform, actions already satisfied) across all steps."""
    todo = sum(1 for s in steps for a in s.actions if not a.already)
    done = sum(1 for s in steps for a in s.actions if a.already)
    return todo, done


# ---------------------------------------------------------------------------
# I/O: the Supervisor REST client. The only place that touches the network.
# ---------------------------------------------------------------------------


class SupervisorError(RuntimeError):
    """A Supervisor API call failed. Carries a human-readable reason."""


class SupervisorClient:
    """Minimal Home Assistant Supervisor REST client (stdlib urllib only)."""

    def __init__(self, base_url: str, token: str, timeout: int = 120, install_timeout: int = 900):
        self.base = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.install_timeout = install_timeout

    def _req(self, method: str, path: str, body: dict | None = None, timeout: int | None = None):
        url = f"{self.base}{path}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                raw = resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", errors="replace")
            raise SupervisorError(f"{method} {path} -> HTTP {e.code}: {detail[:300]}") from None
        except urllib.error.URLError as e:
            raise SupervisorError(
                f"{method} {path} -> {e.reason}. "
                "Is this running on the hub with SUPERVISOR_TOKEN set and http://supervisor reachable?"
            ) from None
        if not raw:
            return {}
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {"raw": raw}

    def get_repositories(self) -> set[str]:
        r = self._req("GET", "/store/repositories")
        data = r.get("data", r)
        items = data.get("repositories") if isinstance(data, dict) else data
        found: set[str] = set()
        for it in items or []:
            src = it.get("source") or it.get("url") or it.get("slug")
            if src:
                found.add(norm_url(src))
        return found

    def get_addons(self) -> dict[str, str]:
        r = self._req("GET", "/addons")
        data = r.get("data", r)
        items = data.get("addons") if isinstance(data, dict) else data
        return {it["slug"]: it.get("state", "") for it in (items or []) if it.get("slug")}

    def register_repository(self, url: str) -> None:
        self._req("POST", "/store/repositories", {"repository": url})

    def install_addon(self, slug: str) -> None:
        self._req("POST", f"/store/addons/{slug}/install", timeout=self.install_timeout)

    def start_addon(self, slug: str) -> None:
        self._req("POST", f"/addons/{slug}/start")


def observe(client: SupervisorClient, seed: dict) -> dict:
    """Snapshot the hub so plan_actions can decide what still needs doing."""
    dests = [s["dest"] for s in seed.get("steps", []) if s.get("dest")]
    return {
        "repositories": client.get_repositories(),
        "addons": client.get_addons(),
        "existing_files": {d for d in dests if os.path.exists(d)},
    }


# ---------------------------------------------------------------------------
# Rendering + execution
# ---------------------------------------------------------------------------


def render_text(steps: list[StepPlan], base_url: str, dry_run: bool) -> str:
    """The narrated plan as text: title + why + for_what + each action."""
    lines: list[str] = []
    for i, s in enumerate(steps, 1):
        lines.append("")
        lines.append(f"[{i}/{len(steps)}] {s.title}")
        if s.why:
            lines.append(f"    why:  {s.why}")
        if s.for_what:
            lines.append(f"    for:  {s.for_what}")
        for a in s.actions:
            if a.already:
                lines.append(f"    - skip: {describe(a, base_url)}  ({a.reason})")
            else:
                verb = "would " if dry_run else "next: "
                lines.append(f"    - {verb}{describe(a, base_url)}")
        if s.user_must_finish:
            lines.append(f"    ! you finish: {s.user_must_finish}")
    todo, done = counts(steps)
    lines.append("")
    lines.append(f"= {len(steps)} steps: {todo} action(s) to perform, {done} already satisfied.")
    return "\n".join(lines)


def steps_to_json(steps: list[StepPlan]) -> str:
    return json.dumps([dataclasses.asdict(s) for s in steps], indent=2)


def execute(steps: list[StepPlan], client: SupervisorClient, assets_root: Path) -> None:
    """Perform the plan, narrating as it goes. Fail-closed: on the first error,
    stop and say so — re-running is safe because every action is idempotent."""
    for i, s in enumerate(steps, 1):
        print(f"\n[{i}/{len(steps)}] {s.title}")
        if s.why:
            print(f"    why:  {s.why}")
        for a in s.actions:
            if a.already:
                print(f"    - skip: {describe(a, client.base)}  ({a.reason})")
                continue
            print(f"    - {describe(a, client.base)}")
            try:
                _perform(a, client, assets_root)
            except (SupervisorError, OSError) as e:
                print(f"    x FAILED: {e}", file=sys.stderr)
                print(
                    "\nStopped. Nothing here is half-done in a way a re-run can't fix: "
                    "this executor is idempotent, so fix the cause and run it again.",
                    file=sys.stderr,
                )
                raise SystemExit(1)
            print("      done")
        if s.user_must_finish:
            print(f"    ! you finish: {s.user_must_finish}")


def _perform(a: Action, client: SupervisorClient, assets_root: Path) -> None:
    if a.kind == "register_repo":
        client.register_repository(a.url)
    elif a.kind == "install_addon":
        client.install_addon(a.slug)
    elif a.kind == "start_addon":
        client.start_addon(a.slug)
    elif a.kind == "write_config":
        src = (assets_root / a.src).resolve()
        if not src.exists():
            raise OSError(
                f"curated config {a.src} not found under {assets_root} — "
                "pass --assets-root pointing at where the repo assets live"
            )
        os.makedirs(os.path.dirname(a.dest), exist_ok=True)
        # cp -L semantics: dereference so a symlinked source lands as real bytes.
        shutil.copyfile(src, a.dest, follow_symlinks=True)
    else:  # pragma: no cover - guarded by the dataclass kinds above
        raise SupervisorError(f"unknown action kind {a.kind!r}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", type=Path, default=DEFAULT_PLAN, help="path to hub_seed.json")
    ap.add_argument(
        "--assets-root",
        type=Path,
        default=REPO,
        help="root the plan's `source` paths resolve against (default: repo root)",
    )
    ap.add_argument("--base-url", default=DEFAULT_BASE_URL, help="Supervisor base URL")
    ap.add_argument(
        "--token",
        default=os.environ.get("SUPERVISOR_TOKEN", ""),
        help="Supervisor bearer token (default: $SUPERVISOR_TOKEN)",
    )
    ap.add_argument("--dry-run", action="store_true", help="print the plan; change nothing; need no hub")
    ap.add_argument(
        "--observe",
        action="store_true",
        help="in --dry-run, query the real hub so already-done steps show as skips (needs a token)",
    )
    ap.add_argument("--format", choices=["text", "json"], default="text", help="dry-run output format")
    args = ap.parse_args(argv)

    try:
        seed = json.loads(Path(args.plan).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        print(f"hub_seed_apply.py: cannot read plan {args.plan}: {e}", file=sys.stderr)
        return 2

    if args.dry_run:
        observed = FRESH_HUB
        if args.observe:
            if not args.token:
                print("--observe needs a token (SUPERVISOR_TOKEN) to query the hub", file=sys.stderr)
                return 2
            observed = observe(SupervisorClient(args.base_url, args.token), seed)
        steps = plan_actions(seed, observed)
        if args.format == "json":
            print(steps_to_json(steps))
        else:
            print(render_text(steps, args.base_url, dry_run=True))
        return 0

    # Real run.
    if not args.token:
        print(
            "hub_seed_apply.py: no SUPERVISOR_TOKEN — this must run where the Supervisor API is "
            "reachable (a first-boot provisioner or the SSH/Terminal add-on). Use --dry-run to "
            "preview the plan anywhere.",
            file=sys.stderr,
        )
        return 2
    client = SupervisorClient(args.base_url, args.token)
    try:
        observed = observe(client, seed)
    except SupervisorError as e:
        print(f"hub_seed_apply.py: cannot reach the hub: {e}", file=sys.stderr)
        return 1
    steps = plan_actions(seed, observed)
    execute(steps, client, Path(args.assets_root))
    todo, done = counts(steps)
    print(f"\nHub provisioned: {done} step-action(s) were already in place, {todo} applied.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
