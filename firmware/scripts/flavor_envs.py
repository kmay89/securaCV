#!/usr/bin/env python3
"""Print a firmware product's PlatformIO env list from firmware/flavors.json.

The release workflows (firmware-release.yml, flasher-release.yml) build the
Canary Display board envs from a list. That list used to be TYPED into each
workflow by hand, twice, with nothing checking it against flavors.json — and
the two copies disagreed: the AMOLED shipped from the tag ceremony and was
missing from every dev publish. A flavor could ship from one button and not
the other, and no gate noticed.

This script is the one place a workflow gets that list from:

    python3 firmware/scripts/flavor_envs.py canary-display
        every env in the product's `build_envs`, one per line, in build order

    python3 firmware/scripts/flavor_envs.py canary-display --release
        the product's `release_envs` — the subset the release workflows build
        and publish as flasher/OTA products — in the order the release steps
        must build them (see ordered_release_envs)

    python3 firmware/scripts/flavor_envs.py canary-display --release --with-core
        the same list, each line `<env> <core>` where <core> is the env's
        PLATFORMIO_CORE_DIR class: a `core_dir_groups` label (shared core dir
        `.pio-core-<label>`), `isolated` (its own `.pio-core-<env>`), or
        `default` (the runner's ~/.platformio). The workflows pick the core
        dir from that word instead of naming envs.

    python3 firmware/scripts/flavor_envs.py canary-display --release --short
        the same list with the `canary-display-` prefix stripped — the short
        names the release's staging/signing loops key on

    python3 firmware/scripts/flavor_envs.py --check-workflows
        the lint: every literal `canary-display-<x>` token in any workflow
        under .github/workflows/ must name an env flavors.json declares, so a
        typo'd or retired env can't hide in a workflow; also validates that
        `release_envs` is a subset of `build_envs` and that every release env
        has its flasher catalog product (canary-local/devices/flash.json).
        Exit 1 with every problem listed. Run from lint.yml.

stdlib only. Run from any directory (the repo root is resolved from this
file's own location).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FLAVORS = REPO / "firmware" / "flavors.json"
FLASH_CATALOG = REPO / "canary-local" / "devices" / "flash.json"
WORKFLOWS = REPO / ".github" / "workflows"

# Products whose env names the workflow lint checks. Every env of these is
# named `<product>-<suffix>` and every literal `<product>-<suffix>` token in a
# workflow is an env (or a product id, which for the display is "securacv-" +
# env by construction: SECURACV_OTA_PRODUCT in canary-display's config.h). The
# other prefixed products are NOT linted yet because their prefixes double as
# other identifiers in workflows — viewer.yml has a job literally named
# `canary-vision-lint` — and a lint that needs an allowlist to pass is a lint
# people learn to extend instead of fix.
LINTED_PRODUCTS = ("canary-display",)

DEFAULT_CORE = "default"
ISOLATED_CORE = "isolated"


def load_flavors() -> list[dict]:
    return json.loads(FLAVORS.read_text(encoding="utf-8"))


def find_product(flavors: list[dict], name: str) -> dict:
    for entry in flavors:
        if entry.get("name") == name:
            return entry
    known = ", ".join(e.get("name", "?") for e in flavors)
    raise SystemExit(
        f"flavor_envs.py: no product named '{name}' in "
        f"{FLAVORS.relative_to(REPO)} (known: {known})"
    )


def core_of(entry: dict, env: str) -> str:
    """The env's PLATFORMIO_CORE_DIR class — same precedence firmware.yml
    applies: a core_dir_groups label wins, then isolated_core_envs, then the
    default core dir."""
    groups = entry.get("core_dir_groups") or {}
    if env in groups:
        return str(groups[env])
    if env in (entry.get("isolated_core_envs") or []):
        return ISOLATED_CORE
    return DEFAULT_CORE


def ordered_release_envs(entry: dict) -> list[str]:
    """`release_envs` in the order the release steps must build them.

    Named core-dir groups first (in the order they first appear), then the
    default core dir, then the isolated envs — within each class in
    build_envs order. This is the order the release workflows always used, and
    it is load-bearing: switching PLATFORMIO_CORE_DIR changes the PlatformIO
    project checksum, which wipes .pio/build. The workflows stage every env's
    outputs the moment it builds, but a core-dir switch must still never run
    after an env whose outputs aren't staged yet.
    """
    envs = entry.get("release_envs")
    if envs is None:
        raise SystemExit(
            f"flavor_envs.py: product '{entry.get('name')}' declares no "
            f"`release_envs` in {FLAVORS.relative_to(REPO)} — add the subset "
            f"of build_envs the release workflows publish"
        )
    named: list[str] = []
    for env in envs:
        core = core_of(entry, env)
        if core not in (DEFAULT_CORE, ISOLATED_CORE) and core not in named:
            named.append(core)
    rank = {label: i for i, label in enumerate(named)}
    rank[DEFAULT_CORE] = len(named)
    rank[ISOLATED_CORE] = len(named) + 1
    build_index = {env: i for i, env in enumerate(entry.get("build_envs") or [])}
    return sorted(envs, key=lambda e: (rank[core_of(entry, e)],
                                       build_index.get(e, 1 << 30)))


def validate(flavors: list[dict]) -> list[str]:
    """Structural checks on flavors.json that the printing modes rely on."""
    problems: list[str] = []
    flash_ids: set[str] | None = None
    if FLASH_CATALOG.exists():
        catalog = json.loads(FLASH_CATALOG.read_text(encoding="utf-8"))
        flash_ids = {p.get("id") for p in catalog.get("products", [])}

    for entry in flavors:
        name = entry.get("name", "?")
        build = list(entry.get("build_envs") or [])
        build_set = set(build)
        if len(build) != len(build_set):
            problems.append(f"{name}: build_envs has duplicates")
        for env in entry.get("isolated_core_envs") or []:
            if env not in build_set:
                problems.append(f"{name}: isolated_core_envs names '{env}', "
                                f"which is not in build_envs")
        for env in entry.get("core_dir_groups") or {}:
            if env not in build_set:
                problems.append(f"{name}: core_dir_groups names '{env}', "
                                f"which is not in build_envs")
        release = entry.get("release_envs")
        if release is None:
            continue
        if len(release) != len(set(release)):
            problems.append(f"{name}: release_envs has duplicates")
        for env in release:
            if env not in build_set:
                problems.append(f"{name}: release_envs names '{env}', which is "
                                f"not in build_envs — a release cannot build "
                                f"an env CI never compiles")
            elif flash_ids is not None and f"securacv-{env}" not in flash_ids:
                problems.append(
                    f"{name}: release env '{env}' has no flasher catalog "
                    f"product 'securacv-{env}' in "
                    f"{FLASH_CATALOG.relative_to(REPO)} — the release would "
                    f"publish a binary the browser flasher cannot offer")
    return problems


def check_workflows(flavors: list[dict], products=LINTED_PRODUCTS) -> list[str]:
    """Every literal `<product>-<suffix>` token in a workflow names a real env."""
    problems: list[str] = []
    for product in products:
        entry = find_product(flavors, product)
        envs = set(entry.get("build_envs") or [])
        # `\b` lets the token start mid-identifier after a hyphen, which is
        # what catches `securacv-canary-display-dash7` and
        # `.pio-core-canary-display-nightstand-c6`; a template such as
        # `canary-display-${E}` never matches because `$` is not a suffix
        # character, and a trailing hyphen before `${VERSION}` is stripped.
        token_re = re.compile(rf"\b{re.escape(product)}-([a-z0-9][a-z0-9-]*)")
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for lineno, line in enumerate(
                    path.read_text(encoding="utf-8").splitlines(), 1):
                for m in token_re.finditer(line):
                    token = f"{product}-{m.group(1)}".rstrip("-")
                    if token not in envs:
                        # The test suite points WORKFLOWS at a temp dir, so
                        # the path may sit outside the repo; name it as-is
                        # then rather than crash on relative_to.
                        try:
                            shown = str(path.relative_to(REPO))
                        except ValueError:
                            shown = str(path)
                        problems.append(
                            f"{shown}:{lineno}: '{token}' is "
                            f"not an env of '{product}' in "
                            f"{FLAVORS.relative_to(REPO)} — the release set is "
                            f"`python3 firmware/scripts/flavor_envs.py "
                            f"{product} --release`; a name not in flavors.json "
                            f"is a typo or a retired env")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("product", nargs="?", help="flavors.json product name")
    ap.add_argument("--release", action="store_true",
                    help="print release_envs (release build order) instead of build_envs")
    ap.add_argument("--with-core", action="store_true",
                    help="append each env's PLATFORMIO_CORE_DIR class")
    ap.add_argument("--short", action="store_true",
                    help="strip the '<product>-' prefix from each env")
    ap.add_argument("--check-workflows", action="store_true",
                    help="lint: workflows name only envs flavors.json declares")
    args = ap.parse_args(argv)

    flavors = load_flavors()

    if args.check_workflows:
        problems = validate(flavors) + check_workflows(flavors)
        if problems:
            for p in problems:
                print(f"::error::{p}")
            print(f"\n{len(problems)} problem(s): every display env a workflow "
                  f"names must exist in firmware/flavors.json, and the release "
                  f"set must be a subset of what CI builds.")
            return 1
        n = sum(len(e.get("release_envs") or []) for e in flavors)
        print(f"flavor_envs.py --check-workflows: OK — workflows name only "
              f"declared envs; {n} release env(s) all in build_envs with a "
              f"flasher product.")
        return 0

    if not args.product:
        ap.error("a product name is required unless --check-workflows is given")
    entry = find_product(flavors, args.product)
    envs = ordered_release_envs(entry) if args.release else list(entry.get("build_envs") or [])
    prefix = f"{args.product}-"
    for env in envs:
        shown = env[len(prefix):] if args.short and env.startswith(prefix) else env
        print(f"{shown} {core_of(entry, env)}" if args.with_core else shown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
