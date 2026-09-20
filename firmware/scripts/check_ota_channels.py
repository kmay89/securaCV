#!/usr/bin/env python3
"""Every OTA manifest the firmware POLLS must be one the release PUBLISHES.

The gap this closes, found the hard way: the Canary Display flavors were fully
wired into the browser flasher (flash.json, build_flash_manifest.py, the release
workflows' build steps) and each flavor's config.h pointed
SECURACV_OTA_MANIFEST_URL at `manifest-canary-display-<flavor>.json` — but
firmware-release.yml never *signed* such a manifest. The displays were
USB-flashable and structurally unable to self-update, and nothing failed: the
device just fetches a 404 forever, quietly. A release has to be cut before you
can see it, and even then only by looking.

So the check is static and offline. It reads both sides of the contract:

  polled     — every SECURACV_OTA_MANIFEST_URL literal in the firmware tree
  published  — every manifest firmware-release.yml writes, including the ones
               its display loop generates: the literals in the loop header
               plus the release envs the workflow derives from
               firmware/flavors.json (the same resolver, flavor_envs.py)

and fails when a polled manifest is neither published nor explicitly declared
as having no channel yet. Declaring is the escape hatch (UNPUBLISHED below) —
it costs one line and a reason, which is the point: a flavor without a channel
becomes a decision someone wrote down, not an accident nobody noticed.

Run locally:  python3 firmware/scripts/check_ota_channels.py
CI:           firmware.yml → "Regression Guards"
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = REPO / ".github/workflows/firmware-release.yml"

# Flavors whose OTA URL is compiled in but which no release publishes yet.
# Each needs a reason. Deleting an entry is how you turn a channel on; adding
# one is how you say "not yet, and here's why" — either way it's deliberate.
UNPUBLISHED: dict[str, str] = {
    "manifest-canary-display-watch-modes.json":
        "no sketch.yaml profile — the gear/modes identity ships on dash only",
    "manifest-canary-display-nightstand.json":
        "Arduino-path fallback; the two nightstand boards take board-specific "
        "products from their PlatformIO envs, which CI does not build for release",
    "manifest-canary-display.json":
        "bare-flavor fallback for an un-flavored display build; no release target",
    # PlatformIO-only display env with no release build target. The Nightstand
    # Line boards (dash7 / nightstand-s3 / nightstand-c6) graduated: the
    # release now runs `pio run` for their envs and stages the binaries into
    # the display signing loop, so their manifests moved to the published set.
    # The mic-bearing 4.3C stays deliberate: a distinct privacy surface earns
    # its channel only with a bench pass (docs/hardware/display_mic_variant.md).
    "manifest-canary-display-dash-mic.json":
        "canary-display-dash-mic env (4.3C + ES7210 mic) — PlatformIO-only, no "
        "sketch.yaml profile and no release build target",
    # Phase 0 reach ports (docs/strategy/30): compile-tested tier, no
    # hardware validation yet. A release channel is a promise the images
    # boot; these earn theirs with a Hardware Test Report, same gate that
    # promotes the board tier. Until then pull-OTA on these boards fails
    # closed (404 → no update), which is the intended posture.
    "manifest-canary-esp32cam.json":
        "esp32cam env (AI-Thinker ESP32-CAM) — compile-tested port, no bench "
        "validation yet; channel turns on with a Hardware Test Report",
    "manifest-canary-wroom.json":
        "esp32-wroom env (WROOM-32 DevKit family) — compile-tested port, no "
        "bench validation yet; channel turns on with a Hardware Test Report",
    "manifest-canary-freenove-s3.json":
        "freenove-s3 env (Freenove FNK0085) — compile-tested port, no bench "
        "validation yet; channel turns on with a Hardware Test Report",
    "manifest-canary-vision-c3-super-mini.json":
        "canary-vision-c3-super-mini env (C3 Super Mini) — compile-tested "
        "port, no bench validation yet; channel turns on with a Hardware "
        "Test Report",
}


# The define, then only whitespace / line-continuations, then the string it
# expands to. Anchoring on the quote is what keeps `#ifndef
# SECURACV_OTA_MANIFEST_URL` from swallowing the `#define` on the next line —
# an earlier version did exactly that and silently checked a third of the tree.
OTA_URL_DEFINE = re.compile(r'SECURACV_OTA_MANIFEST_URL[\s\\]*=?[\s\\]*\\?"([^"]*)"')
MANIFEST_IN_URL = re.compile(r"/(manifest-[A-Za-z0-9._-]+\.json)")

# BOTH ways a device learns its manifest URL, because checking only one is how
# this guard shipped with a blind spot the first time: the C/C++ `#define`
# fallbacks, AND the PlatformIO `-DSECURACV_OTA_MANIFEST_URL="…"` build flags in
# the env files. The per-board envs are precisely where the exotic flavors live
# (nightstand-s3, dash7, dash-mic), so an .ini-only URL is the likeliest kind to
# have no channel — exactly what must not slip through.
SOURCE_GLOBS = (
    "firmware/**/*.h",
    "firmware/**/*.ino",
    "firmware/**/*.cpp",
    "firmware/**/*.ini",
)


def polled_manifests() -> dict[str, list[str]]:
    """manifest filename -> the sources that point a device at it."""
    found: dict[str, list[str]] = {}
    for pattern in SOURCE_GLOBS:
        for src in sorted(REPO.glob(pattern)):
            text = src.read_text(encoding="utf-8", errors="replace")
            if "SECURACV_OTA_MANIFEST_URL" not in text:
                continue
            for url in OTA_URL_DEFINE.findall(text):
                for name in MANIFEST_IN_URL.findall(url):
                    found.setdefault(name, []).append(str(src.relative_to(REPO)))
    return found


def published_manifests(workflow: Path = RELEASE_WORKFLOW) -> set[str]:
    """Every manifest firmware-release.yml publishes for the release.

    Read from the **variant index** rather than from the `--out` paths. Several
    of those paths are shell templates (`manifest-canary-vision${SUFFIX}.json`)
    that no regex should pretend to expand, whereas the index enumerates one
    literal `product=<url>/manifest-*.json` per shipped variant — and the index
    IS the release's declared product catalog, so it is the honest side of the
    contract to check against.
    """
    text = workflow.read_text(encoding="utf-8")
    names = set(
        re.findall(r'=\$\{DL\}/(manifest-[A-Za-z0-9._-]+\.json)"', text)
    )

    # The display flavors are appended to the index at runtime ($DISPLAY_INDEX),
    # so take them from the loop header that drives it. That header holds two
    # kinds of entry: quoted "<name>:<dir>" literals (the Arduino-built
    # flavors) and, since the release envs stopped being typed into the
    # workflow, one `$(jq … "$DISPLAY_ENVS")` expansion of the list the
    # workflow derives from firmware/flavors.json. Read both through the same
    # code the workflow runs, so a flavor added to or removed from either
    # place moves this set with it — and refuse to guess if the loop is gone.
    loops = re.findall(r"for D in (.+?); do", text)
    if len(loops) != 1:
        raise SystemExit(
            f"check_ota_channels.py: expected exactly one `for D in …; do` "
            f"display signing loop in {workflow}, "
            f"found {len(loops)} — update published_manifests() to match the "
            f"workflow's shape rather than letting every display flavor read "
            f"as unpublished."
        )
    header = loops[0]
    for flavor in re.findall(r'"([a-z0-9-]+):[a-z0-9-]+"', header):
        names.add(f"manifest-canary-display-{flavor}.json")
    if "DISPLAY_ENVS" in header:
        for short in derived_display_release_shorts():
            names.add(f"manifest-canary-display-{short}.json")
    return names


def derived_display_release_shorts() -> list[str]:
    """The Canary Display release envs the workflow resolves at run time,
    as the short names its staging loops key on (`canary-display-dash7` →
    `dash7`), from the same resolver: flavor_envs.py canary-display --release.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import flavor_envs  # noqa: E402  (sibling script, not a package)

    product = "canary-display"
    entry = flavor_envs.find_product(flavor_envs.load_flavors(), product)
    prefix = f"{product}-"
    return [env[len(prefix):] if env.startswith(prefix) else env
            for env in flavor_envs.ordered_release_envs(entry)]


def main() -> int:
    polled = polled_manifests()
    published = published_manifests()

    if not polled:
        print("::error::found no SECURACV_OTA_MANIFEST_URL in the firmware tree "
              "— this guard is looking in the wrong place, not passing honestly.")
        return 1
    if not published:
        print(f"::error::parsed no published manifests out of "
              f"{RELEASE_WORKFLOW.relative_to(REPO)} — this guard cannot see the "
              f"release's output, so it is not proving anything.")
        return 1

    problems: list[str] = []
    for name, headers in sorted(polled.items()):
        if name in published:
            continue
        if name in UNPUBLISHED:
            print(f"declared unpublished: {name} — {UNPUBLISHED[name]}")
            continue
        problems.append(
            f"{name} is polled by {', '.join(sorted(set(headers)))} but no "
            f"release publishes it. Devices on that flavor fetch a 404 forever "
            f"and can never self-update. Either sign it in "
            f"firmware-release.yml, or declare it in UNPUBLISHED "
            f"(with a reason) in this script."
        )

    # A stale exception is its own bug: it reads as "no channel" long after the
    # channel exists, so the next person trusts a lie.
    for name in sorted(UNPUBLISHED):
        if name in published:
            problems.append(
                f"{name} is listed as UNPUBLISHED in this script but "
                f"firmware-release.yml does publish it — drop the stale entry."
            )
        elif name not in polled:
            problems.append(
                f"{name} is listed as UNPUBLISHED but nothing polls it — the "
                f"flavor is gone; drop the stale entry."
            )

    if problems:
        for p in problems:
            print(f"::error::{p}")
        return 1

    covered = sorted(set(polled) & published)
    print(f"OTA channels OK: {len(covered)} polled manifests are published "
          f"({', '.join(covered)}); {len(UNPUBLISHED)} declared unpublished.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
