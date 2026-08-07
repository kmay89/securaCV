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
semantics as the PR gate — unguarded envs pass; the absence prints a note so
a log reader can tell "checked" from "skipped"). Exit 1: the file is missing
or the image exceeds the budget — the caller decides whether that sinks the
release (flagship products) or skips the variant (the per-variant loops).
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


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--env", required=True,
                    help="the PlatformIO env that produced (or in PR CI would "
                         "produce) this image, e.g. canary-display-nightstand-c6")
    ap.add_argument("bin", type=pathlib.Path, help="the built image to measure")
    args = ap.parse_args()

    guard = load_budgets().get(args.env)
    if guard is None:
        print(f"check_slot_budget: no size_guards budget declared for env "
              f"'{args.env}' — not checked (declare one in firmware/flavors.json "
              f"if this product OTA-updates into a bounded slot).")
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


if __name__ == "__main__":
    main()
