#!/usr/bin/env python3
"""scripts/check_firmware_advisories.py — published advisories against the
firmware's exact third-party versions.

Dependabot has no PlatformIO or Arduino ecosystem, and audit.yml's other jobs
read Cargo and npm lockfiles only, so nothing used to look at the libraries a
Canary image links (NimBLE-Arduino parses every BLE advertisement in radio
range, PubSubClient every MQTT packet from the broker). OSV and the GitHub
Advisory Database have no ecosystem for Arduino C/C++ libraries or ESP-IDF,
so a scanner fed the SBOM's purls would match nothing and pass. This check
reads the upstreams' own records instead:

  1. sbom/sbom-firmware.cdx.json (scripts/gen_firmware_sbom.py) is the list
     of what the firmware is built from. Every third-party component in it
     (group other than `securacv`) must carry an exact `version`, and its
     name must map to an upstream repository in UPSTREAM below. Either gap
     fails the check, so it cannot pass on a component it did not compare.
     The one exception is an Arduino core with no version that only CI rows
     outside RELEASE_WORKFLOWS build (firmware.yml's PR row on the action's
     "latest", the early warning for the next core): it ships in nothing and
     is listed as not checked.
  2. For each upstream repository, its published GitHub repository security
     advisories (`GET /repos/{owner}/{repo}/security-advisories`), compared
     with every pinned version mapped to it. An affected range that matches
     fails the check. A range this script cannot read (missing, or not the
     `>= a, < b` form) fails it too, until a reviewer records in REVIEWED why
     the advisory does not apply to the pinned versions.

What it does NOT cover, said where the output is read:
  - advisories that are not GitHub repository advisories of these repos:
    Espressif publishes ESP-IDF and chip advisories on its own site, and a
    CVE filed only with NVD has no repository advisory;
  - the libraries ESP-IDF bundles (mbedTLS, lwIP, FreeRTOS, cJSON): the SBOM
    does not enumerate them, so nothing here compares their versions;
  - transitive dependencies of the libraries, which the SBOM does not list.

No secret is needed: the job's own `GITHUB_TOKEN` (GH_TOKEN) is sent when the
environment has one, for the rate limit; GitHub's REST docs list this endpoint
as readable without authentication for a public repository.

Usage:
    python3 scripts/check_firmware_advisories.py              # query and compare
    python3 scripts/check_firmware_advisories.py --coverage   # step 1 only, offline

Exit 0: every third-party component was compared and none is affected.
Exit 1: an affected version, a coverage gap, or an advisory nobody has read.
Exit 2: an upstream could not be queried (so nothing was compared for it).
Tests: scripts/tests/test_check_firmware_advisories.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SBOM = REPO / "sbom" / "sbom-firmware.cdx.json"
API = "https://api.github.com"

# The workflows whose Arduino-CLI rows build an image that ships; the same
# tuple as scripts/gen_firmware_sbom.py's (the unit tests hold them equal —
# this script reads only the SBOM, so it does not import the generator).
RELEASE_WORKFLOWS = ("firmware-release.yml", "flasher-release.yml")

# Component name (as the SBOM writes it) → the GitHub repository whose
# security advisories cover it. A third-party component whose name is not
# here fails the check: add the row in the change that adds the library.
UPSTREAM: dict[str, str] = {
    "arduino-esp32": "espressif/arduino-esp32",
    "esp-idf": "espressif/esp-idf",
    "NimBLE-Arduino": "h2zero/NimBLE-Arduino",
    "ArduinoJson": "bblanchon/ArduinoJson",
    "PubSubClient": "knolleary/pubsubclient",
    # Crypto is one library of rweather/arduinolibs; every advisory there is
    # compared with it, whichever library of the repo it names.
    "Crypto": "rweather/arduinolibs",
    "lvgl": "lvgl/lvgl",
    "GFX Library for Arduino": "moononournation/Arduino_GFX",
    "Seeed_Arduino_SSCMA": "Seeed-Studio/Seeed_Arduino_SSCMA",
    "platformio/espressif32": "platformio/platform-espressif32",
    "pioarduino/platform-espressif32": "pioarduino/platform-espressif32",
}

# GHSA id → why it does not affect the pinned versions, for an advisory whose
# range this script cannot read or that a reviewer has ruled out. An entry is
# a reviewed claim about the versions pinned today; remove it when a pin moves.
REVIEWED: dict[str, str] = {}

_CONSTRAINT_RE = re.compile(r"^(>=|<=|==|=|>|<)?\s*v?(\d+(?:[.-]\d+)*)$")


def version_key(text: str) -> tuple[int, ...] | None:
    """`5.5.4` / `v5.5.4` / `55.03.38-1` → a comparable tuple (trailing zeros
    dropped, so 2.8 == 2.8.0); anything else (a pre-release tag) → None."""
    m = re.fullmatch(r"v?(\d+(?:[.-]\d+)*)", text.strip())
    if not m:
        return None
    parts = [int(p) for p in re.split(r"[.-]", m.group(1))]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def parse_range(text: str | None) -> list[tuple[str, tuple[int, ...]]] | None:
    """GitHub's `vulnerable_version_range` (`>= 1.0.0, < 1.4.3`, `<= 2.5.1`,
    `= 7.0.0`) → [(op, version)]; None when it is missing or not that form."""
    if not text or not text.strip():
        return None
    out: list[tuple[str, tuple[int, ...]]] = []
    for part in text.split(","):
        m = _CONSTRAINT_RE.match(part.strip())
        if not m:
            return None
        key = version_key(m.group(2))
        if key is None:
            return None
        out.append(({"==": "=", None: "="}.get(m.group(1), m.group(1)), key))
    return out


def in_range(version: tuple[int, ...], constraints: list[tuple[str, tuple[int, ...]]]) -> bool:
    def cmp(a: tuple[int, ...], b: tuple[int, ...]) -> int:
        n = max(len(a), len(b))
        a, b = a + (0,) * (n - len(a)), b + (0,) * (n - len(b))
        return (a > b) - (a < b)
    ops = {"=": lambda c: c == 0, ">": lambda c: c > 0, ">=": lambda c: c >= 0,
           "<": lambda c: c < 0, "<=": lambda c: c <= 0}
    return all(ops[op](cmp(version, bound)) for op, bound in constraints)


def _prop(component: dict, name: str) -> str:
    return next((p["value"] for p in component.get("properties") or []
                 if p.get("name") == name), "")


def third_party(doc: dict) -> list[dict]:
    """The components not built from this repository's own sources."""
    return [c for c in doc.get("components") or [] if c.get("group") != "securacv"]


def _floats_on_ci_only(component: dict) -> bool:
    """An Arduino core with no version that only non-release CI rows build."""
    if component.get("name") != "arduino-esp32" or component.get("version"):
        return False
    paths = [p.strip() for p in _prop(component, "securacv:build_paths").split(";") if p.strip()]
    rows = [re.fullmatch(r"arduino-cli \(([^:()]+):[^()]+\)", p) for p in paths]
    return bool(rows) and all(r and r.group(1) not in RELEASE_WORKFLOWS for r in rows)


def coverage(doc: dict) -> tuple[list[dict], list[str], list[str]]:
    """(the components to compare, the gaps that fail, the not-checked notes)."""
    checked: list[dict] = []
    gaps: list[str] = []
    skipped: list[str] = []
    for c in third_party(doc):
        ref = c.get("bom-ref", c.get("name"))
        if _floats_on_ci_only(c):
            skipped.append(f"{ref}: the action's latest core on CI-only rows "
                           f"({_prop(c, 'securacv:build_paths')}); ships in no image")
            continue
        version = str(c.get("version") or "")
        if not version or version_key(version) is None:
            spec = _prop(c, "securacv:version_spec") or "(none)"
            gaps.append(f"{ref} has no exact version (spec {spec}, used by "
                        f"{_prop(c, 'securacv:envs') or _prop(c, 'securacv:build_paths') or '?'}"
                        f"): pin it, or its advisories cannot be compared")
            continue
        if c.get("name") not in UPSTREAM:
            gaps.append(f"{ref}: no upstream repository for {c.get('name')!r} in "
                        f"scripts/check_firmware_advisories.py UPSTREAM")
            continue
        checked.append(c)
    return checked, gaps, skipped


def fetch_advisories(repo: str, token: str | None = None) -> list[dict]:
    """Every published repository security advisory of `repo` (all pages)."""
    url: str | None = f"{API}/repos/{repo}/security-advisories?state=published&per_page=100"
    out: list[dict] = []
    while url:
        req = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "securacv-firmware-advisories",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        })
        with urllib.request.urlopen(req, timeout=30) as resp:  # noqa: S310 — fixed https host
            page = json.loads(resp.read().decode("utf-8"))
            link = resp.headers.get("Link") or ""
        if not isinstance(page, list):
            raise ValueError(f"{repo}: expected a list of advisories, got {type(page).__name__}")
        out.extend(page)
        nxt = re.search(r'<([^>]+)>;\s*rel="next"', link)
        url = nxt.group(1) if nxt else None
    return out


def compare(components: list[dict], advisories: dict[str, list[dict]]) -> tuple[list[str], list[str]]:
    """(affected, unreadable) findings for every component against its
    upstream's advisories. A withdrawn advisory is skipped; a REVIEWED one is
    skipped with its reason printed by the caller."""
    affected: list[str] = []
    unreadable: list[str] = []
    for c in components:
        repo = UPSTREAM[c["name"]]
        ver = version_key(str(c["version"]))
        for adv in advisories.get(repo, []):
            ghsa = adv.get("ghsa_id") or adv.get("html_url") or "?"
            if adv.get("withdrawn_at") or ghsa in REVIEWED:
                continue
            title = f"{ghsa} ({adv.get('severity') or 'severity unset'}: {adv.get('summary') or ''})"
            vulns = adv.get("vulnerabilities") or []
            if not vulns:
                unreadable.append(f"{c['bom-ref']} {c['version']}: {repo} {title} names no "
                                  f"affected range — read it and record the verdict in REVIEWED")
                continue
            for v in vulns:
                pkg = ((v.get("package") or {}).get("name")) or "?"
                rng = parse_range(v.get("vulnerable_version_range"))
                if rng is None:
                    unreadable.append(f"{c['bom-ref']} {c['version']}: {repo} {title} package "
                                      f"{pkg} range {v.get('vulnerable_version_range')!r} is not "
                                      f"machine-readable — read it and record the verdict in "
                                      f"REVIEWED")
                elif in_range(ver, rng):
                    affected.append(f"{c['bom-ref']} {c['version']} is in {repo} {title} "
                                    f"package {pkg} range "
                                    f"{v.get('vulnerable_version_range')!r}; patched: "
                                    f"{v.get('patched_versions') or 'none listed'}")
    return affected, unreadable


NOT_COVERED = (
    "Not covered: advisories that are not GitHub repository advisories of the "
    "repos above (Espressif's own advisory page; a CVE with no repository "
    "advisory); the libraries ESP-IDF bundles (mbedTLS, lwIP, FreeRTOS, cJSON — "
    "not enumerated in the SBOM); transitive dependencies of the libraries."
)


def main(argv: list[str] | None = None, fetch=fetch_advisories) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sbom", type=Path, default=SBOM, help="the firmware SBOM to read")
    ap.add_argument("--coverage", action="store_true",
                    help="only the coverage step (exact versions, known upstreams); no network")
    args = ap.parse_args(argv)
    try:
        doc = json.loads(args.sbom.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"::error::check_firmware_advisories.py: cannot read {args.sbom}: {exc}")
        return 1
    checked, gaps, skipped = coverage(doc)
    for note in skipped:
        print(f"not checked: {note}")
    for gap in gaps:
        print(f"::error::coverage: {gap}")
    if args.coverage:
        print(f"check_firmware_advisories.py --coverage: {len(checked)} third-party "
              f"component(s) comparable, {len(gaps)} gap(s)")
        return 1 if gaps else 0

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or None
    repos = sorted({UPSTREAM[c["name"]] for c in checked})
    advisories: dict[str, list[dict]] = {}
    failed: list[str] = []
    for repo in repos:
        try:
            advisories[repo] = fetch(repo, token)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            failed.append(f"{repo}: {exc}")
    for f in failed:
        print(f"::error::could not read the advisories of {f} — nothing was compared for it")
    affected, unreadable = compare(checked, advisories)
    for line in affected:
        print(f"::error::affected: {line}")
    for line in unreadable:
        print(f"::error::unread: {line}")
    for ghsa, why in sorted(REVIEWED.items()):
        print(f"reviewed, not counted: {ghsa} — {why}")
    print(f"check_firmware_advisories.py: {len(checked)} third-party component(s) at an "
          f"exact version, {len(repos)} upstream repo(s), "
          f"{sum(len(v) for v in advisories.values())} published advisory(ies) read; "
          f"{len(affected)} affected, {len(unreadable)} unread, {len(gaps)} coverage gap(s), "
          f"{len(failed)} repo(s) unreachable.")
    print(NOT_COVERED)
    if failed:
        return 2
    return 1 if (affected or unreadable or gaps) else 0


if __name__ == "__main__":
    sys.exit(main())
