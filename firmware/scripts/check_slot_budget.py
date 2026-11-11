#!/usr/bin/env python3
"""Byte-accurate OTA slot-budget check for one built firmware image.

The budgets live ONCE, in firmware/flavors.json `size_guards` — the same
entries the PR gate (.github/workflows/firmware.yml) walks after each env
builds. This script lets the RELEASE path (firmware-release.yml) enforce the
identical numbers against the artifact it is about to sign and publish: a
release is cut from a tag, not from the branch build, so without this a
manual dispatch or an unguarded env could still publish an OTA image no
fielded slot can hold — the exact shape of the fw-v2.4.6 nightstand-c6
incident (.github/RELEASE_LESSONS.md, 2026-08-07), where every gate that
passed was measuring something other than the published bytes.

Each size_guards entry names its bin as `.pio/build/<env>/firmware.bin`; the
env name is the key, so the check works on any *staged copy* of that env's
output (the release workflow stages into /tmp/release, and the watch/wap
images are arduino-cli builds of the same product an env guards in PR CI).

    python3 firmware/scripts/check_slot_budget.py --env canary-display-nightstand-c6 \
        /tmp/release/canary-display-nightstand-c6-2.4.6.bin

Exit 0: the image fits its declared budget, or the env declares none (same
semantics as the PR gate — unguarded envs pass). The absence is a GitHub
`::warning::` annotation, not a plain log line: the release workflow calls
this about to sign an OTA manifest, and the plain note let 9 of the 17
published OTA products (fw-v2.4.16) ship unmeasured with nothing on the
run's summary page to say so. Exit 1: the file is missing or the image
exceeds the budget — the caller decides whether that sinks the release
(flagship products) or skips the variant (the per-variant loops).

The headroom floor (F367): an image that fits but leaves less than
HEADROOM_FLOOR_PERCENT of its slot free is a `::warning::` naming the env,
its free bytes and the floor, and the exit status does not move (it still
fits, so it still passes; over the slot still fails). fw-v2.4.16's
canary-display-nightstand-c6 fit its 2,031,616-byte slot with 2,832 bytes
to spare and nothing said so; one more feature and the release's display
loop would have skipped its OTA manifest with no earlier sign. The warning
is that earlier sign, in the PR gate and on the release run alike.

The PR gate (.github/workflows/firmware.yml's size_guard) calls this too,
with --require-budget: it only calls for an env whose guard it already
holds, so an env this script cannot find a budget for is a disagreement
between the two readers of flavors.json, and it fails there rather than
passing as unmeasured.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
FLAVORS = REPO_ROOT / "firmware" / "flavors.json"
ENV_RE = re.compile(r"^\.pio/build/([^/]+)/")

# The headroom floor, as a whole-number percentage of the slot that should
# stay free. Below it a fitting image is warned about, never failed: the
# decision about what to drop is a maintainer's, and the warning is there so
# it is made before a release skips a product rather than after.
HEADROOM_FLOOR_PERCENT = 5


def floor_bytes(slot: int) -> int:
    """The fewest free bytes that clear the floor: HEADROOM_FLOOR_PERCENT of
    the slot, rounded UP to a whole byte, in integer arithmetic (no float
    can round a slot's 5% the wrong way). A slot whose 5% is not a whole
    byte (2,031,616 B: 101,580.8) needs the next byte free to clear it, so
    `free < floor_bytes(slot)` is exactly `free * 100 < slot * percent`."""
    return -(-slot * HEADROOM_FLOOR_PERCENT // 100)


def load_budgets() -> dict[str, dict]:
    budgets: dict[str, dict] = {}
    for flavor in json.loads(FLAVORS.read_text(encoding="utf-8")):
        for guard in flavor.get("size_guards", []):
            m = ENV_RE.match(guard["bin"])
            if not m:
                continue
            env = m.group(1)
            if env in budgets and budgets[env] != guard:
                sys.exit(f"check_slot_budget: env '{env}' has conflicting "
                         f"size_guards entries in {FLAVORS} — fix the manifest.")
            budgets[env] = guard
    return budgets


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--env", required=True,
                    help="the PlatformIO env that produced (or in PR CI would "
                         "produce) this image, e.g. canary-display-nightstand-c6")
    ap.add_argument("bin", type=pathlib.Path, help="the built image to measure")
    ap.add_argument("--require-budget", action="store_true",
                    help="fail (exit 1) when the env declares no budget, "
                         "instead of the release path's warning; the PR gate "
                         "passes it, since it only asks about envs it holds "
                         "a guard for")
    args = ap.parse_args(argv)

    guard = load_budgets().get(args.env)
    if guard is None and args.require_budget:
        sys.exit(f"check_slot_budget: no size_guards budget declared for env "
                 f"'{args.env}' in {FLAVORS}, but the caller holds a guard "
                 f"for it — the two readers of flavors.json disagree, so "
                 f"this image was NOT measured.")
    if guard is None:
        # Non-fatal on purpose (an unguarded env passes, as in the PR gate),
        # but an annotation, so "skipped" reads differently from "checked".
        print(f"::warning::check_slot_budget: no size_guards budget declared "
              f"for env '{args.env}' — this OTA image was NOT measured against "
              f"any slot. Declare one in firmware/flavors.json: an image larger "
              f"than the fielded boards' slot cannot install on any of them.")
        return
    if not args.bin.is_file():
        sys.exit(f"check_slot_budget: {args.bin} not found — cannot measure it.")

    size = args.bin.stat().st_size
    slot = int(guard["slot_bytes"])
    name = guard.get("slot_name", "OTA slot")
    if size > slot:
        sys.exit(f"check_slot_budget: {args.bin} is {size} bytes but the "
                 f"{name} holds {slot} — no fielded device could install it "
                 f"(the OTA write fails on every board). Trim features or "
                 f"grow the slots (firmware/PARTITIONS.md).")
    print(f"Firmware size: {size} / {slot} bytes ({size * 100 // slot}% of {name})")
    free = slot - size
    floor = floor_bytes(slot)
    if free < floor:
        # One line, so GitHub reads it as one annotation; the exit stays 0.
        print(f"::warning::check_slot_budget: env '{args.env}' fits its "
              f"{name} with only {free} bytes free, under the "
              f"{HEADROOM_FLOOR_PERCENT} percent headroom floor of {floor} "
              f"bytes ({size} of {slot}). A build that grows it by more "
              f"than {free} bytes no longer fits the slot and cannot "
              f"install over the air: decide what it drops or puts behind "
              f"a flag now (its linker map, largest contributors first); a "
              f"larger table reaches only boards flashed over USB "
              f"(firmware/PARTITIONS.md).")


if __name__ == "__main__":
    main()
