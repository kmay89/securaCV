#!/usr/bin/env python3
"""Print a firmware product's PlatformIO env list from firmware/flavors.json.

The release workflows (firmware-release.yml, flasher-release.yml) build the
Canary Display board envs from a list. That list used to be TYPED into each
workflow by hand, twice, with nothing checking it against flavors.json — and
the two copies disagreed: the AMOLED shipped from the tag ceremony and was
missing from every dev publish. A flavor could ship from one button and not
the other, and no gate noticed.

This script is the one place a workflow gets that list from — and since
roadmap item 23 both release workflows DO: each has a "Resolve the
canary-display release envs" step that runs `--release --json` and feeds the
result to its build and packaging steps, so neither types an env name.

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

    python3 firmware/scripts/flavor_envs.py canary-display --release --json
        the same list as ONE JSON array, in the same order, one object per
        env: {"env": "canary-display-dash7", "short": "dash7",
        "core": "display-pio3"} — what the release workflows write to
        $GITHUB_OUTPUT and read back with jq (the `core` word picks the
        PLATFORMIO_CORE_DIR; the `short` name keys the staging loops)

    python3 firmware/scripts/flavor_envs.py --flavors PATH ...
        read PATH instead of firmware/flavors.json. flasher-release.yml
        rebuilds a TAGGED tree whose flavors.json may predate `release_envs`
        (or the tag may predate this script); it overlays today's copy of
        both from the dispatch ref and points here, so the env list is
        today's — an env the tag's ini lacks fails its `pio run` and warns
        away, exactly as the typed list used to.

    python3 firmware/scripts/flavor_envs.py --build-matrix
        firmware.yml's `build-platformio` matrix as one line of JSON: every
        product from flavors.json, with a product that declares `shards`
        expanded into one leg per shard (see build_legs). The `flavors` job
        runs this; the manifest is validated first, so a shard list that
        drops an env fails that job instead of silently not building it.

    python3 firmware/scripts/flavor_envs.py --check-workflows
        the lint: every literal `canary-display-<x>` token in any workflow
        under .github/workflows/ must name an env flavors.json declares, so a
        typo'd or retired env can't hide in a workflow; the two release
        workflows must derive the list from this script (a `--release`
        invocation present, no literal `pio run -e canary-display-<env>`);
        also validates that `release_envs` is a subset of `build_envs`, that
        every release env has its flasher catalog product
        (canary-local/devices/flash.json), and that a product's `shards`
        partition its build_envs exactly with one PLATFORMIO_CORE_DIR class
        per shard. Exit 1 with every problem listed. Run from lint.yml.

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

# The workflows that must DERIVE the display env list from this script rather
# than type it. Each used to carry its own copy, and the two disagreed.
RELEASE_WORKFLOWS = ("firmware-release.yml", "flasher-release.yml")
RELEASE_PRODUCT = "canary-display"

DEFAULT_CORE = "default"
ISOLATED_CORE = "isolated"

# A shard label names a job leg ("PlatformIO Build (<product>/<label>)") and a
# cache key component (<product>-shard-<label>); keep it to what both accept.
SHARD_LABEL_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
GUARD_BIN_RE = re.compile(r"^\.pio/build/([^/]+)/")


# The file the current run actually read — `--flavors PATH` (flasher-release's
# overlay) must be what the error messages name, not the default.
_READ_FROM: Path = FLAVORS


def _source() -> str:
    try:
        return str(_READ_FROM.relative_to(REPO))
    except ValueError:
        return str(_READ_FROM)


def load_flavors(path: Path = FLAVORS) -> list[dict]:
    global _READ_FROM
    _READ_FROM = path
    return json.loads(path.read_text(encoding="utf-8"))


def find_product(flavors: list[dict], name: str) -> dict:
    for entry in flavors:
        if entry.get("name") == name:
            return entry
    known = ", ".join(e.get("name", "?") for e in flavors)
    raise SystemExit(
        f"flavor_envs.py: no product named '{name}' in "
        f"{_source()} (known: {known})"
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


def shards_of(entry: dict) -> dict[str, list[str]] | None:
    """The product's `shards` — label -> env list, in manifest order — or None
    when the product builds as one leg."""
    shards = entry.get("shards")
    if shards is None:
        return None
    return {str(label): list(envs or []) for label, envs in shards.items()}


def guard_env(guard: dict) -> str | None:
    """The env whose build dir a size guard's bin sits in (.pio/build/<env>/)."""
    m = GUARD_BIN_RE.match(str(guard.get("bin", "")))
    return m.group(1) if m else None


def build_legs(entry: dict) -> list[dict]:
    """firmware.yml's `build-platformio` matrix entries for one product.

    Without `shards`: one leg — the manifest entry plus `leg`, `shard` and
    `cache_name`, so the workflow reads the same fields for every product and
    the cache key of an unsharded product is byte-identical to what it was.
    With `shards`: one leg per shard, `build_envs` narrowed to that shard's
    envs in BUILD_ENVS order (the order the single job always built them in,
    which is also what keeps a shard on one PLATFORMIO_CORE_DIR — validate()
    refuses a shard that mixes classes), and `size_guards` narrowed to the
    bins those envs produce: a guard fires right after its env in the build
    loop, so a guard for another leg's env could never fire here anyway, and
    narrowing keeps each leg's FLAVOR_JSON honest about what it checks.
    """
    name = str(entry.get("name"))
    shards = shards_of(entry)
    if shards is None:
        return [dict(entry, leg=name, shard="", cache_name=name)]
    build = list(entry.get("build_envs") or [])
    legs: list[dict] = []
    for label, envs in shards.items():
        chosen = [env for env in build if env in envs]
        guards = [g for g in (entry.get("size_guards") or [])
                  if guard_env(g) in chosen]
        leg = dict(entry, build_envs=chosen, size_guards=guards,
                   leg=f"{name}/{label}", shard=label,
                   cache_name=f"{name}-shard-{label}")
        leg.pop("shards", None)
        legs.append(leg)
    return legs


def build_matrix(flavors: list[dict]) -> list[dict]:
    """Every product's legs, in manifest order — the whole build matrix."""
    return [leg for entry in flavors for leg in build_legs(entry)]


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
            f"`release_envs` in {_source()} — add the subset "
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
        shards = shards_of(entry)
        if shards is not None:
            home: dict[str, str] = {}
            for label, envs in shards.items():
                if not SHARD_LABEL_RE.match(label):
                    problems.append(f"{name}: shard label '{label}' must match "
                                    f"[a-z0-9][a-z0-9-]* — it names a job leg "
                                    f"and a cache key")
                if not envs:
                    problems.append(f"{name}: shard '{label}' is empty")
                classes = sorted({core_of(entry, e) for e in envs if e in build_set})
                if len(classes) > 1:
                    problems.append(
                        f"{name}: shard '{label}' mixes PLATFORMIO_CORE_DIR "
                        f"classes {classes} — one shard, one toolchain "
                        f"download; split it along the core_dir_groups / "
                        f"isolated_core_envs line")
                for env in envs:
                    if env not in build_set:
                        problems.append(f"{name}: shard '{label}' names '{env}', "
                                        f"which is not in build_envs")
                    elif env in home:
                        problems.append(f"{name}: '{env}' is in shards "
                                        f"'{home[env]}' and '{label}' — an env "
                                        f"builds in exactly one leg")
                    else:
                        home[env] = label
            missing = [env for env in build if env not in home]
            if missing:
                problems.append(
                    f"{name}: build env(s) {', '.join(missing)} are in no shard "
                    f"— once `shards` is declared every build_env must land in "
                    f"exactly one leg, or CI quietly stops compiling it")
            # build_legs() narrows size_guards to the envs of each leg by the
            # env in the guard's `bin` path; a guard whose bin does not parse
            # (or names an env outside build_envs) would land in NO leg and
            # silently stop running. Refuse that here, where the shards are.
            for guard in entry.get("size_guards") or []:
                genv = guard_env(guard)
                if genv is None or genv not in build_set:
                    problems.append(
                        f"{name}: size_guard bin '{guard.get('bin')}' does not sit "
                        f"under .pio/build/<env>/ for an env in build_envs — with "
                        f"`shards` declared it would be dropped from every leg")
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


def check_release_workflows(workflows: Path = WORKFLOWS,
                            names=RELEASE_WORKFLOWS, product=RELEASE_PRODUCT) -> list[str]:
    """The release workflows get the display env list from this script.

    Two shape rules per workflow, both about the list being typed ONCE:
      • it runs `flavor_envs.py … <product> … --release` (the resolve step);
      • it has no literal `pio run -e <product>-<env>` — a build line that
        names an env is the typed list creeping back, one env at a time.
    """
    problems: list[str] = []
    derive_re = re.compile(rf"flavor_envs\.py\b.*\b{re.escape(product)}\b.*--release\b")
    typed_re = re.compile(rf"pio\s+run\s+-e\s+[\"']?{re.escape(product)}-[a-z0-9][a-z0-9-]*")
    for name in names:
        path = workflows / name
        if not path.exists():
            problems.append(f"{name}: not found under {workflows} — the release workflows "
                            f"named in RELEASE_WORKFLOWS must exist")
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        if not any(derive_re.search(line) for line in lines):
            problems.append(
                f"{name}: does not derive its {product} env list from "
                f"`flavor_envs.py {product} --release` — the list must come from "
                f"firmware/flavors.json, not be typed into the workflow")
        for lineno, line in enumerate(lines, 1):
            if typed_re.search(line):
                problems.append(
                    f"{name}:{lineno}: builds a literal {product} env ({line.strip()[:60]}…) — "
                    f"iterate the list `flavor_envs.py {product} --release --json` emits "
                    f"instead of naming envs")
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
    ap.add_argument("--json", action="store_true",
                    help="print the list as one JSON array of {env, short, core}")
    ap.add_argument("--flavors", type=Path, default=FLAVORS, metavar="PATH",
                    help="read this flavors.json instead of firmware/flavors.json")
    ap.add_argument("--check-workflows", action="store_true",
                    help="lint: workflows name only envs flavors.json declares")
    ap.add_argument("--build-matrix", action="store_true",
                    help="print firmware.yml's build-platformio matrix (shards "
                         "expanded) as one line of JSON")
    args = ap.parse_args(argv)

    flavors = load_flavors(args.flavors)

    if args.build_matrix:
        problems = validate(flavors)
        if problems:
            for p in problems:
                print(f"::error::{p}", file=sys.stderr)
            return 1
        print(json.dumps(build_matrix(flavors), separators=(",", ":")))
        return 0

    if args.check_workflows:
        problems = (validate(flavors) + check_workflows(flavors)
                    + check_release_workflows(WORKFLOWS))
        if problems:
            for p in problems:
                print(f"::error::{p}")
            print(f"\n{len(problems)} problem(s): every display env a workflow "
                  f"names must exist in firmware/flavors.json, the release "
                  f"workflows must derive the list from this script, and the "
                  f"release set must be a subset of what CI builds.")
            return 1
        n = sum(len(e.get("release_envs") or []) for e in flavors)
        legs = len(build_matrix(flavors))
        print(f"flavor_envs.py --check-workflows: OK — workflows name only "
              f"declared envs and the release workflows derive the list; {n} "
              f"release env(s) all in build_envs with a flasher product; "
              f"{len(flavors)} product(s) build as {legs} matrix leg(s).")
        return 0

    if not args.product:
        ap.error("a product name is required unless --check-workflows or "
                 "--build-matrix is given")
    entry = find_product(flavors, args.product)
    envs = ordered_release_envs(entry) if args.release else list(entry.get("build_envs") or [])
    prefix = f"{args.product}-"
    if args.json:
        rows = [{"env": env,
                 "short": env[len(prefix):] if env.startswith(prefix) else env,
                 "core": core_of(entry, env)} for env in envs]
        print(json.dumps(rows, separators=(",", ":")))
        return 0
    for env in envs:
        shown = env[len(prefix):] if args.short and env.startswith(prefix) else env
        print(f"{shown} {core_of(entry, env)}" if args.with_core else shown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
